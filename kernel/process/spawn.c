/*
 * spawn.c - Create a process from an executable file on behalf of the
 * calling process (docs/kernel/process/design.md, "Phase 9 additions").
 *
 * The file is read through the VFS into a kernel buffer and handed to the
 * Phase 4 loader; nothing here maps user memory. The caller (the system
 * call layer) has copied every argument out of user space already.
 */

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

int process_spawn(const char *path, const char *const argv[], const char *const envp[],
                  const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, pid_t *pid_out)
{
    struct process *cur = process_current();
    KASSERT(cur != NULL);   /* a system call: always on a process */
    if (argv == NULL || argv[0] == NULL)
        return -EINVAL;
    int rc = validate_handles(cur, handles, nr_handles);
    if (rc)
        return rc;

    struct process_spawn_attr attr = {
        .parent = cur,
        .handles = handles,
        .nr_handles = nr_handles,
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

    /* The executable: found through the caller's directories, checked for
     * execute permission with the caller's credentials (vfs_permission:
     * root too needs an x bit), then read by the kernel on its own
     * authority: like Linux, exec needs x, not r. */
    struct vnode *exe;
    rc = vfs_lookup(cur->cwd, path, &exe);
    if (rc)
        goto out_cwd;
    if (exe->type != VNODE_REG) {
        vnode_put(exe);
        rc = -EACCES;
        goto out_cwd;
    }
    rc = vfs_permission(exe, VFS_MAY_EXEC);
    if (rc) {
        vnode_put(exe);
        goto out_cwd;
    }
    struct file *f;
    rc = vfs_open_vnode(exe, COSMO_O_RDONLY, &f);   /* consumes the reference */
    if (rc)
        goto out_cwd;
    struct cosmo_stat st;
    file_stat(f, &st);
    if (st.size == 0 || st.size > SPAWN_IMAGE_MAX) {
        rc = -ENOEXEC;
        goto out_file;
    }
    size_t size = (size_t)st.size;
    vaddr_t image = vm_kernel_alloc((size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1), VM_KALLOC_POPULATE,
                                    VM_PROT_RW);
    if (image == 0) {
        rc = -ENOMEM;
        goto out_file;
    }
    size_t got = 0;
    while (got < size) {
        int64_t n = file_pread(f, (uint8_t *)image + got, size - got, got);
        if (n <= 0) {
            rc = n < 0 ? (int)n : -EIO;
            goto out_image;
        }
        got += (size_t)n;
    }

    struct process *p = NULL;
    rc = process_create_from_elf((const void *)image, size, basename_of(path), argv, envp, &attr, &p);
    if (rc == 0) {
        *pid_out = p->pid;
        process_put(p);   /* the creator's reference; the table and the thread hold theirs */
    }
out_image:
    vm_kernel_free(image);
out_file:
    file_put(f);
out_cwd:
    if (attr.cwd)
        vnode_put(attr.cwd);
    return rc;
}
