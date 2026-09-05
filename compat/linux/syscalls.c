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
#include <kernel/poll.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
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
#include "linux_internal.h"

#define LX_BRK_MAX (1ull << 30)
#define IOV_MAX 1024
#define SOCK_CHUNK 4096

struct linux_state {
    uint64_t brk_start, brk;
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
    return linux_sigtramp_map(p);
}

void linux_process_release(struct process *p)
{
    kfree(p->linux);
    p->linux = NULL;
}

unsigned linux_auxv(struct process *p, const struct elf_info *info, const struct linux_auxv_args *x, uint64_t *w,
                    unsigned max)
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
    AUX(LX_AT_BASE, x->interp_base);
    AUX(LX_AT_ENTRY, info->entry);
    AUX(LX_AT_RANDOM, x->random_addr);
    AUX(LX_AT_EXECFN, x->execfn_addr);
    AUX(LX_AT_PLATFORM, x->platform_addr);
    AUX(LX_AT_UID, p->cred.ruid);
    AUX(LX_AT_EUID, p->cred.euid);
    AUX(LX_AT_GID, p->cred.rgid);
    AUX(LX_AT_EGID, p->cred.egid);
    AUX(LX_AT_SECURE, 0);
    AUX(LX_AT_HWCAP, 0);
    AUX(LX_AT_HWCAP2, 0);
    AUX(LX_AT_CLKTCK, 100);
    AUX(LX_AT_NULL, 0);
#undef AUX
    return k;
}

/* Handlers only the x86-64 table names (AArch64 has the *at forms and
 * ppoll instead) are compiled everywhere and unused there. */
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif

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
    /* Absolute wall-clock times are ~1.8e9 s; anything beyond 2^62 ns
     * (146 years) is "never" and is clamped there so the arithmetic below
     * and the timer's deadline cannot overflow. */
    if ((uint64_t)ts.tv_sec >= (1ull << 62) / 1000000000ull)
        *ns = 1ull << 62;
    else
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

static __maybe_unused int64_t lx_open(struct syscall_args *a) { return do_open(a->a[0], (unsigned)a->a[1], (uint32_t)a->a[2]); }
static __maybe_unused int64_t lx_creat(struct syscall_args *a)
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

static __maybe_unused int64_t lx_stat(struct syscall_args *a)
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

static __maybe_unused int64_t lx_mkdir(struct syscall_args *a) { return path_call(a, 0, a->a[0]); }
static __maybe_unused int64_t lx_rmdir(struct syscall_args *a) { return path_call(a, 1, a->a[0]); }
static __maybe_unused int64_t lx_unlink(struct syscall_args *a) { return path_call(a, 2, a->a[0]); }
static __maybe_unused int64_t lx_access(struct syscall_args *a) { return path_call(a, 3, a->a[0]); }

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

static __maybe_unused int64_t lx_rename(struct syscall_args *a)
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

static __maybe_unused int64_t lx_dup2(struct syscall_args *a) { return dup_to((int)a->a[0], (int)a->a[1]); }
static int64_t lx_dup3(struct syscall_args *a)
{
    if ((int)a->a[0] == (int)a->a[1])
        return -EINVAL;
    return dup_to((int)a->a[0], (int)a->a[1]);
}

static int64_t do_pipe(uint64_t uarr, unsigned flags)
{
    if (!user_range_ok(uarr, 8))
        return -EFAULT;
    struct kobject *rd, *wr;
    int rc = pipe_create(&rd, &wr);
    if (rc)
        return rc;
    if (flags & LX_O_NONBLOCK) {
        kobject_set_nonblock(rd, 1);
        kobject_set_nonblock(wr, 1);
    }
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

static __maybe_unused int64_t lx_pipe(struct syscall_args *a) { return do_pipe(a->a[0], 0); }
static int64_t lx_pipe2(struct syscall_args *a) { return do_pipe(a->a[0], (unsigned)a->a[1]); }   /* O_CLOEXEC dropped */

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
        rc = 0;
        break;
    case LX_F_SETFL: {
        /* O_NONBLOCK is the one status flag with an effect; the rest are accepted and dropped. */
        int r = kobject_set_nonblock(obj, (a->a[2] & LX_O_NONBLOCK) ? 1 : 0);
        rc = (r < 0 && r != -EOPNOTSUPP) ? r : 0;
        break;
    }
    case LX_F_GETFL:
        rc = (rights & HANDLE_RIGHT_READ) && (rights & HANDLE_RIGHT_WRITE) ? LX_O_RDWR
             : (rights & HANDLE_RIGHT_WRITE)                                ? LX_O_WRONLY
                                                                            : LX_O_RDONLY;
        if (kobject_set_nonblock(obj, -1) == 1)
            rc |= LX_O_NONBLOCK;
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
        /* Growth merges into the existing heap region (same prot and name). */
        if (vm_user_map_anon(p->space, cur_end, new_end - cur_end, VM_PROT_RW, 0, "brk") != 0)
            return (int64_t)ls->brk;
    } else if (new_end < cur_end) {
        /* A shrink cannot fail on a well-formed heap; if it does the break
         * stays where it was and no mapping changes. */
        if (vm_user_unmap(p->space, new_end, cur_end - new_end, 0) != 0)
            return (int64_t)ls->brk;
    }
    ls->brk = want;
    return (int64_t)want;
}

/* Fill [base, base+len) of the caller's own new mapping from `f` at
 * `off`: file_pread into a bounce page, copy_to_user into the region
 * (demand-zero pages appear as the copy touches them). Bytes past the end
 * of the file stay zero. */
static int fill_from_file(struct file *f, uint64_t base, size_t len, uint64_t off)
{
    void *buf = kmalloc(PAGE_SIZE, 0);
    if (buf == NULL)
        return -ENOMEM;
    int rc = 0;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < PAGE_SIZE ? len - done : PAGE_SIZE;
        int64_t n = file_pread(f, buf, chunk, off + done);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if (n == 0)
            break;   /* end of file: the rest is zero */
        if (copy_to_user(base + done, buf, (size_t)n)) {
            rc = -ENOMEM;   /* the region is ours and mapped: only memory can fail the copy */
            break;
        }
        done += (size_t)n;
    }
    kfree(buf);
    return rc;
}

static int64_t lx_mmap(struct syscall_args *a)
{
    uint64_t hint = a->a[0];
    size_t len = (size_t)a->a[1];
    unsigned prot = (unsigned)a->a[2], flags = (unsigned)a->a[3];
    uint64_t off = a->a[5];
    struct process *p = process_current();
    if (len == 0 || len > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    len = page_align_up(len);
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
    /* A file: MAP_PRIVATE, or MAP_SHARED without PROT_WRITE, is a snapshot
     * of the file's bytes (docs/compat/linux/design.md, "Dynamic
     * executables"); a writable shared mapping would need page-cache-backed
     * regions this kernel does not have. */
    struct file *f = NULL;
    if (!(flags & LX_MAP_ANONYMOUS)) {
        if ((flags & LX_MAP_SHARED) && (nprot & COSMO_PROT_WRITE))
            return -EOPNOTSUPP;
        if (!is_page_aligned(off))
            return -EINVAL;
        f = file_of((int)a->a[4], HANDLE_RIGHT_READ);
        if (f == NULL)
            return -EBADF;
    }
    uint64_t base;
    int rc;
    if (flags & LX_MAP_FIXED) {
        if (!is_page_aligned(hint) || !user_range_ok(hint, len)) {
            rc = -EINVAL;
            goto out;
        }
        rc = vm_user_unmap(p->space, hint, len, 0);   /* Linux replaces what was there */
        if (rc)
            goto out;
        base = hint;
    } else {
        uint64_t from = (hint >= USER_LO && is_page_aligned(hint)) ? hint : USER_MMAP_BASE;
        base = vm_user_find_free(p->space, from, len);
        if (base == 0 && from != USER_MMAP_BASE)
            base = vm_user_find_free(p->space, USER_MMAP_BASE, len);
        if (base == 0) {
            rc = -ENOMEM;
            goto out;
        }
    }
    rc = vm_user_map_anon(p->space, base, len, f ? VM_PROT_RW : vprot, 0, f ? "mmap-file" : "mmap");
    if (rc)
        goto out;
    if (f) {
        rc = fill_from_file(f, base, len, off);
        if (rc == 0 && vprot != VM_PROT_RW)
            rc = vm_user_protect(p->space, base, len, vprot);
        if (rc) {
            vm_user_unmap(p->space, base, len, 0);
            goto out;
        }
    }
    rc = 0;
out:
    if (f)
        file_put(f);
    return rc ? rc : (int64_t)base;
}

static int64_t lx_munmap(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    if (!is_page_aligned(addr) || len == 0 || !user_range_ok(addr, page_align_up(len)))
        return -EINVAL;
    return vm_user_unmap(process_current()->space, addr, page_align_up(len), 0);
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
    if (!user_range_ok(addr, len))
        return -ENOMEM;
    return vm_user_protect(process_current()->space, addr, len, vprot);
}

static int64_t lx_madvise(struct syscall_args *a) { (void)a; return 0; }

/* --- process, identity, signals -------------------------------------------------- */

static int64_t lx_getpid(struct syscall_args *a) { (void)a; return process_current()->pid; }
static int64_t lx_getppid(struct syscall_args *a) { (void)a; return process_current()->parent_pid; }
static int64_t lx_getuid(struct syscall_args *a) { (void)a; return process_current()->cred.ruid; }
static int64_t lx_getgid(struct syscall_args *a) { (void)a; return process_current()->cred.rgid; }
static int64_t lx_geteuid(struct syscall_args *a) { (void)a; return process_current()->cred.euid; }
static int64_t lx_getegid(struct syscall_args *a) { (void)a; return process_current()->cred.egid; }

/* Linux ids are 32-bit with -1 meaning "keep"; the kernel takes int64. */
static int64_t lx_id(uint64_t v)
{
    return (uint32_t)v == 0xFFFFFFFFu ? -1 : (int64_t)(uint32_t)v;
}

/* --- rlimits: one value per resource, reported as cur == max ------------------ */

/* The native resource for a Linux one, or -1 for those the kernel does not
 * bound (they read as infinity and accept any value). */
static int lx_rlimit_map(unsigned res)
{
    switch (res) {
    case LX_RLIMIT_AS: return COSMO_RLIMIT_AS;
    case LX_RLIMIT_RSS: return COSMO_RLIMIT_MEM;
    case LX_RLIMIT_NOFILE: return COSMO_RLIMIT_NOFILE;
    case LX_RLIMIT_NPROC: return COSMO_RLIMIT_NPROC;
    default: return -1;
    }
}

static int64_t lx_rlimit_get(unsigned res, uint64_t out)
{
    if (res >= LX_RLIM_NLIMITS)
        return -EINVAL;
    struct lx_rlimit r = { LX_RLIM_INFINITY, LX_RLIM_INFINITY };
    int native = lx_rlimit_map(res);
    if (native >= 0) {
        uint64_t v;
        int rc = process_getrlimit((unsigned)native, &v);
        if (rc)
            return rc;
        r.rlim_cur = r.rlim_max = v == COSMO_RLIM_INFINITY ? LX_RLIM_INFINITY : v;
    }
    return copy_to_user(out, &r, sizeof(r));
}

static int64_t lx_rlimit_set(unsigned res, uint64_t in)
{
    if (res >= LX_RLIM_NLIMITS)
        return -EINVAL;
    struct lx_rlimit r;
    if (copy_from_user(&r, in, sizeof(r)))
        return -EFAULT;
    if (r.rlim_cur > r.rlim_max)
        return -EINVAL;
    int native = lx_rlimit_map(res);
    if (native < 0)
        return 0;   /* not enforced here: accepted and ignored */
    /* One value: the maximum is what binds later raises, so it is the
     * value stored; a maximum above the current limit needs privilege. */
    return process_setrlimit((unsigned)native, r.rlim_max == LX_RLIM_INFINITY ? COSMO_RLIM_INFINITY : r.rlim_max);
}

static int64_t lx_getrlimit(struct syscall_args *a) { return lx_rlimit_get((unsigned)a->a[0], a->a[1]); }
static int64_t lx_setrlimit(struct syscall_args *a) { return lx_rlimit_set((unsigned)a->a[0], a->a[1]); }

static int64_t lx_prlimit64(struct syscall_args *a)
{
    pid_t pid = (pid_t)a->a[0];
    if (pid != 0 && pid != process_current()->pid)
        return -EPERM;   /* other processes' limits are theirs */
    if (a->a[3] != 0) {
        int64_t rc = lx_rlimit_get((unsigned)a->a[1], a->a[3]);
        if (rc)
            return rc;
    }
    if (a->a[2] != 0)
        return lx_rlimit_set((unsigned)a->a[1], a->a[2]);
    return 0;
}

static int64_t lx_setresuid(struct syscall_args *a)
{
    return process_setresuid(lx_id(a->a[0]), lx_id(a->a[1]), lx_id(a->a[2]));
}

static int64_t lx_setresgid(struct syscall_args *a)
{
    return process_setresgid(lx_id(a->a[0]), lx_id(a->a[1]), lx_id(a->a[2]));
}

/* setuid(u): root sets all three; anyone else sets the effective id to a
 * real or saved one. setgid likewise. */
static int64_t lx_setuid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    int64_t u = lx_id(a->a[0]);
    return cred_privileged(c) ? process_setresuid(u, u, u) : process_setresuid(-1, u, -1);
}

static int64_t lx_setgid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    int64_t g = lx_id(a->a[0]);
    return cred_privileged(c) ? process_setresgid(g, g, g) : process_setresgid(-1, g, -1);
}

/* setreuid(r, e): the saved id follows the effective one whenever the
 * real id is set or the effective id becomes something other than the
 * old real id (POSIX). */
static int64_t lx_setreuid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    int64_t r = lx_id(a->a[0]), e = lx_id(a->a[1]);
    int64_t s = (r != -1 || (e != -1 && (uint32_t)e != c->ruid)) ? (e != -1 ? e : (int64_t)c->euid) : -1;
    return process_setresuid(r, e, s);
}

static int64_t lx_setregid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    int64_t r = lx_id(a->a[0]), e = lx_id(a->a[1]);
    int64_t s = (r != -1 || (e != -1 && (uint32_t)e != c->rgid)) ? (e != -1 ? e : (int64_t)c->egid) : -1;
    return process_setresgid(r, e, s);
}

static int64_t lx_getres3(struct syscall_args *a, uint32_t r, uint32_t e, uint32_t s)
{
    if (copy_to_user(a->a[0], &r, 4) || copy_to_user(a->a[1], &e, 4) || copy_to_user(a->a[2], &s, 4))
        return -EFAULT;
    return 0;
}

static int64_t lx_getresuid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    return lx_getres3(a, c->ruid, c->euid, c->suid);
}

static int64_t lx_getresgid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    return lx_getres3(a, c->rgid, c->egid, c->sgid);
}

static int64_t lx_getgroups(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    int n = (int)a->a[0];
    if (n < 0)
        return -EINVAL;
    if (n == 0)
        return c->ngroups;
    if ((unsigned)n < c->ngroups)
        return -EINVAL;
    if (c->ngroups && copy_to_user(a->a[1], c->groups, c->ngroups * 4))
        return -EFAULT;
    return c->ngroups;
}

static int64_t lx_setgroups(struct syscall_args *a)
{
    size_t n = (size_t)a->a[0];
    if (n > CRED_NGROUPS_MAX)
        return -EINVAL;
    uint32_t groups[CRED_NGROUPS_MAX];
    if (n && copy_from_user(groups, a->a[1], n * 4))
        return -EFAULT;
    return process_setgroups(groups, (unsigned)n);
}
static int64_t lx_zero(struct syscall_args *a) { (void)a; return 0; }
static int64_t lx_nosys(struct syscall_args *a) { (void)a; return -ENOSYS; }
static __maybe_unused int64_t lx_getpgrp(struct syscall_args *a) { (void)a; return process_current()->pid; }

static __maybe_unused int64_t lx_arch_prctl(struct syscall_args *a)
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

/* --- time and misc ---------------------------------------------------------------- */

/* CLOCK_REALTIME and its coarse variant read the wall clock (the RTC at
 * boot plus the monotonic clock, kernel/timer); every other clock is the
 * monotonic one (docs/compat/linux/design.md, "Wall clock"). */
static bool clock_is_realtime(unsigned clk)
{
    return clk == LX_CLOCK_REALTIME || clk == LX_CLOCK_REALTIME_COARSE;
}

static uint64_t clock_read(unsigned clk)
{
    return clock_is_realtime(clk) ? clock_realtime_ns() : clock_now_ns();
}

static int64_t lx_clock_gettime(struct syscall_args *a)
{
    unsigned clk = (unsigned)a->a[0];
    if (clk > LX_CLOCK_BOOTTIME)
        return -EINVAL;
    return put_timespec(a->a[1], clock_read(clk));
}

static int64_t lx_gettimeofday(struct syscall_args *a)
{
    if (a->a[0]) {
        uint64_t ns = clock_realtime_ns();
        struct lx_timeval tv = { .tv_sec = (int64_t)(ns / 1000000000ull), .tv_usec = (int64_t)(ns % 1000000000ull / 1000) };
        if (copy_to_user(a->a[0], &tv, sizeof(tv)))
            return -EFAULT;
    }
    return 0;
}

static __maybe_unused int64_t lx_time(struct syscall_args *a)
{
    int64_t t = (int64_t)(clock_realtime_ns() / 1000000000ull);
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
        uint64_t now = clock_read(clk);
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
    strlcpy(u.machine, LX_MACHINE, sizeof(u.machine));
    strlcpy(u.domainname, "(none)", sizeof(u.domainname));
    return copy_to_user(a->a[0], &u, sizeof(u)) ? -EFAULT : 0;
}

static int64_t lx_futex(struct syscall_args *a)
{
    uint64_t uaddr = a->a[0];
    unsigned op = (unsigned)a->a[1] & (unsigned)LX_FUTEX_CMD_MASK;
    bool realtime = ((unsigned)a->a[1] & LX_FUTEX_CLOCK_REALTIME) != 0;
    uint32_t val = (uint32_t)a->a[2];
    struct vm_space *space = process_current()->space;
    if (!user_range_ok(uaddr, 4))
        return -EFAULT;
    switch (op) {
    case LX_FUTEX_WAIT:
    case LX_FUTEX_WAIT_BITSET: {
        /* WAIT's timeout is relative (monotonic); WAIT_BITSET's is absolute
         * on CLOCK_MONOTONIC, or CLOCK_REALTIME with the flag. The bitset is
         * a wake filter (glibc uses MATCH_ANY); only that value is supported. */
        if (op == LX_FUTEX_WAIT_BITSET && (uint32_t)a->a[5] != LX_FUTEX_BITSET_MATCH_ANY)
            return -ENOSYS;
        if (op == LX_FUTEX_WAIT && realtime)
            return -ENOSYS;
        uint64_t timeout = 0;
        if (a->a[3]) {
            int rc = ns_from_timespec(a->a[3], &timeout);
            if (rc)
                return rc;
            if (op == LX_FUTEX_WAIT_BITSET) {
                uint64_t now = realtime ? clock_realtime_ns() : clock_now_ns();
                if (timeout <= now) {
                    /* Already past: the value check still decides EAGAIN vs ETIMEDOUT. */
                    uint32_t cur;
                    if (copy_from_user(&cur, uaddr, sizeof(cur)))
                        return -EFAULT;
                    return cur != val ? -EAGAIN : -ETIMEDOUT;
                }
                timeout -= now;
            }
            if (timeout == 0)
                timeout = 1;
        }
        return futex_wait(space, uaddr, val, timeout);
    }
    case LX_FUTEX_WAKE:
    case LX_FUTEX_WAKE_BITSET:
        if (op == LX_FUTEX_WAKE_BITSET && (uint32_t)a->a[5] != LX_FUTEX_BITSET_MATCH_ANY)
            return -ENOSYS;
        return futex_wake(space, uaddr, val);
    case LX_FUTEX_REQUEUE:
    case LX_FUTEX_CMP_REQUEUE: {
        uint64_t uaddr2 = a->a[4];
        if (!user_range_ok(uaddr2, 4))
            return -EFAULT;
        unsigned nr_requeue = (unsigned)a->a[3];
        int rc = futex_requeue(space, uaddr, uaddr2, val, nr_requeue, op == LX_FUTEX_CMP_REQUEUE, (uint32_t)a->a[5]);
        if (rc < 0)
            return rc;
        /* REQUEUE reports the woken only; CMP_REQUEUE woken + requeued. */
        return op == LX_FUTEX_REQUEUE ? (rc < (int)val ? rc : (int)val) : rc;
    }
    default:
        return -ENOSYS;
    }
}

/* --- poll, ppoll (kernel/io/poll.c) ------------------------------------------- */

static unsigned poll_events_to_io(int16_t ev)
{
    unsigned io = 0;
    if (ev & (LX_POLLIN | LX_POLLRDNORM | LX_POLLPRI))
        io |= COSMO_IO_READABLE;
    if (ev & (LX_POLLOUT | LX_POLLWRNORM))
        io |= COSMO_IO_WRITABLE;
    return io;
}

static int16_t poll_events_from_io(unsigned io, int16_t asked)
{
    int16_t ev = 0;
    if (io & COSMO_IO_READABLE)
        ev |= (int16_t)(asked & (LX_POLLIN | LX_POLLRDNORM | LX_POLLPRI));
    if (io & COSMO_IO_WRITABLE)
        ev |= (int16_t)(asked & (LX_POLLOUT | LX_POLLWRNORM));
    if (io & COSMO_IO_HANGUP)
        ev |= LX_POLLHUP | (int16_t)(asked & LX_POLLRDHUP);
    if (io & COSMO_IO_ERROR)
        ev |= LX_POLLERR;
    return ev;
}

/* The shared body: `timeout_ns` already decided (IO_POLL_FOREVER: none). */
static int64_t do_poll(uint64_t ufds, unsigned n, uint64_t timeout_ns)
{
    if (n > LX_POLL_MAX)
        return -EINVAL;
    struct lx_pollfd *pfds = NULL;
    struct io_pollfd *fds = NULL;
    if (n) {
        pfds = kmalloc(n * sizeof(*pfds), 0);
        fds = kmalloc(n * sizeof(*fds), KMEM_ZERO);
        if (pfds == NULL || fds == NULL) {
            kfree(pfds);
            kfree(fds);
            return -ENOMEM;
        }
        if (copy_from_user(pfds, ufds, n * sizeof(*pfds))) {
            kfree(pfds);
            kfree(fds);
            return -EFAULT;
        }
    }
    /* Resolve the handles once; a closed one is POLLNVAL at once. */
    int nval = 0;
    struct process *p = process_current();
    for (unsigned i = 0; i < n; i++) {
        pfds[i].revents = 0;
        if (pfds[i].fd < 0)
            continue;
        unsigned rights;
        struct kobject *obj = handle_get(&p->handles, pfds[i].fd, &rights);
        if (obj == NULL || kobject_io_of(obj) == NULL) {
            if (obj)
                kobject_put(obj);
            pfds[i].revents = LX_POLLNVAL;
            nval++;
            continue;
        }
        fds[i].obj = obj;
        fds[i].events = poll_events_to_io(pfds[i].events);
    }
    int64_t rc;
    if (nval) {
        rc = io_poll(fds, n, 0) + nval;
    } else {
        rc = io_poll(fds, n, timeout_ns);
    }
    for (unsigned i = 0; i < n; i++) {
        if (fds[i].obj) {
            if (rc >= 0)
                pfds[i].revents = poll_events_from_io(fds[i].revents, pfds[i].events);
            kobject_put(fds[i].obj);
        }
    }
    if (rc >= 0 && n && copy_to_user(ufds, pfds, n * sizeof(*pfds)))
        rc = -EFAULT;
    kfree(pfds);
    kfree(fds);
    return rc;
}

static __maybe_unused int64_t lx_poll(struct syscall_args *a)
{
    int timeout_ms = (int)a->a[2];
    uint64_t timeout_ns = timeout_ms < 0 ? IO_POLL_FOREVER : (uint64_t)timeout_ms * 1000000ull;
    return do_poll(a->a[0], (unsigned)a->a[1], timeout_ns);
}

/* ppoll: a timespec (NULL: forever) and a signal mask swapped in for the
 * wait (the core's saved-mask rule restores it, or a handler's frame
 * records it, exactly as rt_sigsuspend). */
static int64_t lx_ppoll(struct syscall_args *a)
{
    uint64_t timeout_ns = IO_POLL_FOREVER;
    if (a->a[2]) {
        int rc = ns_from_timespec(a->a[2], &timeout_ns);
        if (rc)
            return rc;
    }
    bool swap = a->a[3] != 0;
    uint64_t mask = 0, old = 0;
    if (swap) {
        if (a->a[4] != 8)
            return -EINVAL;
        if (copy_from_user(&mask, a->a[3], 8))
            return -EFAULT;
        old = signal_blocked();
        signal_set_blocked(mask);
    }
    int64_t rc = do_poll(a->a[0], (unsigned)a->a[1], timeout_ns);
    if (swap)
        signal_set_blocked_saved(old);
    return rc;
}

/* --- threads: clone (the thread set only), sched_getaffinity ------------------- */

static int64_t lx_clone(struct syscall_args *a)
{
    uint64_t flags = a->a[0] & ~0xffull;   /* the low byte is the exit signal; CLONE_THREAD ignores it */
    uint64_t newsp = a->a[1], ptid = a->a[2];
#if defined(ARCH_X86_64)
    uint64_t ctid = a->a[3], tls = a->a[4];
#else
    uint64_t tls = a->a[3], ctid = a->a[4];   /* AArch64 swaps the last two */
#endif
    if (!(flags & LX_CLONE_THREAD))
        return -ENOSYS;   /* a fork-like clone: no address-space copy exists (docs/compat/linux/design.md) */
    if ((flags & LX_CLONE_THREAD_REQUIRED) != LX_CLONE_THREAD_REQUIRED || (flags & ~LX_CLONE_THREAD_ALLOWED))
        return -EINVAL;   /* Linux: CLONE_THREAD needs CLONE_SIGHAND, which needs CLONE_VM */
    if ((flags & LX_CLONE_PARENT_SETTID) && !user_range_ok(ptid, 4))
        return -EFAULT;
    if ((flags & (LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID)) && !user_range_ok(ctid, 4))
        return -EFAULT;
    struct process *p = process_current();
    struct thread *cur = thread_current();

    /* The child is the caller at this instant: the same registers, the
     * result 0, its own stack and thread pointer. */
    struct arch_user_regs regs;
    arch_user_regs_from_syscall(a->frame, &regs);
    arch_user_regs_set_result(&regs, 0);
    if (newsp)
        arch_user_regs_set_sp(&regs, (uintptr_t)newsp);
    uintptr_t tls_base = (flags & LX_CLONE_SETTLS) ? (uintptr_t)tls : cur->tls_base;

    struct thread *t;
    int rc = process_add_thread(p, &regs, tls_base, &t);
    if (rc)
        return rc;
    uint32_t tid = t->lx_tid;
    if (flags & LX_CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid;
    /* The tid words are written before the child can run: a joiner that
     * reads the word right after clone returns sees the tid or 0, never
     * the stale value racing the child's exit. */
    if (((flags & LX_CLONE_PARENT_SETTID) && copy_to_user(ptid, &tid, sizeof(tid))) ||
        ((flags & LX_CLONE_CHILD_SETTID) && copy_to_user(ctid, &tid, sizeof(tid)))) {
        process_thread_abandon(t);
        return -EFAULT;
    }
    process_thread_start(t);
    return tid;
}

/* Accepted and ignored: threads run on any CPU (docs/compat/linux/design.md). */
static int64_t lx_sched_setaffinity(struct syscall_args *a)
{
    if (a->a[1] < sizeof(uint64_t) || !user_range_ok(a->a[2], sizeof(uint64_t)))
        return -EINVAL;
    return 0;
}

static int64_t lx_sched_getaffinity(struct syscall_args *a)
{
    int pid = (int)a->a[0];
    size_t len = (size_t)a->a[1];
    struct process *cur = process_current();
    if (pid != 0 && (uint32_t)pid != cur->pid && process_find_thread(cur, (uint32_t)pid) == NULL) {
        struct process *other = process_lookup((pid_t)pid);
        if (other == NULL)
            return -ESRCH;
        process_put(other);   /* every process may run on every CPU: one answer */
    }
    if (len < sizeof(uint64_t))
        return -EINVAL;
    uint64_t mask = cpu_online_mask();
    if (copy_to_user(a->a[2], &mask, sizeof(mask)))
        return -EFAULT;
    return (int64_t)sizeof(mask);
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
    bool nonblock = ((unsigned)a->a[1] & LX_SOCK_NONBLOCK) != 0;
    unsigned type = (unsigned)a->a[1] & ~(unsigned)(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC);
    if (family != LX_AF_INET && family != LX_AF_INET6)
        return -EAFNOSUPPORT;
    if (type != LX_SOCK_STREAM && type != LX_SOCK_DGRAM)
        return -EINVAL;
    struct socket *s;
    int rc = ksock_create(family == LX_AF_INET ? COSMO_AF_INET : COSMO_AF_INET6,
                          type == LX_SOCK_STREAM ? COSMO_SOCK_STREAM : COSMO_SOCK_DGRAM, process_current()->cred.euid,
                          &s);
    if (rc)
        return rc;
    if (nonblock)
        ksock_set_nonblock(s, true);
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
    if (a->a[3] & LX_SOCK_NONBLOCK)   /* accept4 flags; accept passes 0 */
        ksock_set_nonblock(c, true);
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
#ifdef LX_open
    [LX_open] = lx_open,
#endif
    [LX_close] = lx_close,
#ifdef LX_stat
    [LX_stat] = lx_stat,
#endif
    [LX_fstat] = lx_fstat,
#ifdef LX_lstat
    [LX_lstat] = lx_stat,
#endif
    [LX_lseek] = lx_lseek,
    [LX_mmap] = lx_mmap,
    [LX_mprotect] = lx_mprotect,
    [LX_munmap] = lx_munmap,
    [LX_brk] = lx_brk,
    [LX_rt_sigaction] = lx_rt_sigaction,
    [LX_rt_sigprocmask] = lx_rt_sigprocmask,
    [LX_rt_sigreturn] = lx_rt_sigreturn,
    [LX_rt_sigpending] = lx_rt_sigpending,
    [LX_rt_sigsuspend] = lx_rt_sigsuspend,
#ifdef LX_pause
    [LX_pause] = lx_pause,
#endif
#ifdef LX_poll
    [LX_poll] = lx_poll,
#endif
    [LX_ppoll] = lx_ppoll,
    [LX_tkill] = lx_tkill,
    [LX_ioctl] = lx_ioctl,
    [LX_pread64] = lx_pread64,
    [LX_pwrite64] = lx_pwrite64,
    [LX_readv] = lx_readv,
    [LX_writev] = lx_writev,
#ifdef LX_access
    [LX_access] = lx_access,
#endif
#ifdef LX_pipe
    [LX_pipe] = lx_pipe,
#endif
    [LX_sched_yield] = lx_sched_yield,
    [LX_madvise] = lx_madvise,
    [LX_dup] = lx_dup,
#ifdef LX_dup2
    [LX_dup2] = lx_dup2,
#endif
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
    [LX_clone] = lx_clone,
#ifdef LX_fork
    [LX_fork] = lx_nosys,
#endif
#ifdef LX_vfork
    [LX_vfork] = lx_nosys,
#endif
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
#ifdef LX_rename
    [LX_rename] = lx_rename,
#endif
#ifdef LX_mkdir
    [LX_mkdir] = lx_mkdir,
#endif
#ifdef LX_rmdir
    [LX_rmdir] = lx_rmdir,
#endif
#ifdef LX_creat
    [LX_creat] = lx_creat,
#endif
#ifdef LX_unlink
    [LX_unlink] = lx_unlink,
#endif
#ifdef LX_readlink
    [LX_readlink] = lx_nosys,
#endif
    [LX_umask] = lx_umask,
    [LX_gettimeofday] = lx_gettimeofday,
    [LX_getrlimit] = lx_getrlimit,
    [LX_sysinfo] = lx_nosys,
    [LX_getuid] = lx_getuid,
    [LX_getgid] = lx_getgid,
    [LX_geteuid] = lx_geteuid,
    [LX_getegid] = lx_getegid,
    [LX_setuid] = lx_setuid,
    [LX_setgid] = lx_setgid,
    [LX_setreuid] = lx_setreuid,
    [LX_setregid] = lx_setregid,
    [LX_getgroups] = lx_getgroups,
    [LX_setgroups] = lx_setgroups,
    [LX_setresuid] = lx_setresuid,
    [LX_getresuid] = lx_getresuid,
    [LX_setresgid] = lx_setresgid,
    [LX_getresgid] = lx_getresgid,
    [LX_setpgid] = lx_zero,
    [LX_getppid] = lx_getppid,
#ifdef LX_getpgrp
    [LX_getpgrp] = lx_getpgrp,
#endif
    [LX_setsid] = lx_getpgrp,
    [LX_sigaltstack] = lx_sigaltstack,
#ifdef LX_arch_prctl
    [LX_arch_prctl] = lx_arch_prctl,
#endif
    [LX_setrlimit] = lx_setrlimit,
    [LX_sync] = lx_sync,
    [LX_gettid] = lx_gettid,
#ifdef LX_time
    [LX_time] = lx_time,
#endif
    [LX_futex] = lx_futex,
    [LX_sched_setaffinity] = lx_sched_setaffinity,
    [LX_sched_getaffinity] = lx_sched_getaffinity,
    [LX_getdents64] = lx_getdents64,
    [LX_set_tid_address] = lx_set_tid_address,
    [LX_clock_gettime] = lx_clock_gettime,
    [LX_clock_nanosleep] = lx_clock_nanosleep,
    [LX_exit_group] = lx_exit_group,
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
    [LX_prlimit64] = lx_prlimit64,
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
    .signal_frame = linux_signal_frame,
    .thread_exit = linux_thread_exit,
};
