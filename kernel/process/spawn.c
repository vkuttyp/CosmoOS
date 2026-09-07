/*
 * spawn.c - Create a process from an executable file on behalf of the
 * calling process (docs/kernel/process/design.md, "Phase 9 additions").
 *
 * The file is read through the VFS into a kernel buffer and handed to the
 * Phase 4 loader; nothing here maps user memory. The caller (the system
 * call layer) has copied every argument out of user space already.
 */

#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include <uapi/cosmo/syscall.h>

#define SPAWN_IMAGE_MAX (16u << 20)

static const char *basename_of(const char *path)
{
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1] != '\0')
            b = s + 1;
    return b;
}

/* The map names caller handles that exist and child slots that are valid and distinct. */
static int validate_handles(struct process *cur, const struct process_handle_map *map, unsigned n)
{
    if (n > HANDLE_TABLE_SIZE)
        return -EINVAL;
    uint64_t seen = 0;
    for (unsigned i = 0; i < n; i++) {
        int c = map[i].child;
        if (c < 0 || c >= HANDLE_TABLE_SIZE || (seen & (1ull << c)))
            return -EINVAL;
        seen |= 1ull << c;
        unsigned rights;
        struct kobject *obj = handle_get(&cur->handles, map[i].parent, &rights);
        if (obj == NULL)
            return -EBADF;
        kobject_put(obj);
    }
    return 0;
}

/* COSMO_SPAWN_SETCRED: a privileged caller names any ids; an unprivileged
 * one only ids among its own real, effective and saved set, the setresuid
 * rule (docs/kernel/security/design.md §1). */
static bool may_set_cred(const struct credentials *c, uint32_t uid, uint32_t gid)
{
    if (cred_privileged(c))
        return true;
    bool u = uid == c->ruid || uid == c->euid || uid == c->suid;
    bool g = gid == c->rgid || gid == c->egid || gid == c->sgid;
    return u && g;
}

/* Look `path` up from the caller's directory, require a regular file the
 * caller may execute, and read it whole into a kernel buffer the caller
 * frees with vm_kernel_free. */
static int read_executable(struct process *cur, const char *path, struct process_image *img)
{
    struct vnode *exe;
    int rc = vfs_lookup(cur->cwd, path, &exe);
    if (rc)
        return rc;
    if (exe->type != VNODE_REG) {
        vnode_put(exe);
        return -EACCES;
    }
    rc = vfs_permission(exe, VFS_MAY_EXEC);
    if (rc) {
        vnode_put(exe);
        return rc;
    }
    struct file *f;
    rc = vfs_open_vnode(exe, COSMO_O_RDONLY, &f);   /* consumes the reference */
    if (rc)
        return rc;
    struct cosmo_stat st;
    file_stat(f, &st);
    if (st.size == 0 || st.size > SPAWN_IMAGE_MAX) {
        file_put(f);
        return -ENOEXEC;
    }
    size_t size = (size_t)st.size;
    vaddr_t image = vm_kernel_alloc((size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1), VM_KALLOC_POPULATE,
                                    VM_PROT_RW);
    if (image == 0) {
        file_put(f);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < size) {
        int64_t n = file_pread(f, (uint8_t *)image + got, size - got, got);
        if (n <= 0) {
            vm_kernel_free(image);
            file_put(f);
            return n < 0 ? (int)n : -EIO;
        }
        got += (size_t)n;
    }
    file_put(f);
    img->data = (const void *)image;
    img->size = size;
    return 0;
}

int process_spawn(const char *path, const char *const argv[], const char *const envp[],
                  const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, const char *root,
                  const struct process_spawn_cred *cred, pid_t *pid_out)
{
    struct process *cur = process_current();
    KASSERT(cur != NULL);   /* a system call: always on a process */
    if (argv == NULL || argv[0] == NULL)
        return -EINVAL;
    int rc = validate_handles(cur, handles, nr_handles);
    if (rc)
        return rc;
    if (cred && !may_set_cred(&cur->cred, cred->uid, cred->gid))
        return -EPERM;

    struct process_spawn_attr attr = {
        .parent = cur,
        .handles = handles,
        .nr_handles = nr_handles,
        .set_cred = cred != NULL,
        .uid = cred ? cred->uid : 0,
        .gid = cred ? cred->gid : 0,
    };
    char cwd_path[sizeof(cur->cwd_path)];
    if (cwd) {
        rc = path_normalize(cur->cwd_path, cwd, cwd_path, sizeof(cwd_path));
        if (rc)
            return rc;
        struct vnode *vn;
        rc = vfs_lookup(cur->cwd, cwd, &vn);
        if (rc)
            return rc;
        if (vn->type != VNODE_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
        attr.cwd = vn;
        attr.cwd_path = cwd_path;
    }
    /*
     * The child's root, resolved in the caller's own namespace -- so a
     * caller already confined to a root can only hand its child a
     * directory inside that one, and confinement only ever tightens.
     * Privileged, like setting credentials: a process that could root
     * itself anywhere could root itself at a directory whose contents it
     * chose (docs/kernel/process/design.md, "Per-process roots").
     */
    if (root) {
        if (!cred_privileged(&cur->cred)) {
            rc = -EPERM;
            goto out_cwd;
        }
        /* A root and a working directory together would need the cwd
         * resolved in the child's namespace to be sure it lies inside
         * the root, and this call resolves paths in the caller's. Until
         * that exists, the two are not offered together: a rooted child
         * starts at its root. Refusing is better than resolving it in
         * the wrong namespace and hoping. */
        if (cwd) {
            rc = -EINVAL;
            goto out_cwd;
        }
        struct vnode *rv;
        rc = vfs_lookup(cur->cwd, root, &rv);
        if (rc)
            goto out_cwd;
        if (rv->type != VNODE_DIR) {
            vnode_put(rv);
            rc = -ENOTDIR;
            goto out_cwd;
        }
        attr.root = rv;
        /*
         * And the child starts *at* its root. Inheriting the parent's
         * working directory would leave it standing outside its own
         * root, where every relative path reaches outside it and ".."
         * climbs to the global root rather than stopping -- the
         * confinement would be bypassed by doing nothing at all.
         */
        vnode_get(rv);
        attr.cwd = rv;
        attr.cwd_path = "/";
    }

    /* The executable, and the interpreter it names (PT_INTERP), are found
     * through the caller's directories and checked for execute permission
     * with the caller's credentials (vfs_permission: root too needs an x
     * bit), then read by the kernel on its own authority: like Linux, exec
     * needs x, not r. */
    struct process_image exe = { .path = path }, interp = { 0 };
    rc = read_executable(cur, path, &exe);
    if (rc)
        goto out_cwd;
    struct elf_info peek;
    const char *why = NULL;
    rc = elf_validate(exe.data, exe.size, USER_LO, USER_HI, &peek, &why);
    if (rc) {
        kwarn("process: '%s' rejected: %s", basename_of(path), why ? why : "?");
        goto out_exe;
    }
    if (peek.has_interp) {
        interp.path = peek.interp;
        rc = read_executable(cur, peek.interp, &interp);
        if (rc) {
            kwarn("process: '%s': interpreter %s: %d", basename_of(path), peek.interp, rc);
            goto out_exe;
        }
    }

    struct process *p = NULL;
    rc = process_create_from_images(&exe, peek.has_interp ? &interp : NULL, basename_of(path), argv, envp, &attr, &p);
    if (rc == 0) {
        *pid_out = p->pid;
        process_put(p);   /* the creator's reference; the table and the thread hold theirs */
    }
    if (interp.data)
        vm_kernel_free((vaddr_t)interp.data);
out_exe:
    vm_kernel_free((vaddr_t)exe.data);
out_cwd:
    if (attr.cwd)
        vnode_put(attr.cwd);
    if (attr.root)
        vnode_put(attr.root);   /* the child took its own reference */
    return rc;
}
