/*
 * syscalls.c - The Linux personality: the x86-64 Linux system-call table
 * translating onto the native kernel services (docs/compat/linux/).
 *
 * Every entry validates its arguments, converts Linux structures and
 * flags (convert.c), and calls the same VFS, socket, process and memory
 * services the native personality uses. Nothing here is reachable by a
 * native process, and nothing native depends on this file.
 */

#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/futex.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/percpu.h>
#include <kernel/pipe.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/version.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>
#include <arch/user.h>

#include "convert.h"
#include "linux_abi.h"

#define LX_BRK_MAX (1ull << 30)
#define IOV_MAX 1024
#define SOCK_CHUNK 4096

struct linux_state {
    uint64_t brk_start, brk;
    uint64_t clear_child_tid;
    uint64_t sigmask;
    struct lx_sigaction act[LX_NSIG];
    struct lx_stack_t altstack;
    unsigned unknown_syscalls;
};

/* --- process hooks (kernel/process/process.c) --------------------------------- */

static const syscall_fn *linux_table_get(void);

int linux_process_init(struct process *p, const struct elf_info *info)
{
    (void)linux_table_get();
    struct linux_state *ls = kzalloc(sizeof(*ls));
    if (ls == NULL)
        return -ENOMEM;
    ls->brk_start = page_align_up(info->hi);
    ls->brk = ls->brk_start;
    p->linux = ls;
    return 0;
}

void linux_process_release(struct process *p)
{
    kfree(p->linux);
    p->linux = NULL;
}

unsigned linux_auxv(struct process *p, const struct elf_info *info, uint64_t random_addr, uint64_t *w, unsigned max)
{
    unsigned k = 0;
#define AUX(t, v)                                                                        \
    do {                                                                                 \
        if (k + 2 <= max) {                                                              \
            w[k++] = (t);                                                                \
            w[k++] = (v);                                                                \
        }                                                                                \
    } while (0)
    AUX(LX_AT_PHDR, info->phdr_vaddr);
    AUX(LX_AT_PHENT, info->phent);
    AUX(LX_AT_PHNUM, info->phnum);
    AUX(LX_AT_PAGESZ, PAGE_SIZE);
    AUX(LX_AT_ENTRY, info->entry);
    AUX(LX_AT_RANDOM, random_addr);
    AUX(LX_AT_UID, p->cred.uid);
    AUX(LX_AT_EUID, p->cred.uid);
    AUX(LX_AT_GID, p->cred.gid);
    AUX(LX_AT_EGID, p->cred.gid);
    AUX(LX_AT_SECURE, 0);
    AUX(LX_AT_HWCAP, 0);
    AUX(LX_AT_CLKTCK, 100);
    AUX(LX_AT_NULL, 0);
#undef AUX
    return k;
}

/* --- helpers ------------------------------------------------------------------ */

static struct linux_state *lx(void)
{
    return process_current()->linux;
}

static int get_path(uint64_t uptr, char *buf)
{
    int rc = strncpy_from_user(buf, uptr, VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    return buf[0] == '\0' ? -ENOENT : 0;
}

/* Only AT_FDCWD is a directory handle the kernel can resolve from; a
 * real dirfd would need openat semantics the VFS does not offer yet. */
static int check_dirfd(int64_t dirfd, const char *path)
{
    if ((int)dirfd == LX_AT_FDCWD || path[0] == '/')
        return 0;
    return -ENOSYS;
}

static struct file *file_of(int h, unsigned rights)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, rights);
    if (obj == NULL)
        return NULL;
    struct file *f = file_from_kobject(obj);
    if (f == NULL)
        kobject_put(obj);
    return f;
}

static struct socket *sock_of(int h, unsigned rights)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, rights);
    if (obj == NULL)
        return NULL;
    struct socket *s = socket_from_kobject(obj);
    if (s == NULL)
        kobject_put(obj);
    return s;
}

static int64_t put_u64(uint64_t uptr, uint64_t v)
{
    return copy_to_user(uptr, &v, sizeof(v)) ? -EFAULT : 0;
}

static int ns_from_timespec(uint64_t uptr, uint64_t *ns)
{
    struct lx_timespec ts;
    if (copy_from_user(&ts, uptr, sizeof(ts)))
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    if ((uint64_t)ts.tv_sec > 3600ull * 24 * 365)
        return -EINVAL;
    *ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

static int64_t put_timespec(uint64_t uptr, uint64_t ns)
{
    struct lx_timespec ts = { .tv_sec = (int64_t)(ns / 1000000000ull), .tv_nsec = (int64_t)(ns % 1000000000ull) };
    return copy_to_user(uptr, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/* --- files ---------------------------------------------------------------------- */

static int64_t lx_read(struct syscall_args *a) { return syscall_handle_read((int)a->a[0], a->a[1], (size_t)a->a[2]); }
static int64_t lx_write(struct syscall_args *a) { return syscall_handle_write((int)a->a[0], a->a[1], (size_t)a->a[2]); }

static int64_t rw_at(struct syscall_args *a, bool write)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    int64_t off = (int64_t)a->a[3];
    if (off < 0)
        return -EINVAL;
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct file *f = file_of((int)a->a[0], write ? HANDLE_RIGHT_WRITE : HANDLE_RIGHT_READ);
    if (f == NULL)
        return -EBADF;
    uint8_t *tmp = kmalloc(len < 4096 ? (len ? len : 1) : 4096, 0);
    if (tmp == NULL) {
        file_put(f);
        return -ENOMEM;
    }
    int64_t done = 0, rc = 0;
    while ((size_t)done < len) {
        size_t n = len - (size_t)done < 4096 ? len - (size_t)done : 4096;
        if (write) {
            if (copy_from_user(tmp, ubuf + (uint64_t)done, n)) {
                rc = -EFAULT;
                break;
            }
            rc = file_pwrite(f, tmp, n, (uint64_t)(off + done));
        } else {
            rc = file_pread(f, tmp, n, (uint64_t)(off + done));
            if (rc > 0 && copy_to_user(ubuf + (uint64_t)done, tmp, (size_t)rc))
                rc = -EFAULT;
        }
        if (rc <= 0)
            break;
        done += rc;
        if ((size_t)rc < n)
            break;
    }
    kfree(tmp);
    file_put(f);
    return done > 0 ? done : rc;
}

static int64_t lx_pread64(struct syscall_args *a) { return rw_at(a, false); }
static int64_t lx_pwrite64(struct syscall_args *a) { return rw_at(a, true); }

static int64_t rw_vec(struct syscall_args *a, bool write)
{
    int h = (int)a->a[0];
    uint64_t uiov = a->a[1];
    int64_t cnt = (int64_t)a->a[2];
    if (cnt < 0 || cnt > IOV_MAX)
        return -EINVAL;
    int64_t done = 0;
    for (int64_t i = 0; i < cnt; i++) {
        struct lx_iovec iov;
        if (copy_from_user(&iov, uiov + (uint64_t)i * sizeof(iov), sizeof(iov)))
            return done ? done : -EFAULT;
        if (iov.iov_len == 0)
            continue;
        int64_t rc = write ? syscall_handle_write(h, iov.iov_base, (size_t)iov.iov_len)
                           : syscall_handle_read(h, iov.iov_base, (size_t)iov.iov_len);
        if (rc < 0)
            return done ? done : rc;
        done += rc;
        if ((uint64_t)rc < iov.iov_len)
            break;
    }
    return done;
}

static int64_t lx_readv(struct syscall_args *a) { return rw_vec(a, false); }
static int64_t lx_writev(struct syscall_args *a) { return rw_vec(a, true); }

static int64_t do_open(uint64_t upath, unsigned lxflags, uint32_t mode)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(upath, path);
    if (rc)
        return rc;
    unsigned flags;
    if (lx_open_flags(lxflags, &flags) < 0)
        return -EINVAL;
    struct file *f;
    rc = vfs_open(process_current()->cwd, path, flags, mode & 07777u, &f);
    if (rc)
        return rc;
    unsigned rights = 0, acc = flags & COSMO_O_ACCMODE;
    if (acc == COSMO_O_RDONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_READ;
    if (acc == COSMO_O_WRONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_WRITE;
    int h = handle_install(&process_current()->handles, &f->obj, rights);
    file_put(f);
    return h;
}

static int64_t lx_open(struct syscall_args *a) { return do_open(a->a[0], (unsigned)a->a[1], (uint32_t)a->a[2]); }
static int64_t lx_creat(struct syscall_args *a)
{
    return do_open(a->a[0], LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, (uint32_t)a->a[1]);
}
static int64_t lx_openat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    rc = check_dirfd((int64_t)a->a[0], path);
    if (rc)
        return rc;
    return do_open(a->a[1], (unsigned)a->a[2], (uint32_t)a->a[3]);
}

static int64_t lx_close(struct syscall_args *a) { return handle_close(&process_current()->handles, (int)a->a[0]); }

static int64_t lx_lseek(struct syscall_args *a)
{
    struct file *f = file_of((int)a->a[0], 0);
    if (f == NULL) {
        /* pipes, sockets, the console: not seekable */
        struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], 0);
        if (obj == NULL)
            return -EBADF;
        kobject_put(obj);
        return -ESPIPE;
    }
    int64_t rc = file_seek(f, (int64_t)a->a[1], (int)a->a[2]);   /* SEEK_* values coincide */
    file_put(f);
    return rc;
}

static int64_t stat_out(const struct cosmo_stat *st, uint64_t uptr)
{
    struct lx_stat ls;
    lx_stat_from_native(st, &ls);
    return copy_to_user(uptr, &ls, sizeof(ls)) ? -EFAULT : 0;
}

static int64_t lx_stat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    rc = vfs_stat(process_current()->cwd, path, &st);
    return rc ? rc : stat_out(&st, a->a[1]);
}

static int64_t lx_fstat(struct syscall_args *a)
{
    struct cosmo_stat st;
    int rc = syscall_handle_stat((int)a->a[0], &st);
    return rc ? rc : stat_out(&st, a->a[1]);
}

static int64_t lx_newfstatat(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[3];
    char path[VFS_PATH_MAX];
    int rc = strncpy_from_user(path, a->a[1], VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    if (path[0] == '\0') {
        if (!(flags & LX_AT_EMPTY_PATH))
            return -ENOENT;
        struct cosmo_stat st;
        rc = syscall_handle_stat((int)a->a[0], &st);
        return rc ? rc : stat_out(&st, a->a[2]);
    }
    rc = check_dirfd((int64_t)a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    rc = vfs_stat(process_current()->cwd, path, &st);
    return rc ? rc : stat_out(&st, a->a[2]);
}

static int64_t lx_getdents64(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (len > 65536)
        len = 65536;
    if (len < 32)
        return -EINVAL;
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct file *f = file_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (f == NULL)
        return -EBADF;
    /* Native records are at most as large as Linux ones plus alignment;
     * read fewer bytes than the caller offers so the converted output fits. */
    uint8_t *in = kmalloc(len, 0);
    uint8_t *out = kmalloc(len, 0);
    if (in == NULL || out == NULL) {
        kfree(in);
        kfree(out);
        file_put(f);
        return -ENOMEM;
    }
    int64_t n = file_readdir(f, in, len - len / 4);
    file_put(f);
    int64_t rc = n;
    if (n > 0) {
        size_t o = lx_dirents_from_native(in, (size_t)n, out, len);
        rc = (int64_t)o;
        if (o && copy_to_user(ubuf, out, o))
            rc = -EFAULT;
    }
    kfree(in);
    kfree(out);
    return rc;
}

static int64_t path_call(struct syscall_args *a, unsigned which, uint64_t upath)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(upath, path);
    if (rc)
        return rc;
    struct vnode *cwd = process_current()->cwd;
    switch (which) {
    case 0: return vfs_mkdir(cwd, path, (uint32_t)a->a[1] & 07777u);
    case 1: return vfs_rmdir(cwd, path);
    case 2: return vfs_unlink(cwd, path);
    case 3: {
        struct cosmo_stat st;
        return vfs_stat(cwd, path, &st);   /* access: existence */
    }
    default: return -ENOSYS;
    }
}

static int64_t lx_mkdir(struct syscall_args *a) { return path_call(a, 0, a->a[0]); }
static int64_t lx_rmdir(struct syscall_args *a) { return path_call(a, 1, a->a[0]); }
static int64_t lx_unlink(struct syscall_args *a) { return path_call(a, 2, a->a[0]); }
static int64_t lx_access(struct syscall_args *a) { return path_call(a, 3, a->a[0]); }

static int64_t lx_mkdirat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    rc = check_dirfd((int64_t)a->a[0], path);
    return rc ? rc : vfs_mkdir(process_current()->cwd, path, (uint32_t)a->a[2] & 07777u);
}

static int64_t lx_unlinkat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    rc = check_dirfd((int64_t)a->a[0], path);
    if (rc)
        return rc;
    return (a->a[2] & LX_AT_REMOVEDIR) ? vfs_rmdir(process_current()->cwd, path)
                                        : vfs_unlink(process_current()->cwd, path);
}

static int64_t lx_faccessat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    rc = check_dirfd((int64_t)a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    return vfs_stat(process_current()->cwd, path, &st);
}

static int64_t lx_rename(struct syscall_args *a)
{
    char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
    int rc = get_path(a->a[0], oldp);
    if (rc)
        return rc;
    rc = get_path(a->a[1], newp);
    return rc ? rc : vfs_rename(process_current()->cwd, oldp, newp);
}

static int64_t lx_renameat(struct syscall_args *a)
{
    char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
    int rc = get_path(a->a[1], oldp);
    if (rc)
        return rc;
    rc = get_path(a->a[3], newp);
    if (rc)
        return rc;
    rc = check_dirfd((int64_t)a->a[0], oldp);
    if (rc == 0)
        rc = check_dirfd((int64_t)a->a[2], newp);
    return rc ? rc : vfs_rename(process_current()->cwd, oldp, newp);
}

static int64_t lx_chdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : process_chdir(path);
}

static int64_t lx_getcwd(struct syscall_args *a)
{
    struct process *p = process_current();
    size_t len = (size_t)a->a[1];
    char buf[VFS_PATH_MAX];
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    size_t n = strlcpy(buf, p->cwd_path, sizeof(buf));
    spin_unlock_irqrestore(&p->lock, s);
    if (len < n + 1)
        return -ERANGE;
    return copy_to_user(a->a[0], buf, n + 1) ? -EFAULT : (int64_t)(n + 1);   /* Linux returns the length incl. NUL */
}

static int64_t lx_dup(struct syscall_args *a)
{
    struct handle_table *t = &process_current()->handles;
    unsigned rights;
    struct kobject *obj = handle_get(t, (int)a->a[0], &rights);
    if (obj == NULL)
        return -EBADF;
    int rc = handle_install(t, obj, rights);
    kobject_put(obj);
    return rc;
}

static int64_t dup_to(int h, int target)
{
    if (target < 0 || target >= HANDLE_TABLE_SIZE)
        return -EBADF;
    struct handle_table *t = &process_current()->handles;
    unsigned rights;
    struct kobject *obj = handle_get(t, h, &rights);
    if (obj == NULL)
        return -EBADF;
    int rc;
    if (target == h) {
        rc = h;
    } else {
        handle_close(t, target);
        rc = handle_install_at(t, target, obj, rights);
    }
    kobject_put(obj);
    return rc;
}

static int64_t lx_dup2(struct syscall_args *a) { return dup_to((int)a->a[0], (int)a->a[1]); }
static int64_t lx_dup3(struct syscall_args *a)
{
    if ((int)a->a[0] == (int)a->a[1])
        return -EINVAL;
    return dup_to((int)a->a[0], (int)a->a[1]);
}

static int64_t do_pipe(uint64_t uarr)
{
    if (!user_range_ok(uarr, 8))
        return -EFAULT;
    struct kobject *rd, *wr;
    int rc = pipe_create(&rd, &wr);
    if (rc)
        return rc;
    struct handle_table *t = &process_current()->handles;
    int32_t h[2];
    h[0] = handle_install(t, rd, HANDLE_RIGHT_READ);
    h[1] = h[0] < 0 ? -EMFILE : handle_install(t, wr, HANDLE_RIGHT_WRITE);
    kobject_put(rd);
    kobject_put(wr);
    if (h[0] < 0 || h[1] < 0) {
        if (h[0] >= 0)
            handle_close(t, h[0]);
        return -EMFILE;
    }
    if (copy_to_user(uarr, h, sizeof(h))) {
        handle_close(t, h[0]);
        handle_close(t, h[1]);
        return -EFAULT;
    }
    return 0;
}

static int64_t lx_pipe(struct syscall_args *a) { return do_pipe(a->a[0]); }
static int64_t lx_pipe2(struct syscall_args *a) { return do_pipe(a->a[0]); }   /* O_CLOEXEC/O_NONBLOCK dropped */

static int64_t lx_fcntl(struct syscall_args *a)
{
    int h = (int)a->a[0];
    unsigned cmd = (unsigned)a->a[1];
    struct handle_table *t = &process_current()->handles;
    unsigned rights;
    struct kobject *obj = handle_get(t, h, &rights);
    if (obj == NULL)
        return -EBADF;
    int64_t rc;
    switch (cmd) {
    case LX_F_GETFD:
    case LX_F_SETFD:
    case LX_F_SETFL:
        rc = 0;
        break;
    case LX_F_GETFL:
        rc = (rights & HANDLE_RIGHT_READ) && (rights & HANDLE_RIGHT_WRITE) ? LX_O_RDWR
             : (rights & HANDLE_RIGHT_WRITE)                                ? LX_O_WRONLY
                                                                            : LX_O_RDONLY;
        break;
    case LX_F_DUPFD:
    case LX_F_DUPFD_CLOEXEC: {
        int min = (int)a->a[2];
        rc = -EMFILE;
        for (int i = min < 0 ? 0 : min; i < HANDLE_TABLE_SIZE; i++) {
            int r = handle_install_at(t, i, obj, rights);
            if (r >= 0) {
                rc = r;
                break;
            }
            if (r != -EBUSY)
                break;
        }
        break;
    }
    default:
        rc = -EINVAL;
    }
    kobject_put(obj);
    return rc;
}

static int64_t lx_ioctl(struct syscall_args *a)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], 0);
    if (obj == NULL)
        return -EBADF;
    kobject_put(obj);
    return -ENOTTY;   /* every request: libcs then treat the console as a plain file */
}

static int64_t lx_sync(struct syscall_args *a) { (void)a; return vfs_sync(); }
static int64_t lx_fsync(struct syscall_args *a)
{
    struct file *f = file_of((int)a->a[0], 0);
    if (f == NULL)
        return -EBADF;
    int rc = file_sync(f);
    file_put(f);
    return rc;
}
static int64_t lx_umask(struct syscall_args *a) { (void)a; return 022; }

/* --- memory --------------------------------------------------------------------- */

static int64_t lx_brk(struct syscall_args *a)
{
    struct process *p = process_current();
    struct linux_state *ls = lx();
    uint64_t want = a->a[0];
    if (want == 0)
        return (int64_t)ls->brk;
    if (want < ls->brk_start || want > ls->brk_start + LX_BRK_MAX)
        return (int64_t)ls->brk;   /* Linux returns the unchanged break on failure */
    uint64_t cur_end = page_align_up(ls->brk), new_end = page_align_up(want);
    if (new_end > cur_end) {
        if (vm_user_map_anon(p->space, cur_end, new_end - cur_end, VM_PROT_RW, 0, "brk") != 0)
            return (int64_t)ls->brk;
    } else if (new_end < cur_end) {
        vm_user_unmap(p->space, new_end, cur_end - new_end);
    }
    ls->brk = want;
    return (int64_t)want;
}

static int64_t lx_mmap(struct syscall_args *a)
{
    uint64_t hint = a->a[0];
    size_t len = (size_t)a->a[1];
    unsigned prot = (unsigned)a->a[2], flags = (unsigned)a->a[3];
    struct process *p = process_current();
    if (len == 0 || len > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    len = page_align_up(len);
    if (!(flags & LX_MAP_ANONYMOUS))
        return -ENODEV;   /* file mappings: the dynamic linker's stage */
    int nprot;
    if (lx_prot(prot, &nprot) < 0)
        return -EINVAL;
    if ((nprot & COSMO_PROT_WRITE) && (nprot & COSMO_PROT_EXEC))
        return -EINVAL;   /* W^X */
    vm_prot_t vprot = 0;
    if (nprot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (nprot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (nprot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    if (vprot == 0)
        vprot = VM_PROT_READ;
    uint64_t base;
    if (flags & LX_MAP_FIXED) {
        if (!is_page_aligned(hint) || !user_range_ok(hint, len))
            return -EINVAL;
        vm_user_unmap(p->space, hint, len);   /* Linux replaces what was there */
        base = hint;
    } else {
        uint64_t from = (hint >= USER_LO && is_page_aligned(hint)) ? hint : USER_MMAP_BASE;
        base = vm_user_find_free(p->space, from, len);
        if (base == 0 && from != USER_MMAP_BASE)
            base = vm_user_find_free(p->space, USER_MMAP_BASE, len);
        if (base == 0)
            return -ENOMEM;
    }
    int rc = vm_user_map_anon(p->space, base, len, vprot, 0, "mmap");
    return rc ? rc : (int64_t)base;
}

static int64_t lx_munmap(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    if (!is_page_aligned(addr) || len == 0 || !user_range_ok(addr, page_align_up(len)))
        return -EINVAL;
    return vm_user_unmap(process_current()->space, addr, page_align_up(len));
}

static int64_t lx_mprotect(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = page_align_up((size_t)a->a[1]);
    int nprot;
    if (!is_page_aligned(addr) || len == 0 || lx_prot((unsigned)a->a[2], &nprot) < 0)
        return -EINVAL;
    if ((nprot & COSMO_PROT_WRITE) && (nprot & COSMO_PROT_EXEC))
        return -EINVAL;
    vm_prot_t vprot = 0;
    if (nprot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (nprot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (nprot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    if (vprot == 0)
        vprot = VM_PROT_READ;
    return vm_user_protect(process_current()->space, addr, len, vprot);
}

static int64_t lx_madvise(struct syscall_args *a) { (void)a; return 0; }

/* --- process, identity, signals -------------------------------------------------- */

static int64_t lx_exit(struct syscall_args *a) { process_exit((int)a->a[0] & 0xff); }
static int64_t lx_getpid(struct syscall_args *a) { (void)a; return process_current()->pid; }
static int64_t lx_getppid(struct syscall_args *a) { (void)a; return process_current()->parent_pid; }
static int64_t lx_getuid(struct syscall_args *a) { (void)a; return process_current()->cred.uid; }
static int64_t lx_getgid(struct syscall_args *a) { (void)a; return process_current()->cred.gid; }
static int64_t lx_zero(struct syscall_args *a) { (void)a; return 0; }
static int64_t lx_nosys(struct syscall_args *a) { (void)a; return -ENOSYS; }
static int64_t lx_getpgrp(struct syscall_args *a) { (void)a; return process_current()->pid; }

static int64_t lx_set_tid_address(struct syscall_args *a)
{
    lx()->clear_child_tid = a->a[0];
    return process_current()->pid;
}

static int64_t lx_arch_prctl(struct syscall_args *a)
{
    unsigned code = (unsigned)a->a[0];
    uint64_t addr = a->a[1];
    switch (code) {
    case LX_ARCH_SET_FS:
        if (addr != 0 && !user_range_ok(addr, 8))
            return -EPERM;
        arch_set_tls_base((uintptr_t)addr);
        return 0;
    case LX_ARCH_GET_FS:
        return put_u64(addr, thread_current()->tls_base);
    case LX_ARCH_SET_GS:
    case LX_ARCH_GET_GS:
    default:
        return -EINVAL;
    }
}

static int64_t lx_wait4(struct syscall_args *a)
{
    int pid = (int)a->a[0];
    unsigned options = (unsigned)a->a[2];
    if (pid == 0 || pid < -1)
        return -ECHILD;   /* process groups do not exist */
    pid_t got = 0;
    int status = 0;
    int rc = process_wait_child(pid, (options & LX_WNOHANG) ? PROCESS_WAIT_NOHANG : 0, &got, &status);
    if (rc)
        return rc;
    if (got != 0 && a->a[1] != 0) {
        int32_t w = lx_wait_status(status);
        if (copy_to_user(a->a[1], &w, sizeof(w)))
            return -EFAULT;
    }
    if (got != 0 && a->a[3] != 0) {
        uint8_t zero[144] = { 0 };   /* struct rusage */
        if (copy_to_user(a->a[3], zero, sizeof(zero)))
            return -EFAULT;
    }
    return got;
}

static int64_t lx_kill(struct syscall_args *a)
{
    int pid = (int)a->a[0], sig = (int)a->a[1];
    if (pid <= 0)
        return -ESRCH;
    if (sig == 0) {
        struct process *t = process_lookup((pid_t)pid);
        if (t == NULL)
            return -ESRCH;
        process_put(t);
        return 0;
    }
    if (sig < 1 || sig >= LX_NSIG)
        return -EINVAL;
    struct process *target = process_lookup((pid_t)pid);
    if (target == NULL)
        return -ESRCH;
    struct process *cur = process_current();
    int rc = 0;
    if (cur->cred.uid != 0 && cur->cred.uid != target->cred.uid)
        rc = -EPERM;
    else
        process_kill(target, sig < 32 ? sig : 9);
    process_put(target);
    return rc;
}

static int64_t lx_tgkill(struct syscall_args *a)
{
    struct syscall_args k = { .a = { a->a[1], a->a[2] } };
    return lx_kill(&k);
}

static int64_t lx_rt_sigaction(struct syscall_args *a)
{
    int sig = (int)a->a[0];
    if (sig < 1 || sig >= LX_NSIG || a->a[3] != 8)
        return -EINVAL;
    struct linux_state *ls = lx();
    if (a->a[2] && copy_to_user(a->a[2], &ls->act[sig], sizeof(struct lx_sigaction)))
        return -EFAULT;
    if (a->a[1]) {
        if (sig == LX_SIGKILL || sig == LX_SIGSTOP)
            return -EINVAL;
        struct lx_sigaction act;
        if (copy_from_user(&act, a->a[1], sizeof(act)))
            return -EFAULT;
        ls->act[sig] = act;   /* stored; nothing is delivered in this phase */
    }
    return 0;
}

static int64_t lx_rt_sigprocmask(struct syscall_args *a)
{
    if (a->a[3] != 8)
        return -EINVAL;
    struct linux_state *ls = lx();
    uint64_t old = ls->sigmask;
    if (a->a[1]) {
        uint64_t set;
        if (copy_from_user(&set, a->a[1], 8))
            return -EFAULT;
        switch ((int)a->a[0]) {
        case 0: ls->sigmask |= set; break;       /* SIG_BLOCK */
        case 1: ls->sigmask &= ~set; break;      /* SIG_UNBLOCK */
        case 2: ls->sigmask = set; break;        /* SIG_SETMASK */
        default: return -EINVAL;
        }
    }
    if (a->a[2] && copy_to_user(a->a[2], &old, 8))
        return -EFAULT;
    return 0;
}

static int64_t lx_sigaltstack(struct syscall_args *a)
{
    struct linux_state *ls = lx();
    if (a->a[1] && copy_to_user(a->a[1], &ls->altstack, sizeof(ls->altstack)))
        return -EFAULT;
    if (a->a[0] && copy_from_user(&ls->altstack, a->a[0], sizeof(ls->altstack)))
        return -EFAULT;
    return 0;
}

/* --- time and misc ---------------------------------------------------------------- */

static int64_t lx_clock_gettime(struct syscall_args *a)
{
    unsigned clk = (unsigned)a->a[0];
    if (clk > LX_CLOCK_BOOTTIME)
        return -EINVAL;
    return put_timespec(a->a[1], clock_now_ns());   /* no wall clock yet: every clock is monotonic */
}

static int64_t lx_gettimeofday(struct syscall_args *a)
{
    if (a->a[0]) {
        uint64_t ns = clock_now_ns();
        struct lx_timeval tv = { .tv_sec = (int64_t)(ns / 1000000000ull), .tv_usec = (int64_t)(ns % 1000000000ull / 1000) };
        if (copy_to_user(a->a[0], &tv, sizeof(tv)))
            return -EFAULT;
    }
    return 0;
}

static int64_t lx_time(struct syscall_args *a)
{
    int64_t t = (int64_t)(clock_now_ns() / 1000000000ull);
    if (a->a[0] && copy_to_user(a->a[0], &t, sizeof(t)))
        return -EFAULT;
    return t;
}

static int64_t lx_nanosleep(struct syscall_args *a)
{
    uint64_t ns;
    int rc = ns_from_timespec(a->a[0], &ns);
    if (rc)
        return rc;
    rc = thread_sleep_ns_killable(ns);
    if (rc && a->a[1])
        put_timespec(a->a[1], 0);
    return rc;
}

static int64_t lx_clock_nanosleep(struct syscall_args *a)
{
    unsigned clk = (unsigned)a->a[0], flags = (unsigned)a->a[1];
    if (clk > LX_CLOCK_BOOTTIME)
        return -EINVAL;
    uint64_t ns;
    int rc = ns_from_timespec(a->a[2], &ns);
    if (rc)
        return rc;
    if (flags & LX_TIMER_ABSTIME) {
        uint64_t now = clock_now_ns();
        ns = ns > now ? ns - now : 0;
    }
    rc = thread_sleep_ns_killable(ns);
    if (rc && a->a[3] && !(flags & LX_TIMER_ABSTIME))
        put_timespec(a->a[3], 0);
    return rc;
}

static int64_t lx_sched_yield(struct syscall_args *a) { (void)a; sched_yield(); return 0; }

static int64_t lx_getrandom(struct syscall_args *a)
{
    size_t len = (size_t)a->a[1];
    if (len > 256 * 1024)
        len = 256 * 1024;
    if (!user_range_ok(a->a[0], len))
        return -EFAULT;
    uint8_t tmp[256];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        random_get_bytes(tmp, n);
        if (copy_to_user(a->a[0] + done, tmp, n))
            return -EFAULT;
        done += n;
    }
    return (int64_t)len;
}

static int64_t lx_uname(struct syscall_args *a)
{
    struct lx_utsname u;
    memset(&u, 0, sizeof(u));
    strlcpy(u.sysname, "Linux", sizeof(u.sysname));
    strlcpy(u.nodename, "cosmo", sizeof(u.nodename));
    strlcpy(u.release, "6.0.0-cosmo", sizeof(u.release));
    ksnprintf(u.version, sizeof(u.version), "%s %s %s", KERNEL_NAME, KERNEL_VERSION, COSMO_BUILD_ID);
    strlcpy(u.machine, "x86_64", sizeof(u.machine));
    strlcpy(u.domainname, "(none)", sizeof(u.domainname));
    return copy_to_user(a->a[0], &u, sizeof(u)) ? -EFAULT : 0;
}

static int64_t lx_futex(struct syscall_args *a)
{
    uint64_t uaddr = a->a[0];
    unsigned op = (unsigned)a->a[1] & (unsigned)LX_FUTEX_CMD_MASK;
    uint32_t val = (uint32_t)a->a[2];
    struct vm_space *space = process_current()->space;
    if (!user_range_ok(uaddr, 4))
        return -EFAULT;
    switch (op) {
    case LX_FUTEX_WAIT: {
        uint64_t timeout = 0;
        if (a->a[3]) {
            int rc = ns_from_timespec(a->a[3], &timeout);
            if (rc)
                return rc;
            if (timeout == 0)
                timeout = 1;
        }
        return futex_wait(space, uaddr, val, timeout);
    }
    case LX_FUTEX_WAKE:
        return futex_wake(space, uaddr, val);
    default:
        return -ENOSYS;
    }
}

/* --- sockets ----------------------------------------------------------------------- */

static int addr_from_user(uint64_t uptr, size_t len, struct netaddr *out)
{
    uint8_t buf[sizeof(struct lx_sockaddr_in6)];
    if (len < 2 || len > sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, uptr, len))
        return -EFAULT;
    return lx_sockaddr_to_netaddr(buf, len, out);
}

static int addr_to_user(uint64_t uptr, uint64_t ulen, const struct netaddr *na)
{
    if (uptr == 0 || ulen == 0)
        return 0;
    int32_t cap;
    if (copy_from_user(&cap, ulen, sizeof(cap)))
        return -EFAULT;
    if (cap < 0)
        return -EINVAL;
    uint8_t buf[sizeof(struct lx_sockaddr_in6)];
    size_t full = lx_sockaddr_from_netaddr(na, buf, sizeof(buf));
    size_t n = (size_t)cap < full ? (size_t)cap : full;
    if (n && copy_to_user(uptr, buf, n))
        return -EFAULT;
    int32_t out = (int32_t)full;
    return copy_to_user(ulen, &out, sizeof(out)) ? -EFAULT : 0;
}

static int64_t lx_socket(struct syscall_args *a)
{
    int family = (int)a->a[0];
    unsigned type = (unsigned)a->a[1] & ~(unsigned)(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC);
    if (family != LX_AF_INET && family != LX_AF_INET6)
        return -EAFNOSUPPORT;
    if (type != LX_SOCK_STREAM && type != LX_SOCK_DGRAM)
        return -EINVAL;
    struct socket *s;
    int rc = ksock_create(family == LX_AF_INET ? COSMO_AF_INET : COSMO_AF_INET6,
                          type == LX_SOCK_STREAM ? COSMO_SOCK_STREAM : COSMO_SOCK_DGRAM, process_current()->cred.uid,
                          &s);
    if (rc)
        return rc;
    int h = handle_install(&process_current()->handles, &s->obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    ksock_put(s);
    return h;
}

static int64_t lx_bind(struct syscall_args *a)
{
    struct netaddr addr;
    int rc = addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    rc = ksock_bind(s, &addr);
    ksock_put(s);
    return rc;
}

static int64_t lx_connect(struct syscall_args *a)
{
    struct netaddr addr;
    int rc = addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    rc = ksock_connect(s, &addr);
    ksock_put(s);
    return rc;
}

static int64_t lx_listen(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    int rc = ksock_listen(s, (int)a->a[1]);
    ksock_put(s);
    return rc;
}

static int64_t lx_accept(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (s == NULL)
        return -EBADF;
    struct socket *c;
    struct netaddr peer;
    int rc = ksock_accept(s, &c, &peer);
    ksock_put(s);
    if (rc)
        return rc;
    rc = addr_to_user(a->a[1], a->a[2], &peer);
    if (rc) {
        ksock_put(c);
        return rc;
    }
    int h = handle_install(&process_current()->handles, &c->obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    ksock_put(c);
    return h;
}

static int64_t lx_sendto(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct netaddr to;
    bool have_to = a->a[4] != 0;
    if (have_to) {
        int rc = addr_from_user(a->a[4], (size_t)a->a[5], &to);
        if (rc)
            return rc;
    }
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (s == NULL)
        return -EBADF;
    size_t cap = s->type == COSMO_SOCK_DGRAM ? (len ? len : 1) : SOCK_CHUNK;
    if (cap > SOCK_CHUNK * 16) {
        ksock_put(s);
        return -EMSGSIZE;
    }
    uint8_t *tmp = kmalloc(cap, 0);
    if (tmp == NULL) {
        ksock_put(s);
        return -ENOMEM;
    }
    int64_t done = 0, rc = 0;
    if (s->type == COSMO_SOCK_DGRAM) {
        if (copy_from_user(tmp, ubuf, len))
            rc = -EFAULT;
        else
            rc = ksock_sendto(s, tmp, len, have_to ? &to : NULL);
        done = rc > 0 ? rc : 0;
    } else {
        while ((size_t)done < len) {
            size_t n = len - (size_t)done < SOCK_CHUNK ? len - (size_t)done : SOCK_CHUNK;
            if (copy_from_user(tmp, ubuf + (uint64_t)done, n)) {
                rc = -EFAULT;
                break;
            }
            int64_t w = ksock_sendto(s, tmp, n, have_to ? &to : NULL);
            if (w <= 0) {
                rc = w;
                break;
            }
            done += w;
            if ((size_t)w < n)
                break;
        }
    }
    kfree(tmp);
    ksock_put(s);
    return done > 0 ? done : rc;
}

static int64_t lx_recvfrom(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (s == NULL)
        return -EBADF;
    size_t cap = len < SOCK_CHUNK * 16 ? (len ? len : 1) : SOCK_CHUNK * 16;
    uint8_t *tmp = kmalloc(cap, 0);
    if (tmp == NULL) {
        ksock_put(s);
        return -ENOMEM;
    }
    struct netaddr from;
    int64_t rc = ksock_recvfrom(s, tmp, cap, a->a[4] ? &from : NULL);
    ksock_put(s);
    if (rc > 0 && copy_to_user(ubuf, tmp, (size_t)rc))
        rc = -EFAULT;
    kfree(tmp);
    if (rc >= 0 && a->a[4]) {
        int r2 = addr_to_user(a->a[4], a->a[5], &from);
        if (r2)
            return r2;
    }
    return rc;
}

static int64_t lx_shutdown(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    int rc = ksock_shutdown(s, (int)a->a[1]);   /* SHUT_* values coincide */
    ksock_put(s);
    return rc;
}

static int64_t name_call(struct syscall_args *a, bool peer)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    struct netaddr addr;
    int rc = peer ? ksock_getpeername(s, &addr) : ksock_getsockname(s, &addr);
    ksock_put(s);
    return rc ? rc : addr_to_user(a->a[1], a->a[2], &addr);
}

static int64_t lx_getsockname(struct syscall_args *a) { return name_call(a, false); }
static int64_t lx_getpeername(struct syscall_args *a) { return name_call(a, true); }

static int64_t lx_setsockopt(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    ksock_put(s);
    return (int)a->a[1] == LX_SOL_SOCKET ? 0 : -ENOPROTOOPT;
}

static int64_t lx_getsockopt(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    ksock_put(s);
    return -ENOPROTOOPT;
}

/* --- the table ------------------------------------------------------------------------ */

static int64_t lx_unknown(struct syscall_args *a)
{
    struct linux_state *ls = lx();
    if (ls && ls->unknown_syscalls++ < 8)
        kdebug("linux: pid %u: unimplemented system call %llu", process_current()->pid, (unsigned long long)a->nr);
    return -ENOSYS;
}

static const syscall_fn linux_table[LX_NR_MAX] = {
    [LX_read] = lx_read,
    [LX_write] = lx_write,
    [LX_open] = lx_open,
    [LX_close] = lx_close,
    [LX_stat] = lx_stat,
    [LX_fstat] = lx_fstat,
    [LX_lstat] = lx_stat,
    [LX_lseek] = lx_lseek,
    [LX_mmap] = lx_mmap,
    [LX_mprotect] = lx_mprotect,
    [LX_munmap] = lx_munmap,
    [LX_brk] = lx_brk,
    [LX_rt_sigaction] = lx_rt_sigaction,
    [LX_rt_sigprocmask] = lx_rt_sigprocmask,
    [LX_ioctl] = lx_ioctl,
    [LX_pread64] = lx_pread64,
    [LX_pwrite64] = lx_pwrite64,
    [LX_readv] = lx_readv,
    [LX_writev] = lx_writev,
    [LX_access] = lx_access,
    [LX_pipe] = lx_pipe,
    [LX_sched_yield] = lx_sched_yield,
    [LX_madvise] = lx_madvise,
    [LX_dup] = lx_dup,
    [LX_dup2] = lx_dup2,
    [LX_nanosleep] = lx_nanosleep,
    [LX_getpid] = lx_getpid,
    [LX_socket] = lx_socket,
    [LX_connect] = lx_connect,
    [LX_accept] = lx_accept,
    [LX_sendto] = lx_sendto,
    [LX_recvfrom] = lx_recvfrom,
    [LX_shutdown] = lx_shutdown,
    [LX_bind] = lx_bind,
    [LX_listen] = lx_listen,
    [LX_getsockname] = lx_getsockname,
    [LX_getpeername] = lx_getpeername,
    [LX_setsockopt] = lx_setsockopt,
    [LX_getsockopt] = lx_getsockopt,
    [LX_clone] = lx_nosys,
    [LX_fork] = lx_nosys,
    [LX_vfork] = lx_nosys,
    [LX_execve] = lx_nosys,
    [LX_exit] = lx_exit,
    [LX_wait4] = lx_wait4,
    [LX_kill] = lx_kill,
    [LX_uname] = lx_uname,
    [LX_fcntl] = lx_fcntl,
    [LX_fsync] = lx_fsync,
    [LX_fdatasync] = lx_fsync,
    [LX_getcwd] = lx_getcwd,
    [LX_chdir] = lx_chdir,
    [LX_rename] = lx_rename,
    [LX_mkdir] = lx_mkdir,
    [LX_rmdir] = lx_rmdir,
    [LX_creat] = lx_creat,
    [LX_unlink] = lx_unlink,
    [LX_readlink] = lx_nosys,
    [LX_umask] = lx_umask,
    [LX_gettimeofday] = lx_gettimeofday,
    [LX_getrlimit] = lx_nosys,
    [LX_sysinfo] = lx_nosys,
    [LX_getuid] = lx_getuid,
    [LX_getgid] = lx_getgid,
    [LX_geteuid] = lx_getuid,
    [LX_getegid] = lx_getgid,
    [LX_setpgid] = lx_zero,
    [LX_getppid] = lx_getppid,
    [LX_getpgrp] = lx_getpgrp,
    [LX_setsid] = lx_getpgrp,
    [LX_sigaltstack] = lx_sigaltstack,
    [LX_arch_prctl] = lx_arch_prctl,
    [LX_setrlimit] = lx_nosys,
    [LX_sync] = lx_sync,
    [LX_gettid] = lx_getpid,
    [LX_time] = lx_time,
    [LX_futex] = lx_futex,
    [LX_sched_getaffinity] = lx_nosys,
    [LX_getdents64] = lx_getdents64,
    [LX_set_tid_address] = lx_set_tid_address,
    [LX_clock_gettime] = lx_clock_gettime,
    [LX_clock_nanosleep] = lx_clock_nanosleep,
    [LX_exit_group] = lx_exit,
    [LX_tgkill] = lx_tgkill,
    [LX_openat] = lx_openat,
    [LX_mkdirat] = lx_mkdirat,
    [LX_newfstatat] = lx_newfstatat,
    [LX_unlinkat] = lx_unlinkat,
    [LX_renameat] = lx_renameat,
    [LX_readlinkat] = lx_nosys,
    [LX_faccessat] = lx_faccessat,
    [LX_set_robust_list] = lx_zero,
    [LX_accept4] = lx_accept,
    [LX_dup3] = lx_dup3,
    [LX_pipe2] = lx_pipe2,
    [LX_prlimit64] = lx_nosys,
    [LX_getrandom] = lx_getrandom,
    [LX_rseq] = lx_nosys,
    [LX_clone3] = lx_nosys,
};

/* The dispatcher returns -ENOSYS itself for a NULL entry; lx_unknown is
 * installed for every empty slot so the miss is counted and logged. */
static syscall_fn g_table[LX_NR_MAX];
static bool g_table_ready;
static spinlock_t g_table_lock = SPINLOCK_INIT("linux-table");

static const syscall_fn *linux_table_get(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    if (!g_table_ready) {
        for (unsigned i = 0; i < LX_NR_MAX; i++)
            g_table[i] = linux_table[i] ? linux_table[i] : lx_unknown;
        g_table_ready = true;   /* published before the process can run: it is not yet scheduled */
    }
    spin_unlock_irqrestore(&g_table_lock, s);
    return g_table;
}

/* The table is completed the first time a Linux process is created
 * (linux_process_init), before that process can make a call. */
const struct personality personality_linux = {
    .name = "linux",
    .table = g_table,
    .count = LX_NR_MAX,
};
