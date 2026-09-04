/*
 * native.c - The CosmoOS native personality: system-call table and
 * implementations. Numbers come from uapi/cosmo/syscall.h.
 *
 * Every function validates its arguments first and returns a negative
 * errno on failure. User memory is touched only through uaccess.h.
 */

#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define IO_CHUNK 512

static int64_t sys_exit(struct syscall_args *a)
{
    process_exit((int)a->a[0]);
}

static int64_t sys_write(struct syscall_args *a)
{
    int h = (int)a->a[0];
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];

    if (!user_range_ok(ubuf, len))
        return -EFAULT;

    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_WRITE);
    if (obj == NULL)
        return -EBADF;
    const struct kobject_io_type *io = (const struct kobject_io_type *)obj->type;
    if (io->write == NULL) {
        kobject_put(obj);
        return -EBADF;
    }

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
    kobject_put(obj);
    return done > 0 ? (int64_t)done : rc;
}

static int64_t sys_read(struct syscall_args *a)
{
    int h = (int)a->a[0];
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];

    if (!user_range_ok(ubuf, len))
        return -EFAULT;

    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_READ);
    if (obj == NULL)
        return -EBADF;
    const struct kobject_io_type *io = (const struct kobject_io_type *)obj->type;
    if (io->read == NULL) {
        kobject_put(obj);
        return -EBADF;
    }

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
    kobject_put(obj);
    return rc;
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
    thread_sleep_ns(ns);
    return 0;
}

static int64_t sys_clock_ns(struct syscall_args *a)
{
    (void)a;
    return (int64_t)clock_now_ns();
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
    if (vprot == 0)
        vprot = VM_PROT_READ; /* PROT_NONE reserves the range readable-only for now */

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
    return vm_user_unmap(process_current()->space, addr, len);
}

static int64_t sys_log(struct syscall_args *a)
{
    uint64_t ustr = a->a[0];
    size_t len = (size_t)a->a[1];
    char buf[200];

    if (len >= sizeof(buf))
        return -EINVAL;
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
};

const struct personality personality_native = {
    .name = "native",
    .table = native_table,
    .count = SYS_COUNT,
};
