/*
 * native.c - The CosmoOS native personality: system-call table and
 * implementations. Numbers come from uapi/cosmo/syscall.h.
 *
 * Every function validates its arguments first and returns a negative
 * errno on failure. User memory is touched only through uaccess.h.
 */

#include <kernel/aio.h>
#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/hv.h>
#include <arch/cpu.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pipe.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/netif.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/uaccess.h>
#include <kernel/version.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>
#include <kernel/utsns.h>

#include <uapi/cosmo/syscall.h>

#define IO_CHUNK 1024   /* one console line (TTY_LINE_MAX) fits a single read */

static int64_t sys_exit(struct syscall_args *a)
{
    process_exit((int)a->a[0]);
}

int64_t syscall_obj_write(struct kobject *obj, const uint64_t ubuf, size_t len)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->write == NULL)
        return -EBADF;
    char tmp[IO_CHUNK];
    size_t done = 0;
    int64_t rc = 0;
    while (done < len) {
        size_t n = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        rc = copy_from_user(tmp, ubuf + done, n);
        if (rc)
            break;
        rc = io->write(obj, tmp, n);
        if (rc < 0)
            break;
        KASSERT(rc <= (int64_t)n);
        if (rc > (int64_t)n) {
            rc = -EIO;
            break;
        }
        done += (size_t)rc;
        if ((size_t)rc < n)
            break;
    }
    return done > 0 ? (int64_t)done : rc;
}

int64_t syscall_handle_write(int h, uint64_t ubuf, size_t len)
{
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_WRITE);
    if (obj == NULL)
        return -EBADF;
    int64_t rc = syscall_obj_write(obj, ubuf, len);
    kobject_put(obj);
    return rc;
}

static int64_t sys_write(struct syscall_args *a)
{
    return syscall_handle_write((int)a->a[0], a->a[1], (size_t)a->a[2]);
}

int64_t syscall_obj_read(struct kobject *obj, uint64_t ubuf, size_t len)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->read == NULL)
        return -EBADF;
    char tmp[IO_CHUNK];
    size_t n = len < IO_CHUNK ? len : IO_CHUNK;
    int64_t rc = io->read(obj, tmp, n);
    /* An object may never report more than it was offered; the count
     * bounds the copy out of the kernel stack buffer. */
    KASSERT(rc <= (int64_t)n);
    if (rc > (int64_t)n)
        rc = -EIO;
    if (rc > 0 && copy_to_user(ubuf, tmp, (size_t)rc))
        rc = -EFAULT;
    return rc;
}

int64_t syscall_handle_read(int h, uint64_t ubuf, size_t len)
{
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_READ);
    if (obj == NULL)
        return -EBADF;
    int64_t rc = syscall_obj_read(obj, ubuf, len);
    kobject_put(obj);
    return rc;
}

static int64_t sys_read(struct syscall_args *a)
{
    return syscall_handle_read((int)a->a[0], a->a[1], (size_t)a->a[2]);
}

static int64_t sys_getpid(struct syscall_args *a)
{
    (void)a;
    return (int64_t)process_current()->pid;
}

static int64_t sys_yield(struct syscall_args *a)
{
    (void)a;
    sched_yield();
    return 0;
}

static int64_t sys_sleep_ns(struct syscall_args *a)
{
    uint64_t ns = a->a[0];
    if (ns > 3600ULL * NS_PER_SEC)
        return -EINVAL;
    return thread_sleep_ns_killable(ns);
}

static int64_t sys_clock_ns(struct syscall_args *a)
{
    switch ((unsigned)a->a[0]) {
    case COSMO_CLOCK_MONOTONIC: return (int64_t)clock_now_ns();
    case COSMO_CLOCK_REALTIME:  return (int64_t)clock_realtime_ns();
    default:                    return -EINVAL;
    }
}

static int64_t sys_mmap(struct syscall_args *a)
{
    uint64_t hint = a->a[0];
    size_t len = (size_t)a->a[1];
    int prot = (int)a->a[2];
    int flags = (int)a->a[3];
    struct process *p = process_current();

    if (len == 0 || !is_page_aligned(len) || len > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    if (!(flags & COSMO_MAP_ANONYMOUS))
        return -EINVAL; /* file mappings arrive with the VFS */
    if (prot & ~(COSMO_PROT_READ | COSMO_PROT_WRITE | COSMO_PROT_EXEC))
        return -EINVAL;
    if ((prot & COSMO_PROT_WRITE) && (prot & COSMO_PROT_EXEC))
        return -EINVAL; /* W^X */

    vm_prot_t vprot = 0;
    if (prot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (prot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (prot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    /* PROT_NONE reserves the range: every access faults (design.md §6.2). */

    uint64_t base;
    if (flags & COSMO_MAP_FIXED) {
        if (!is_page_aligned(hint) || !user_range_ok(hint, len))
            return -EINVAL;
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
    if (rc)
        return rc;
    return (int64_t)base;
}

static int64_t sys_munmap(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    if (!is_page_aligned(addr) || len == 0 || !is_page_aligned(len) || !user_range_ok(addr, len))
        return -EINVAL;
    return vm_user_unmap(process_current()->space, addr, len, VM_UNMAP_STRICT);
}

static int64_t sys_log(struct syscall_args *a)
{
    uint64_t ustr = a->a[0];
    size_t len = (size_t)a->a[1];
    char buf[200];

    if (len >= sizeof(buf))
        return -EINVAL;
    if (!process_log_permitted())
        return -EAGAIN;   /* an unprivileged writer past its rate limit (docs/kernel/security/design.md §1) */
    int rc = copy_from_user(buf, ustr, len);
    if (rc)
        return rc;
    buf[len] = '\0';
    kinfo("pid %u: %s", process_current()->pid, buf);
    return 0;
}

static int64_t sys_close(struct syscall_args *a)
{
    return handle_close(&process_current()->handles, (int)a->a[0]);
}

/* --- Phase 7: files ------------------------------------------------------- */

static int get_path(uint64_t uptr, char *buf)
{
    int rc = strncpy_from_user(buf, uptr, VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    return buf[0] == '\0' ? -ENOENT : 0;
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

static int64_t sys_open(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    unsigned flags = (unsigned)a->a[1];
    uint32_t mode = (uint32_t)a->a[2];
    struct file *f;
    rc = vfs_open(process_current()->cwd, path, flags, mode, &f);
    if (rc)
        return rc;
    /* The access mode decides read and write; opening a file is what
     * makes the caller its owner, so it may also copy, pass on and
     * administer the handle. */
    unsigned rights = HANDLE_RIGHT_OWNER;
    unsigned acc = flags & COSMO_O_ACCMODE;
    if (acc == COSMO_O_RDONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_READ;
    if (acc == COSMO_O_WRONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_WRITE;
    int h = handle_install(&process_current()->handles, &f->obj, rights);
    file_put(f);   /* the table holds its own reference */
    return h;
}

static int64_t sys_stat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    rc = vfs_stat(process_current()->cwd, path, &st);
    if (rc)
        return rc;
    return copy_to_user(a->a[1], &st, sizeof(st)) ? -EFAULT : 0;
}

int syscall_handle_stat(int h, struct cosmo_stat *st)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, 0);
    if (obj == NULL)
        return -EBADF;
    struct file *f = file_from_kobject(obj);
    int rc = 0;
    if (f) {
        file_stat(f, st);
    } else {
        const struct kobject_io_type *io = kobject_io_of(obj);
        if (io == NULL || io->stat == NULL)
            rc = -EBADF;
        else
            rc = io->stat(obj, st);
    }
    kobject_put(obj);
    return rc;
}

static int64_t sys_fstat(struct syscall_args *a)
{
    struct cosmo_stat st;
    int rc = syscall_handle_stat((int)a->a[0], &st);
    if (rc)
        return rc;
    return copy_to_user(a->a[1], &st, sizeof(st)) ? -EFAULT : 0;
}

static int64_t sys_lseek(struct syscall_args *a)
{
    struct file *f = file_of((int)a->a[0], 0);
    if (f == NULL)
        return -EBADF;
    int64_t rc = file_seek(f, (int64_t)a->a[1], (int)a->a[2]);
    file_put(f);
    return rc;
}

static int64_t sys_mkdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : vfs_mkdir(process_current()->cwd, path, (uint32_t)a->a[1]);
}

static int64_t sys_unlink(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : vfs_unlink(process_current()->cwd, path);
}

static int64_t sys_rmdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : vfs_rmdir(process_current()->cwd, path);
}

static int64_t sys_rename(struct syscall_args *a)
{
    char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
    int rc = get_path(a->a[0], oldp);
    if (rc)
        return rc;
    rc = get_path(a->a[1], newp);
    return rc ? rc : vfs_rename(process_current()->cwd, oldp, newp);
}

static int64_t sys_getdents(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (len > 65536)
        len = 65536;
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct file *f = file_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (f == NULL)
        return -EBADF;
    void *tmp = kmalloc(len, 0);
    if (tmp == NULL) {
        file_put(f);
        return -ENOMEM;
    }
    int64_t n = file_readdir(f, tmp, len);
    file_put(f);
    if (n > 0 && copy_to_user(ubuf, tmp, (size_t)n))
        n = -EFAULT;
    kfree(tmp);
    return n;
}

static int64_t sys_sync(struct syscall_args *a)
{
    (void)a;
    return vfs_sync();
}

/* Commit one file, not every mount: fsync(fd). Unlike SYS_sync, a caller
 * (e.g. a VM owner honouring a guest's virtio-blk flush) cannot force
 * synchronous commits of filesystems it has nothing to do with. */
static int64_t sys_fsync(struct syscall_args *a)
{
    struct file *f = file_of((int)a->a[0], 0);
    if (f == NULL)
        return -EBADF;
    int rc = file_sync(f);
    file_put(f);
    return rc;
}

static int64_t sys_mount(struct syscall_args *a)
{
    if (!cred_privileged(cred_current()))
        return -EPERM;
    char source[BLKDEV_NAME_MAX], target[VFS_PATH_MAX], fstype[16];
    int rc = strncpy_from_user(source, a->a[0], sizeof(source));
    if (rc < 0)
        return rc;
    rc = get_path(a->a[1], target);
    if (rc)
        return rc;
    rc = strncpy_from_user(fstype, a->a[2], sizeof(fstype));
    if (rc < 0)
        return rc;
    unsigned flags = (unsigned)a->a[3] & COSMO_MOUNT_RDONLY;
    struct blkdev *bd = NULL;
    if (source[0] != '\0' && strcmp(source, "none") != 0) {
        bd = blk_find(source);
        if (bd == NULL)
            return -ENODEV;
    }
    rc = vfs_mount(target, fstype, bd, flags);
    if (bd)
        blkdev_put(bd);   /* the mount took its own reference */
    return rc;
}

static int64_t sys_umount(struct syscall_args *a)
{
    if (!cred_privileged(cred_current()))
        return -EPERM;
    char target[VFS_PATH_MAX];
    int rc = get_path(a->a[0], target);
    unsigned flags = (unsigned)a->a[1] & COSMO_UMOUNT_FORCE;
    return rc ? rc : vfs_umount2(target, flags ? VFS_UMOUNT_FORCE : 0);
}

/* --- Phase 8: sockets ------------------------------------------------------- */

/*
 * `err` distinguishes a handle that is not there from one that is and
 * does not carry the right asked for. The caller is entitled to tell
 * those apart: EBADF is what POSIX says of a descriptor that is not
 * open, and a per-type right the holder was never given is EPERM --
 * an operation POSIX has no opinion about (S9).
 */
static struct socket *sock_of_err(int h, unsigned rights, int *err)
{
    unsigned have = 0;
    struct kobject *obj = handle_get(&process_current()->handles, h, &have);
    if (obj == NULL) {
        *err = -EBADF;
        return NULL;
    }
    /* The type first, then its rights. A bit in the upper half means
     * whatever the object's kind says, so asking for one of a handle
     * that is not a socket is not a rights failure -- it is the wrong
     * handle, and answering EPERM would say the bit meant something
     * here. */
    struct socket *s = socket_from_kobject(obj);
    if (s == NULL) {
        kobject_put(obj);
        *err = -EBADF;
        return NULL;
    }
    if ((have & rights) != rights) {
        ksock_put(s);
        *err = -EPERM;
        return NULL;
    }
    return s;
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

static int addr_from_user(uint64_t uptr, size_t len, struct netaddr *out)
{
    struct cosmo_sockaddr sa;
    if (len < sizeof(sa))
        return -EINVAL;
    if (copy_from_user(&sa, uptr, sizeof(sa)))
        return -EFAULT;
    return netaddr_from_user_shape(out, &sa);
}

static int addr_to_user(uint64_t uptr, uint64_t ulen, const struct netaddr *a)
{
    if (uptr == 0)
        return 0;
    struct cosmo_sockaddr sa;
    netaddr_to_user_shape(&sa, a);
    /* The caller's length bounds the copy (a short buffer gets a prefix);
     * the full size is reported back, as POSIX does. */
    size_t room = sizeof(sa);
    if (ulen && copy_from_user(&room, ulen, sizeof(room)))
        return -EFAULT;
    if (room > sizeof(sa))
        room = sizeof(sa);
    if (room && copy_to_user(uptr, &sa, room))
        return -EFAULT;
    size_t len = sizeof(sa);
    if (ulen && copy_to_user(ulen, &len, sizeof(len)))
        return -EFAULT;
    return 0;
}

static int64_t sys_socket(struct syscall_args *a)
{
    struct socket *s;
    int type = (int)a->a[1];
    bool nonblock = (type & COSMO_SOCK_NONBLOCK) != 0;
    int rc = ksock_create((int)a->a[0], type & ~COSMO_SOCK_NONBLOCK, process_current()->cred.euid, &s);
    if (rc)
        return rc;
    if (nonblock)
        ksock_set_nonblock(s, true);
    int h = handle_install(&process_current()->handles, &s->obj, HANDLE_RIGHT_SOCK_ALL);
    ksock_put(s);
    return h;
}

static int64_t sys_bind(struct syscall_args *a)
{
    struct netaddr addr;
    int rc = addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_BIND, &serr);
    if (s == NULL)
        return serr;
    rc = ksock_bind(s, &addr);
    ksock_put(s);
    return rc;
}

static int64_t sys_listen(struct syscall_args *a)
{
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_BIND, &serr);
    if (s == NULL)
        return serr;
    int rc = ksock_listen(s, (int)a->a[1]);
    ksock_put(s);
    return rc;
}

static int64_t sys_accept(struct syscall_args *a)
{
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_ACCEPT, &serr);
    if (s == NULL)
        return serr;
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
    /* What an established connection can use, and not the three that
     * name things it cannot do (architecture.md, "The upper sixteen
     * bits"). */
    int h = handle_install(&process_current()->handles, &c->obj, HANDLE_RIGHT_SOCK_CONNECTED);
    ksock_put(c);
    return h;
}

static int64_t sys_connect(struct syscall_args *a)
{
    struct netaddr addr;
    int rc = addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_CONNECT, &serr);
    if (s == NULL)
        return serr;
    rc = ksock_connect(s, &addr);
    ksock_put(s);
    return rc;
}

static int64_t sys_sendto(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct netaddr to;
    bool have_to = a->a[3] != 0;
    if (have_to) {
        int rc = addr_from_user(a->a[3], (size_t)a->a[4], &to);
        if (rc)
            return rc;
    }
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (s == NULL)
        return -EBADF;
    uint8_t *tmp = kmalloc(len < SOCK_IO_CHUNK ? (len ? len : 1) : SOCK_IO_CHUNK, 0);
    if (tmp == NULL) {
        ksock_put(s);
        return -ENOMEM;
    }
    int64_t done = 0, rc = 0;
    if (s->type == COSMO_SOCK_DGRAM) {
        if (len > SOCK_IO_CHUNK * 16) {
            rc = -EMSGSIZE;
        } else {
            kfree(tmp);
            tmp = kmalloc(len ? len : 1, 0);
            if (tmp == NULL)
                rc = -ENOMEM;
            else if (copy_from_user(tmp, ubuf, len))
                rc = -EFAULT;
            else
                rc = ksock_sendto(s, tmp, len, have_to ? &to : NULL);
            done = rc > 0 ? rc : 0;
        }
    } else {
        while ((size_t)done < len) {
            size_t n = len - (size_t)done < SOCK_IO_CHUNK ? len - (size_t)done : SOCK_IO_CHUNK;
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

static int64_t sys_recvfrom(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (s == NULL)
        return -EBADF;
    size_t chunk = len < SOCK_IO_CHUNK * 16 ? len : SOCK_IO_CHUNK * 16;
    uint8_t *tmp = kmalloc(chunk ? chunk : 1, 0);
    if (tmp == NULL) {
        ksock_put(s);
        return -ENOMEM;
    }
    struct netaddr from;
    int64_t n = ksock_recvfrom(s, tmp, chunk, &from);
    ksock_put(s);
    if (n > 0 && copy_to_user(ubuf, tmp, (size_t)n))
        n = -EFAULT;
    kfree(tmp);
    if (n >= 0 && a->a[3]) {
        int rc = addr_to_user(a->a[3], a->a[4], &from);
        if (rc)
            return rc;
    }
    return n;
}

static int64_t sys_shutdown(struct syscall_args *a)
{
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_SHUTDOWN, &serr);
    if (s == NULL)
        return serr;
    int rc = ksock_shutdown(s, (int)a->a[1]);
    ksock_put(s);
    return rc;
}

static int64_t sys_getsockname(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    struct netaddr addr;
    int rc = ksock_getsockname(s, &addr);
    ksock_put(s);
    return rc ? rc : addr_to_user(a->a[1], a->a[2], &addr);
}

/* --- Phase 9: processes, pipes, cwd, introspection ---------------------------- */

struct spawn_copy {
    char path[VFS_PATH_MAX];
    char cwd[VFS_PATH_MAX];
    const char *argv[COSMO_ARG_ENTRIES + 1];
    const char *envp[COSMO_ARG_ENTRIES + 1];
    char strings[COSMO_ARG_MAX];
    size_t used;
    struct process_handle_map map[HANDLE_TABLE_SIZE];
};

/* Copy a NULL-terminated pointer array and its strings. `*count` entries
 * are already used across argv and envp together. */
static int copy_strv(uint64_t uarr, const char **out, struct spawn_copy *sc, unsigned *count)
{
    unsigned n = 0;
    if (uarr == 0) {
        out[0] = NULL;
        return 0;
    }
    for (;;) {
        uint64_t uptr;
        if (copy_from_user(&uptr, uarr + (uint64_t)n * 8, 8))
            return -EFAULT;
        if (uptr == 0)
            break;
        if (*count >= COSMO_ARG_ENTRIES)
            return -E2BIG;
        size_t room = sizeof(sc->strings) - sc->used;
        if (room == 0)
            return -E2BIG;
        int len = strncpy_from_user(sc->strings + sc->used, uptr, room);
        if (len < 0)
            return len == -ENAMETOOLONG ? -E2BIG : len;
        out[n++] = sc->strings + sc->used;
        sc->used += (size_t)len + 1;
        (*count)++;
    }
    out[n] = NULL;
    return 0;
}

static int64_t sys_spawn(struct syscall_args *a)
{
    /* Copy what every caller has, decide from its flags whether it has
     * the field added after that, and only then read further: a program
     * built against the older header passes the older struct, and the
     * kernel must not read past what it gave. */
    struct cosmo_spawn req;
    memset(&req, 0, sizeof(req));
    if (copy_from_user(&req, a->a[0], COSMO_SPAWN_SIZE_V1))
        return -EFAULT;
    if ((req.flags &
         ~(COSMO_SPAWN_SETCRED | COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_SETROOT | COSMO_SPAWN_NEWDOMAIN |
           COSMO_SPAWN_NEWMOUNTNS | COSMO_SPAWN_NEWUTSNS | COSMO_SPAWN_SETPGID)) ||
        req.path == NULL || req.argv == NULL)
        return -EINVAL;
    if ((req.flags & (COSMO_SPAWN_SETROOT | COSMO_SPAWN_SETPGID)) &&
        copy_from_user(&req, a->a[0], (req.flags & COSMO_SPAWN_SETPGID) ? sizeof(req) : COSMO_SPAWN_SIZE_V2))
        return -EFAULT;
    struct process_spawn_cred cred = { .uid = req.uid, .gid = req.gid };
    if (req.nr_handles > HANDLE_TABLE_SIZE || (req.nr_handles != 0 && req.handles == NULL))
        return -EINVAL;
    struct spawn_copy *sc = kzalloc(sizeof(*sc));
    if (sc == NULL)
        return -ENOMEM;
    int rc = get_path((uint64_t)(uintptr_t)req.path, sc->path);
    if (rc)
        goto out;
    const char *cwd = NULL;
    if (req.cwd) {
        rc = get_path((uint64_t)(uintptr_t)req.cwd, sc->cwd);
        if (rc)
            goto out;
        cwd = sc->cwd;
    }
    unsigned count = 0;
    rc = copy_strv((uint64_t)(uintptr_t)req.argv, sc->argv, sc, &count);
    if (rc)
        goto out;
    rc = copy_strv((uint64_t)(uintptr_t)req.envp, sc->envp, sc, &count);
    if (rc)
        goto out;
    if (req.nr_handles) {
        STATIC_ASSERT(sizeof(struct process_handle_map) == sizeof(struct cosmo_spawn_handle), "handle map shape");
        if (req.flags & COSMO_SPAWN_HANDLE_RIGHTS) {
            if (copy_from_user(sc->map, (uint64_t)(uintptr_t)req.handles,
                               req.nr_handles * sizeof(struct cosmo_spawn_handle))) {
                rc = -EFAULT;
                goto out;
            }
        } else {
            /* The map as it was before rights existed: two ints per
             * entry, and the child gets what the caller holds. Reading
             * the wider element would take the next entry's child for
             * this one's rights. */
            struct legacy_handle {
                int child;
                int parent;
            };
            struct legacy_handle legacy[HANDLE_TABLE_SIZE];
            if (copy_from_user(legacy, (uint64_t)(uintptr_t)req.handles,
                               req.nr_handles * sizeof(struct legacy_handle))) {
                rc = -EFAULT;
                goto out;
            }
            for (unsigned i = 0; i < req.nr_handles; i++) {
                sc->map[i].child = legacy[i].child;
                sc->map[i].parent = legacy[i].parent;
                sc->map[i].rights = COSMO_RIGHTS_SAME;
                sc->map[i].pad = 0;
            }
        }
    }
    pid_t pid = 0;
    char rootbuf[VFS_PATH_MAX];
    const char *rootp = NULL;
    if (req.flags & COSMO_SPAWN_SETROOT) {
        if (req.root == NULL) {
            rc = -EINVAL;
            goto out;
        }
        if (strncpy_from_user(rootbuf, (uint64_t)(uintptr_t)req.root, sizeof(rootbuf)) < 0) {
            rc = -EFAULT;
            goto out;
        }
        rootp = rootbuf;
    }
    rc = process_spawn(sc->path, sc->argv, sc->envp, req.nr_handles ? sc->map : NULL, (unsigned)req.nr_handles, cwd,
                       rootp, (req.flags & COSMO_SPAWN_NEWDOMAIN) != 0,
                       (req.flags & COSMO_SPAWN_NEWMOUNTNS) != 0, (req.flags & COSMO_SPAWN_NEWUTSNS) != 0,
                       (req.flags & COSMO_SPAWN_SETCRED) ? &cred : NULL,
                       (req.flags & COSMO_SPAWN_SETPGID) != 0, (pid_t)req.pgid, &pid);
out:
    kfree(sc);
    return rc ? rc : (int64_t)pid;
}

static int64_t sys_wait(struct syscall_args *a)
{
    int pid = (int)a->a[0];
    unsigned flags = (unsigned)a->a[2];
    if (pid == 0 || pid < -1 || (flags & ~(COSMO_WNOHANG | COSMO_WUNTRACED | COSMO_WCONTINUED)))
        return -EINVAL;
    unsigned wf = 0;
    if (flags & COSMO_WNOHANG)
        wf |= PROCESS_WAIT_NOHANG;
    if (flags & COSMO_WUNTRACED)
        wf |= PROCESS_WAIT_UNTRACED;
    if (flags & COSMO_WCONTINUED)
        wf |= PROCESS_WAIT_CONTINUED;
    pid_t got = 0;
    int status = 0;
    int rc = process_wait_child(pid, wf, &got, &status);
    if (rc)
        return rc;
    if (got != 0 && a->a[1] != 0 && copy_to_user(a->a[1], &status, sizeof(status)))
        return -EFAULT;
    return got;
}

/* One target, every check. -ESRCH when the caller may not even see it,
 * -EPERM when it may see it and not signal it, 0 when `sig` is 0 and
 * the target passed both. */
static int kill_one(struct process *target, int sig)
{
    struct process *cur = process_current();
    /* A process outside domain 0 cannot reach another domain, and is
     * told the target does not exist rather than that it may not touch
     * it: -EPERM would confirm the pid is in use, which is the one
     * thing the domain is meant not to tell it. */
    if (cur && cur->domain != 0 && target->domain != cur->domain)
        return -ESRCH;
    if (!cred_may_signal(&cur->cred, &target->cred))
        return -EPERM;
    if (sig == 0)
        return 0;   /* it exists and could be signalled; nothing is sent */
    /* The signal core: a default-terminate signal ends the target
     * (128 + sig); default-ignore ones (SIGCHLD, ...) are discarded; a
     * process that installed a handler runs it. */
    struct signal_info info = { .sig = sig, .source = SIGSRC_USER, .sender_pid = cur->pid,
                                .sender_uid = cur->cred.ruid };
    return signal_send(target, sig, &info);
}

/*
 * A whole process group. POSIX's rule for the group form is that the
 * call succeeds if the signal reached anyone, so a shell interrupting a
 * job is not made to care that one member of it belongs to root: the
 * refusals only surface when every member refused.
 */
static int64_t kill_group(pid_t pgid, int sig)
{
    unsigned sent = 0, denied = 0;
    pid_t after = 0;
    struct process *p;
    while ((p = process_group_next(pgid, after)) != NULL) {
        after = p->pid;
        int rc = kill_one(p, sig);
        process_put(p);
        if (rc == 0)
            sent++;
        else if (rc == -EPERM)
            denied++;
    }
    if (sent > 0)
        return 0;
    return denied > 0 ? -EPERM : -ESRCH;
}

static int64_t sys_kill(struct syscall_args *a)
{
    int pid = (int)a->a[0];
    int sig = (int)a->a[1];
    /* Signal 0 sends nothing and reports whether the target exists and
     * could be signalled, which is what POSIX says of it and the only
     * way a supervisor can tell a live pid from a stale pid file
     * (docs/userland/design.md, "Services"). It goes through every
     * check below and stops before the delivery. */
    if (sig < 0 || sig >= COSMO_NSIG || pid == -1)
        return -EINVAL;   /* -1 is "every process", which nothing here wants */
    if (pid <= 0) {
        /* 0 is the caller's own group, a negative pid names one. */
        pid_t pgid;
        if (pid == 0) {
            int rc = process_getpgid(0, &pgid);
            if (rc)
                return rc;
        } else {
            pgid = (pid_t)(-pid);
        }
        return kill_group(pgid, sig);
    }
    struct process *target = process_lookup((pid_t)pid);
    if (target == NULL)
        return -ESRCH;
    int rc = kill_one(target, sig);
    process_put(target);
    return rc;
}

/* --- sessions, process groups and the terminal ------------------------------ */

static int64_t sys_setpgid(struct syscall_args *a)
{
    return process_setpgid((pid_t)(int)a->a[0], (pid_t)(int)a->a[1]);
}

static int64_t sys_getpgid(struct syscall_args *a)
{
    pid_t pgid;
    int rc = process_getpgid((pid_t)(int)a->a[0], &pgid);
    return rc ? rc : (int64_t)pgid;
}

static int64_t sys_setsid(struct syscall_args *a)
{
    (void)a;
    pid_t sid = 0;
    int rc = process_setsid(&sid);
    return rc ? rc : (int64_t)sid;
}

static int64_t sys_getsid(struct syscall_args *a)
{
    pid_t sid;
    int rc = process_getsid((pid_t)(int)a->a[0], &sid);
    return rc ? rc : (int64_t)sid;
}

/*
 * The tty behind a handle. There are two ways to hold one: the console
 * kobject that init inherits as handles 0, 1 and 2, and an open file on
 * `/dev/console` or `/dev/tty`. Both are terminals and both must answer
 * -- a program that opened `/dev/tty` because it had closed handle 0
 * would otherwise be told the thing it just opened is not a terminal.
 */
static struct tty *tty_of_handle(int h, int *err)
{
    unsigned rights = 0;
    struct kobject *obj = handle_get(&process_current()->handles, h, &rights);
    if (obj == NULL) {
        *err = -EBADF;
        return NULL;
    }
    struct tty *t = tty_of_open(obj);
    kobject_put(obj);
    if (t == NULL)
        *err = -ENOTTY;
    return t;
}

static int64_t sys_tcgetpgrp(struct syscall_args *a)
{
    int err = 0;
    struct tty *t = tty_of_handle((int)a->a[0], &err);
    if (t == NULL)
        return err;
    pid_t pgid = 0;
    int rc = tty_get_pgrp(t, &pgid);
    return rc ? rc : (int64_t)pgid;
}

static int64_t sys_tcgetattr(struct syscall_args *a)
{
    int err = 0;
    struct tty *t = tty_of_handle((int)a->a[0], &err);
    if (t == NULL)
        return err;
    struct cosmo_termios tio;
    tty_get_termios(t, &tio);
    return copy_to_user(a->a[1], &tio, sizeof(tio)) ? -EFAULT : 0;
}

static int64_t sys_tcsetattr(struct syscall_args *a)
{
    int err = 0;
    struct tty *t = tty_of_handle((int)a->a[0], &err);
    if (t == NULL)
        return err;
    struct cosmo_termios tio;
    if (copy_from_user(&tio, a->a[1], sizeof(tio)))
        return -EFAULT;
    /* A mode this kernel does not have is a program asking for
     * behaviour it will not get, which is worth refusing rather than
     * silently dropping. */
    if ((tio.modes & ~(uint32_t)COSMO_TTY_MODES) || tio.reserved != 0)
        return -EINVAL;
    tty_set_termios(t, &tio);
    return 0;
}

static int64_t sys_ttysize(struct syscall_args *a)
{
    int err = 0;
    struct tty *t = tty_of_handle((int)a->a[0], &err);
    if (t == NULL)
        return err;
    struct cosmo_ttysize sz;
    tty_get_size(t, &sz);
    return copy_to_user(a->a[1], &sz, sizeof(sz)) ? -EFAULT : 0;
}

static int64_t sys_tcsetpgrp(struct syscall_args *a)
{
    int err = 0;
    struct tty *t = tty_of_handle((int)a->a[0], &err);
    if (t == NULL)
        return err;
    int pgid = (int)a->a[1];
    if (pgid <= 0)
        return -EINVAL;
    return tty_set_pgrp(t, (pid_t)pgid);
}

/* --- credentials (Prompt #3, 3.6) ------------------------------------------- */

static int64_t sys_setresuid(struct syscall_args *a)
{
    return process_setresuid((int64_t)a->a[0], (int64_t)a->a[1], (int64_t)a->a[2]);
}

static int64_t sys_setresgid(struct syscall_args *a)
{
    return process_setresgid((int64_t)a->a[0], (int64_t)a->a[1], (int64_t)a->a[2]);
}

static int64_t put_three(struct syscall_args *a, uint32_t r, uint32_t e, uint32_t s)
{
    if (copy_to_user(a->a[0], &r, sizeof(r)) || copy_to_user(a->a[1], &e, sizeof(e)) ||
        copy_to_user(a->a[2], &s, sizeof(s)))
        return -EFAULT;
    return 0;
}

static int64_t sys_getresuid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    return put_three(a, c->ruid, c->euid, c->suid);
}

static int64_t sys_getresgid(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    return put_three(a, c->rgid, c->egid, c->sgid);
}

static int64_t sys_setgroups(struct syscall_args *a)
{
    size_t n = (size_t)a->a[1];
    if (n > CRED_NGROUPS_MAX)
        return -EINVAL;
    uint32_t groups[CRED_NGROUPS_MAX];
    if (n && copy_from_user(groups, a->a[0], n * sizeof(groups[0])))
        return -EFAULT;
    return process_setgroups(groups, (unsigned)n);
}

static int64_t sys_getgroups(struct syscall_args *a)
{
    const struct credentials *c = cred_current();
    size_t n = (size_t)a->a[1];
    if (n == 0)
        return c->ngroups;
    if (n < c->ngroups)
        return -EINVAL;
    if (c->ngroups && copy_to_user(a->a[0], c->groups, c->ngroups * sizeof(c->groups[0])))
        return -EFAULT;
    return c->ngroups;
}

/* --- resource limits (audit milestone 6) --------------------------------------- */

static int64_t sys_getrlimit(struct syscall_args *a)
{
    uint64_t v;
    int rc = process_getrlimit((unsigned)a->a[0], &v);
    if (rc)
        return rc;
    return copy_to_user(a->a[1], &v, sizeof(v));
}

static int64_t sys_setrlimit(struct syscall_args *a)
{
    return process_setrlimit((unsigned)a->a[0], a->a[1]);
}

static int64_t sys_ioready(struct syscall_args *a)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], 0);
    if (obj == NULL)
        return -EBADF;
    unsigned r = kobject_ready(obj);
    kobject_put(obj);
    return (int64_t)r;
}

/* Making an object non-blocking changes how it behaves rather than what
 * it holds, which is what MANAGE is for. */
static int64_t sys_setnonblock(struct syscall_args *a)
{
    bool no_rights;
    struct kobject *obj =
        handle_lookup_rights(&process_current()->handles, (int)a->a[0], HANDLE_RIGHT_MANAGE, &no_rights);
    if (obj == NULL)
        return no_rights ? -EPERM : -EBADF;
    int rc = kobject_set_nonblock(obj, a->a[1] ? 1 : 0);
    kobject_put(obj);
    return rc < 0 ? rc : 0;
}

/* --- milestone 9: the asynchronous I/O ring (kernel/io/aio.c) --------------- */

static int64_t sys_aio_create(struct syscall_args *a)
{
    struct aio_ring *r;
    int rc = aio_ring_create((unsigned)a->a[0], (unsigned)a->a[1], &r);
    if (rc)
        return rc;
    int h = handle_install(&process_current()->handles, &r->obj, HANDLE_RIGHT_ALL);
    kobject_put(&r->obj);
    return h;
}

static struct aio_ring *ring_of(int h)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    if (obj == NULL)
        return NULL;
    struct aio_ring *r = aio_ring_from_kobject(obj);
    if (r == NULL)
        kobject_put(obj);
    return r;
}

static int64_t sys_aio_submit(struct syscall_args *a)
{
    struct aio_ring *r = ring_of((int)a->a[0]);
    if (r == NULL)
        return -EBADF;
    int64_t rc = aio_submit(r, a->a[1], (unsigned)a->a[2]);
    kobject_put(&r->obj);
    return rc;
}

static int64_t sys_aio_wait(struct syscall_args *a)
{
    struct aio_ring *r = ring_of((int)a->a[0]);
    if (r == NULL)
        return -EBADF;
    int64_t rc = aio_wait(r, a->a[1], (unsigned)a->a[2], (unsigned)a->a[3], a->a[4]);
    kobject_put(&r->obj);
    return rc;
}

static int64_t sys_pipe(struct syscall_args *a)
{
    if (!user_range_ok(a->a[0], 2 * sizeof(int)))
        return -EFAULT;
    struct kobject *rd, *wr;
    int rc = pipe_create(&rd, &wr);
    if (rc)
        return rc;
    struct handle_table *t = &process_current()->handles;
    int h[2];
    h[0] = handle_install(t, rd, HANDLE_RIGHT_READ | HANDLE_RIGHT_OWNER);
    h[1] = h[0] < 0 ? -EMFILE : handle_install(t, wr, HANDLE_RIGHT_WRITE | HANDLE_RIGHT_OWNER);
    kobject_put(rd);
    kobject_put(wr);
    if (h[0] < 0 || h[1] < 0) {
        if (h[0] >= 0)
            handle_close(t, h[0]);
        return -EMFILE;
    }
    if (copy_to_user(a->a[0], h, sizeof(h))) {
        handle_close(t, h[0]);
        handle_close(t, h[1]);
        return -EFAULT;
    }
    return 0;
}

/*
 * Duplicating a handle needs the right to duplicate it, and may hand the
 * copy *less* than the original holds -- never more. That is what makes
 * a handle a capability rather than a name: a process can pass a
 * read-only view of something it can write, and cannot get back what it
 * gave away (docs/kernel/object/architecture.md, "Rights").
 */
static int64_t sys_dup(struct syscall_args *a)
{
    int h = (int)a->a[0];
    int target = (int)a->a[1];
    unsigned want = (unsigned)a->a[2];
    if (target < -1 || target >= HANDLE_TABLE_SIZE)
        return -EINVAL;
    struct handle_table *t = &process_current()->handles;
    unsigned rights;
    struct kobject *obj = handle_get(t, h, &rights);
    if (obj == NULL)
        return -EBADF;
    if (!(rights & HANDLE_RIGHT_DUP)) {
        kobject_put(obj);
        return -EPERM;
    }
    if (want != COSMO_RIGHTS_SAME) {
        if ((want & ~rights) != 0) {
            kobject_put(obj);
            return -EPERM;   /* rights only ever shrink */
        }
        rights = want;
    }
    int rc;
    if (target == -1) {
        rc = handle_install(t, obj, rights);
    } else if (target == h) {
        rc = h;
    } else {
        handle_close(t, target);   /* -EBADF when free: fine */
        rc = handle_install_at(t, target, obj, rights);
    }
    kobject_put(obj);
    return rc;
}

static int64_t sys_getppid(struct syscall_args *a)
{
    (void)a;
    struct process *cur = process_current();
    /* The process that started a domain has a parent outside it. Naming
     * that pid would leak one number out of the very thing the domain
     * hides, so it reports no parent -- which is also what a process at
     * the top of a tree conventionally reports. */
    struct process *parent = cur->parent_pid ? process_lookup(cur->parent_pid) : NULL;
    if (parent) {
        bool outside = cur->domain != 0 && parent->domain != cur->domain;
        process_put(parent);
        if (outside)
            return 0;
    }
    return (int64_t)cur->parent_pid;
}

static int64_t sys_chdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : process_chdir(path);
}

static int64_t sys_getcwd(struct syscall_args *a)
{
    struct process *p = process_current();
    size_t len = (size_t)a->a[1];
    char buf[VFS_PATH_MAX];
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    size_t n = strlcpy(buf, p->cwd_path, sizeof(buf));
    spin_unlock_irqrestore(&p->lock, s);
    if (len < n + 1)
        return -ERANGE;
    return copy_to_user(a->a[0], buf, n + 1) ? -EFAULT : (int64_t)n;
}

static int64_t sys_procinfo(struct syscall_args *a)
{
    size_t count = (size_t)a->a[1];
    if (count > 4096)
        count = 4096;
    if (!user_range_ok(a->a[0], count * sizeof(struct cosmo_procinfo)))
        return -EFAULT;
    struct cosmo_procinfo *tmp = NULL;
    if (count) {
        tmp = kmalloc(count * sizeof(*tmp), 0);
        if (tmp == NULL)
            return -ENOMEM;
    }
    unsigned total = process_info(tmp, (unsigned)count, cred_current());
    unsigned filled = total < count ? total : (unsigned)count;
    int rc = 0;
    if (filled && copy_to_user(a->a[0], tmp, filled * sizeof(*tmp)))
        rc = -EFAULT;
    kfree(tmp);
    return rc ? rc : (int64_t)total;
}

static int64_t sys_klog(struct syscall_args *a)
{
    /* The kernel log carries kernel addresses and every process's
     * activity: privileged, like dmesg on a hardened system. */
    if (!cred_privileged(cred_current()))
        return -EPERM;
    size_t len = (size_t)a->a[1];
    if (len > KLOG_RING_SIZE)
        len = KLOG_RING_SIZE;
    if (!user_range_ok(a->a[0], len))
        return -EFAULT;
    if (len == 0)
        return 0;
    char *tmp = kmalloc(len, 0);
    if (tmp == NULL)
        return -ENOMEM;
    size_t n = klog_copy(tmp, len);
    int rc = n && copy_to_user(a->a[0], tmp, n) ? -EFAULT : 0;
    kfree(tmp);
    return rc ? rc : (int64_t)n;
}

/*
 * Unprivileged on purpose: it only ever takes authority from the caller
 * and its children (docs/kernel/security/design.md §1f). Copied in
 * fully before anything is applied, so a faulting mask leaves the
 * process with the filter it had rather than half of a new one.
 */
static int64_t sys_syscall_filter(struct syscall_args *a)
{
    size_t words = (size_t)a->a[1];
    if (words == 0 || words > COSMO_SYSCALL_MASK_WORDS)
        return -EINVAL;
    uint64_t mask[COSMO_SYSCALL_MASK_WORDS];
    if (copy_from_user(mask, a->a[0], words * sizeof(mask[0])))
        return -EFAULT;
    syscall_filter_install(process_current(), mask, (unsigned)words);
    return 0;
}

static int64_t sys_gethostname(struct syscall_args *a)
{
    char buf[COSMO_HOST_NAME_MAX];
    size_t n = utsns_gethostname(utsns_current(), buf, sizeof(buf));
    if ((size_t)a->a[1] < n + 1)
        return -ERANGE;
    return copy_to_user(a->a[0], buf, n + 1) ? -EFAULT : (int64_t)n;
}

/* Privileged: a name is not a secret, but a process that could rename
 * the machine could make another one's logs say whatever it liked. */
static int64_t sys_sethostname(struct syscall_args *a)
{
    if (!cred_privileged(cred_current()))
        return -EPERM;
    size_t len = (size_t)a->a[1];
    if (len >= COSMO_HOST_NAME_MAX)
        return -EINVAL;
    char buf[COSMO_HOST_NAME_MAX];
    if (copy_from_user(buf, a->a[0], len))
        return -EFAULT;
    return utsns_sethostname(utsns_current(), buf, len);
}

static const char *const sysctl_names[] = {
    "kernel.name", "kernel.version", "kernel.build", "kernel.arch", "kernel.uptime_ns", "kernel.nprocs",
    "kernel.hostname",
    "hw.ncpu", "vm.page_size", "vm.pages_total", "vm.pages_free", "vm.cache_pages", "vm.cache_limit",
    "hv.backend", "hv.vms", "hv.vcpus", "hv.exits",
    "net.steer",
    "sysctl.names",
    "debug.faultinject",
};

static int sysctl_value(const char *name, char *out, size_t n)
{
    if (strcmp(name, "kernel.name") == 0)
        return ksnprintf(out, n, "%s", KERNEL_NAME);
    if (strcmp(name, "kernel.version") == 0)
        return ksnprintf(out, n, "%s", KERNEL_VERSION);
    if (strcmp(name, "kernel.build") == 0)
        return ksnprintf(out, n, "%s %s", COSMO_BUILD_ID, COSMO_BUILD_TYPE);
    if (strcmp(name, "kernel.arch") == 0)
        return ksnprintf(out, n, "%s", arch_name());
    if (strcmp(name, "kernel.uptime_ns") == 0)
        return ksnprintf(out, n, "%llu", (unsigned long long)clock_now_ns());
    /* From the caller's namespace, like gethostname and uname: three
     * ways to ask must not give a contained process three answers. */
    if (strcmp(name, "kernel.hostname") == 0) {
        char host[COSMO_HOST_NAME_MAX];
        utsns_gethostname(utsns_current(), host, sizeof(host));
        return ksnprintf(out, n, "%s", host);
    }
    if (strcmp(name, "kernel.nprocs") == 0)
        return ksnprintf(out, n, "%u", process_count());
    if (strcmp(name, "hw.ncpu") == 0)
        return ksnprintf(out, n, "%u", cpu_count());
    if (strcmp(name, "vm.page_size") == 0)
        return ksnprintf(out, n, "%u", (unsigned)PAGE_SIZE);
    if (strcmp(name, "vm.cache_pages") == 0) {
        struct pagecache_stats st;
        pagecache_get_stats(&st);
        return ksnprintf(out, n, "%llu", (unsigned long long)st.pages);
    }
    if (strcmp(name, "vm.cache_limit") == 0)
        return ksnprintf(out, n, "%llu", (unsigned long long)pagecache_limit());
    if (strcmp(name, "vm.pages_total") == 0 || strcmp(name, "vm.pages_free") == 0) {
        struct pmm_stats st;
        pmm_get_stats(&st);
        return ksnprintf(out, n, "%llu",
                         (unsigned long long)(name[9] == 't' ? st.total_pages : st.free_pages));
    }
    if (strncmp(name, "hv.", 3) == 0)
        return hv_sysctl(name + 3, out, n);
    if (strcmp(name, "net.steer") == 0)
        return ksnprintf(out, n, "%u", netif_steering() ? 1u : 0u);
    if (strcmp(name, "debug.faultinject") == 0) {
        int len = faultinject_sysctl(out, n);
        return len < 0 ? -ENOENT : len;   /* -ENOENT in release builds: the knob does not exist */
    }
    if (strcmp(name, "sysctl.names") == 0) {
        int len = 0;
        for (size_t i = 0; i < ARRAY_SIZE(sysctl_names); i++) {
            int m = ksnprintf(out + (len < (int)n ? len : (int)n), len < (int)n ? n - (size_t)len : 0, "%s\n",
                              sysctl_names[i]);
            len += m;
        }
        return len;
    }
    return -ENOENT;
}

static int64_t sys_sysctl(struct syscall_args *a)
{
    char name[64];
    int rc = strncpy_from_user(name, a->a[0], sizeof(name));
    if (rc < 0)
        return rc == -ENAMETOOLONG ? -ENOENT : rc;
    size_t len = (size_t)a->a[2];
    if (len > 4096)
        len = 4096;
    if (!user_range_ok(a->a[1], len))
        return -EFAULT;
    char value[512];
    int vlen = sysctl_value(name, value, sizeof(value));
    if (vlen < 0)
        return vlen;
    if (vlen >= (int)sizeof(value))
        vlen = (int)sizeof(value) - 1;
    size_t copy = (size_t)vlen + 1 <= len ? (size_t)vlen + 1 : len;   /* NUL when it fits */
    if (copy && copy_to_user(a->a[1], value, copy))
        return -EFAULT;
    return vlen;
}

static const syscall_fn native_table[SYS_COUNT] = {
    [SYS_exit] = sys_exit,
    [SYS_write] = sys_write,
    [SYS_read] = sys_read,
    [SYS_getpid] = sys_getpid,
    [SYS_yield] = sys_yield,
    [SYS_sleep_ns] = sys_sleep_ns,
    [SYS_clock_ns] = sys_clock_ns,
    [SYS_mmap] = sys_mmap,
    [SYS_munmap] = sys_munmap,
    [SYS_log] = sys_log,
    [SYS_close] = sys_close,
    [SYS_open] = sys_open,
    [SYS_stat] = sys_stat,
    [SYS_fstat] = sys_fstat,
    [SYS_lseek] = sys_lseek,
    [SYS_mkdir] = sys_mkdir,
    [SYS_unlink] = sys_unlink,
    [SYS_rmdir] = sys_rmdir,
    [SYS_rename] = sys_rename,
    [SYS_getdents] = sys_getdents,
    [SYS_sync] = sys_sync,
    [SYS_fsync] = sys_fsync,
    [SYS_mount] = sys_mount,
    [SYS_umount] = sys_umount,
    [SYS_socket] = sys_socket,
    [SYS_bind] = sys_bind,
    [SYS_listen] = sys_listen,
    [SYS_accept] = sys_accept,
    [SYS_connect] = sys_connect,
    [SYS_sendto] = sys_sendto,
    [SYS_recvfrom] = sys_recvfrom,
    [SYS_shutdown] = sys_shutdown,
    [SYS_getsockname] = sys_getsockname,
    [SYS_spawn] = sys_spawn,
    [SYS_wait] = sys_wait,
    [SYS_kill] = sys_kill,
    [SYS_pipe] = sys_pipe,
    [SYS_dup] = sys_dup,
    [SYS_getppid] = sys_getppid,
    [SYS_chdir] = sys_chdir,
    [SYS_getcwd] = sys_getcwd,
    [SYS_procinfo] = sys_procinfo,
    [SYS_syscall_filter] = sys_syscall_filter,
    [SYS_gethostname] = sys_gethostname,
    [SYS_sethostname] = sys_sethostname,
    [SYS_klog] = sys_klog,
    [SYS_sysctl] = sys_sysctl,
    [SYS_vm_create] = sys_vm_create,
    [SYS_vm_mem] = sys_vm_mem,
    [SYS_vm_mem_rw] = sys_vm_mem_rw,
    [SYS_vcpu_create] = sys_vcpu_create,
    [SYS_vcpu_regs] = sys_vcpu_regs,
    [SYS_vcpu_run] = sys_vcpu_run,
    [SYS_vcpu_irq] = sys_vcpu_irq,
    [SYS_vm_raise_spi] = sys_vm_raise_spi,
    [SYS_vm_lower_spi] = sys_vm_lower_spi,
    [SYS_setresuid] = sys_setresuid,
    [SYS_setresgid] = sys_setresgid,
    [SYS_getresuid] = sys_getresuid,
    [SYS_getresgid] = sys_getresgid,
    [SYS_getrlimit] = sys_getrlimit,
    [SYS_setrlimit] = sys_setrlimit,
    [SYS_ioready] = sys_ioready,
    [SYS_setnonblock] = sys_setnonblock,
    [SYS_aio_create] = sys_aio_create,
    [SYS_aio_submit] = sys_aio_submit,
    [SYS_aio_wait] = sys_aio_wait,
    [SYS_setgroups] = sys_setgroups,
    [SYS_getgroups] = sys_getgroups,
    [SYS_sigaction] = sys_sigaction,
    [SYS_sigprocmask] = sys_sigprocmask,
    [SYS_sigreturn] = sys_sigreturn,
    [SYS_sigpending] = sys_sigpending,
    [SYS_setpgid] = sys_setpgid,
    [SYS_getpgid] = sys_getpgid,
    [SYS_setsid] = sys_setsid,
    [SYS_getsid] = sys_getsid,
    [SYS_tcgetpgrp] = sys_tcgetpgrp,
    [SYS_tcsetpgrp] = sys_tcsetpgrp,
    [SYS_tcgetattr] = sys_tcgetattr,
    [SYS_tcsetattr] = sys_tcsetattr,
    [SYS_ttysize] = sys_ttysize,
};

/* A process must always be able to stop, whatever its filter says: a
 * filter that killed a process for exiting would turn every clean
 * shutdown into a signal death. */
/* sigreturn joins it: a filter that denied the return from a handler
 * would turn every caught signal into a kill. */
static const uint16_t native_always_allowed[] = { SYS_exit, SYS_sigreturn };

const struct personality personality_native = {
    .name = "native",
    .table = native_table,
    .count = SYS_COUNT,
    .always_allowed = native_always_allowed,
    .nr_always_allowed = sizeof(native_always_allowed) / sizeof(native_always_allowed[0]),
    .signal_frame = native_signal_frame,
};
