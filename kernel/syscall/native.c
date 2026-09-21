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
#include <kernel/selftest.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/unix.h>
#include <arch/user.h>
#include <kernel/futex.h>
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

static uint64_t g_bounce_heap, g_bounce_fallback;

void syscall_bounce_get(struct io_bounce *b, char *stack, size_t len)
{
    b->buf = stack;
    b->cap = IO_CHUNK;
    b->heap = false;
    if (len > IO_CHUNK) {
        size_t want = len < IO_BOUNCE_MAX ? len : IO_BOUNCE_MAX;
        char *p = kmalloc(want, 0);   /* the page path above the largest slab class */
        if (p != NULL) {
            b->buf = p;
            b->cap = want;
            b->heap = true;
            __atomic_fetch_add(&g_bounce_heap, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&g_bounce_fallback, 1, __ATOMIC_RELAXED);
        }
    }
}

void syscall_bounce_put(struct io_bounce *b)
{
    if (b->heap)
        kfree(b->buf);
}

uint64_t syscall_bounce_heap_count(void) { return __atomic_load_n(&g_bounce_heap, __ATOMIC_RELAXED); }
uint64_t syscall_bounce_fallback_count(void) { return __atomic_load_n(&g_bounce_fallback, __ATOMIC_RELAXED); }

static int64_t sys_exit(struct syscall_args *a)
{
    process_exit((int)a->a[0]);
}

int64_t syscall_obj_write(struct kobject *obj, const uint64_t ubuf, size_t len)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->write == NULL)
        return -EBADF;
    char stack[IO_CHUNK];
    struct io_bounce b;
    syscall_bounce_get(&b, stack, len);
    size_t done = 0;
    int64_t rc = 0;
    while (done < len) {
        size_t n = len - done < b.cap ? len - done : b.cap;
        rc = copy_from_user(b.buf, ubuf + done, n);
        if (rc)
            break;
        rc = io->write(obj, b.buf, n);
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
    syscall_bounce_put(&b);
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
    /* One object call, as large as the request up to the bounce: what
     * one call returns is what the read returns, so a pipe or a tty that
     * has 300 bytes returns 300 and a file fills up to 64 KiB. */
    char stack[IO_CHUNK];
    struct io_bounce b;
    syscall_bounce_get(&b, stack, len);
    size_t n = len < b.cap ? len : b.cap;
    int64_t rc = io->read(obj, b.buf, n);
    /* An object may never report more than it was offered; the count
     * bounds the copy out of the kernel buffer. */
    KASSERT(rc <= (int64_t)n);
    if (rc > (int64_t)n)
        rc = -EIO;
    if (rc > 0 && copy_to_user(ubuf, b.buf, (size_t)rc))
        rc = -EFAULT;
    syscall_bounce_put(&b);
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

/* The caller's own thread id: the pid for a process's first thread, so a
 * single-threaded program's answer is its pid, as on Linux. */
static int64_t sys_thread_self(struct syscall_args *a)
{
    (void)a;
    return (int64_t)thread_current()->user_tid;
}

/* Wait while the word still holds `val`, and wake waiters on it. The futex
 * itself requires 4-byte alignment (-EINVAL) and does the compare and the
 * enqueue under one lock, so a wake between them cannot be lost; these add
 * only the range check. */
static int64_t sys_futex_wait(struct syscall_args *a)
{
    if (!user_range_ok(a->a[0], 4))
        return -EFAULT;
    /* The timer's deadline used to be a bare clock_now_ns() + this, so a
     * duration near the top of the range wrapped into the past and
     * expired at once -- the opposite of what the caller asked for. It
     * saturates now (clock_deadline_ns), and this check stays anyway: a
     * syscall should refuse an impossible duration rather than silently
     * turn it into "never". Anything past INT64_MAX (292 years) is
     * -EINVAL; 0 is still "no timeout". */
    if (a->a[2] > (uint64_t)INT64_MAX)
        return -EINVAL;
    return futex_wait(process_current()->space, a->a[0], (uint32_t)a->a[1], a->a[2], false);
}

static int64_t sys_futex_wake(struct syscall_args *a)
{
    if (!user_range_ok(a->a[0], 4))
        return -EFAULT;
    return futex_wake(process_current()->space, a->a[0], (unsigned)a->a[1], false);
}

/*
 * Wake up to `nr_wake` waiters on the first word and move up to
 * `nr_requeue` more onto the second without waking them -- if the first
 * word still holds `val`, else -EAGAIN and nobody moved. The compare form
 * only: the native ABI is new and need not carry the non-comparing
 * FUTEX_REQUEUE's lost-wakeup race for compatibility with nothing
 * (docs/audit/next-subsystem-native-thread-door.md). The kernel never
 * reads the second word -- it is a key, not a load -- which is what lets
 * libc's broadcast hand it a pointer it has not dereferenced itself.
 * Returns woken + requeued, as the Linux door reports.
 */
static int64_t sys_futex_requeue(struct syscall_args *a)
{
    if (!user_range_ok(a->a[0], 4) || !user_range_ok(a->a[1], 4))
        return -EFAULT;
    return futex_requeue(process_current()->space, a->a[0], a->a[1], (unsigned)a->a[2], (unsigned)a->a[3], true,
                         (uint32_t)a->a[4], false);   /* the native calls always classify: the kernel can see what the word maps */
}

/*
 * The calling thread's thread pointer, and only the calling thread's: a
 * thread pointer is the definition of per-thread state, so this is shaped
 * like sigprocmask and not like the syscall filter. SYS_thread_create's
 * `tls` remains how a *new* thread gets one before its first instruction;
 * this is how a thread that already exists gets one, which in practice
 * means a process's first thread, from libc's startup.
 *
 * Zero is legal, and is what every thread has until something sets it.
 *
 * The range check is eight bytes because eight is what the kernel can
 * honestly promise: libc's block is libc's business and its size is not
 * the kernel's to know, but on x86-64 the architecture itself dereferences
 * the first word through %fs:0, so a base whose first word is not in the
 * caller's space is wrong on its face. The alignment is 16, which every
 * layout a C ABI will put there wants anyway.
 */
static int64_t sys_set_tls(struct syscall_args *a)
{
    uint64_t base = a->a[0];
    if (base % 16u)
        return -EINVAL;
    if (base != 0 && !user_range_ok(base, 8))
        return -EFAULT;
    arch_set_tls_base((uintptr_t)base);
    return 0;
}

/* This thread ends; the process ends with the last of them, carrying that
 * thread's status. SYS_exit remains the process. */
static int64_t sys_thread_exit(struct syscall_args *a)
{
    process_thread_exit((int)a->a[0]);
}

/*
 * A thread of the calling process, at `entry` on a stack the caller owns.
 * The order here is the order the failure modes want: everything before
 * process_add_thread fails with nothing created, and everything after it
 * fails with process_thread_abandon.
 */
static int64_t sys_thread_create(struct syscall_args *a)
{
    struct cosmo_thread req;
    if (copy_from_user(&req, a->a[0], sizeof(req)))
        return -EFAULT;
    if (req.flags != 0 || req.reserved != 0 || req.entry == 0 || req.stack_top == 0)
        return -EINVAL;
    if (req.stack_top % 16u || req.stack_top <= ARCH_THREAD_TOP_BYTES)
        return -EINVAL;
    if (req.clear_tid % 4u)
        return -EINVAL;
    if (req.clear_tid && !user_range_ok(req.clear_tid, 4))
        return -EFAULT;

    /* The top of the stack: x86-64's return slot, and on every
     * architecture the write that proves the stack is really there. Done
     * before anything is linked, because an aligned stack_top can still be
     * unmapped or read-only and a failure here must leave nothing behind.
     * Uniform across architectures deliberately: a validation that one
     * architecture performed and the other did not would let a program
     * that only runs on one create a thread doomed to fault on its first
     * push. */
    {
        uint64_t top = 0;
        uint64_t at = req.stack_top - ARCH_THREAD_TOP_BYTES;
        if (!user_range_ok(at, ARCH_THREAD_TOP_BYTES) || copy_to_user(at, &top, ARCH_THREAD_TOP_BYTES))
            return -EFAULT;
    }

    struct arch_user_regs regs;
    arch_user_regs_init_thread(&regs, (uintptr_t)req.entry, (uintptr_t)req.arg, (uintptr_t)req.stack_top);

    struct thread *t;
    int rc = process_add_thread(process_current(), &regs, (uintptr_t)req.tls, &t);
    if (rc)
        return rc;
    uint32_t tid = t->user_tid;
    if (req.clear_tid) {
        t->clear_child_tid = req.clear_tid;
        /* Before the start, so the word's only two states are this id and
         * the zero the exit writes: a caller that wrote it afterwards could
         * lose the race to a child that had already finished. */
        if (copy_to_user(req.clear_tid, &tid, sizeof(tid))) {
            process_thread_abandon(t);
            return -EFAULT;
        }
    }
    process_thread_start(t);
    return (int64_t)tid;
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

static struct file *file_of(int h, unsigned rights);

/*
 * The file half of a mapping request, the same at both doors
 * (compat/linux/syscalls.c makes the same checks with Linux's names): a
 * regular file (else -ENODEV) reachable through a handle with the READ
 * right (else -EBADF); a SHARED mapping with PROT_WRITE needs the file
 * opened for writing and the handle's WRITE right (else -EACCES). On
 * success *fp holds a referenced file and *maxprot the ceiling a later
 * mprotect may reach: a shared mapping of a file opened read-only can
 * never be made writable, everything else is bounded by W^X alone.
 */
static int mmap_file_check(int fd, bool shared, bool write, struct file **fp, vm_prot_t *maxprot)
{
    /* ONE lookup, carrying the handle's rights: a second lookup of the
     * same number could resolve to a different file if another thread
     * closed and reopened it in between, and "writable" would then be
     * decided by a file other than the one mapped (review found the
     * first version doing exactly that). */
    unsigned rights = 0;
    struct kobject *obj = handle_get(&process_current()->handles, fd, &rights);
    if (obj == NULL)
        return -EBADF;
    struct file *f = file_from_kobject(obj);
    if (f == NULL || !(rights & HANDLE_RIGHT_READ)) {
        kobject_put(obj);
        return -EBADF;
    }
    if (f->vn->type != VNODE_REG) {
        file_put(f);
        return -ENODEV;
    }
    /* The handle's rights bound the file's mode: a handle duplicated
     * without WRITE cannot map for writing what the file allows. */
    bool writable = (f->flags & COSMO_O_ACCMODE) != COSMO_O_RDONLY && (rights & HANDLE_RIGHT_WRITE);
    if (shared && write && !writable) {
        file_put(f);
        return -EACCES;
    }
    *maxprot = VM_PROT_READ | VM_PROT_EXEC | ((!shared || writable) ? VM_PROT_WRITE : 0);
    *fp = f;
    return 0;
}

static int64_t sys_mmap(struct syscall_args *a)
{
    uint64_t hint = a->a[0];
    size_t len = (size_t)a->a[1];
    int prot = (int)a->a[2];
    int flags = (int)a->a[3];
    int fd = (int)a->a[4];
    uint64_t off = a->a[5];
    struct process *p = process_current();

    /* A flag bit this kernel does not define is refused, so a program can
     * learn what the kernel it runs on supports and a future flag is
     * never silently dropped (the rule for every native flags word). */
    if (flags & ~(COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED | COSMO_MAP_FIXED_NOREPLACE | COSMO_MAP_SHARED |
                  COSMO_MAP_PRIVATE))
        return -EINVAL;
    /* NOREPLACE qualifies FIXED; on its own it has no address to keep. */
    if ((flags & COSMO_MAP_FIXED_NOREPLACE) && !(flags & COSMO_MAP_FIXED))
        return -EINVAL;
    if (len == 0 || !is_page_aligned(len) || len > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    bool anon = (flags & COSMO_MAP_ANONYMOUS) != 0;
    bool shared = (flags & COSMO_MAP_SHARED) != 0;
    /*
     * A file mapping names exactly one of SHARED and PRIVATE. Anonymous
     * memory may say PRIVATE (it is) and may not say SHARED: without a
     * fork there is nobody to share it with, and a program that asked
     * for cross-process anonymous sharing must not be told yes
     * (docs/audit/next-subsystem-file-regions.md, "The two doors").
     */
    if (anon ? shared : (shared == ((flags & COSMO_MAP_PRIVATE) != 0)))
        return -EINVAL;
    if (prot & ~(COSMO_PROT_READ | COSMO_PROT_WRITE | COSMO_PROT_EXEC))
        return -EINVAL;
    if ((prot & COSMO_PROT_WRITE) && (prot & COSMO_PROT_EXEC))
        return -EINVAL; /* W^X */
    if (!anon && (!is_page_aligned(off) || off + len < off))
        return -EINVAL;

    vm_prot_t vprot = 0;
    if (prot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (prot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (prot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    /* PROT_NONE reserves the range: every access faults (design.md §6.2). */

    struct file *f = NULL;
    vm_prot_t maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC;
    if (!anon) {
        int frc = mmap_file_check(fd, shared, (vprot & VM_PROT_WRITE) != 0, &f, &maxprot);
        if (frc)
            return frc;
    }

    int rc;
    uint64_t base;
    if (flags & COSMO_MAP_FIXED) {
        if (!is_page_aligned(hint) || !user_range_ok(hint, len)) {
            rc = -EINVAL;
            goto out;
        }
        base = hint;
        /*
         * POSIX: a fixed mapping takes the range whatever is there.
         * Replacement is one operation in the VM layer rather than an
         * unmap and a map here, because the gap between those two is a
         * window another thread's mmap(NULL, ...) can be handed -- which
         * is exactly what cosmo_thread_start used to lose
         * (docs/audit/next-subsystem-map-fixed.md). NOREPLACE keeps the
         * old refusal. A file mapping goes through the same replacement,
         * so M40 holds for it too.
         */
        if (!(flags & COSMO_MAP_FIXED_NOREPLACE)) {
            rc = f ? vm_user_map_file(p->space, base, len, vprot, maxprot,
                                      VM_MAP_REPLACE | (shared ? VM_MAP_SHARED : 0), f->vn, off, "mmap-file")
                   : vm_user_map_anon_replace(p->space, base, len, vprot, 0, "mmap");
            goto out;
        }
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

    rc = f ? vm_user_map_file(p->space, base, len, vprot, maxprot, shared ? VM_MAP_SHARED : 0, f->vn, off,
                              "mmap-file")
           : vm_user_map_anon(p->space, base, len, vprot, 0, "mmap");
out:
    if (f)
        file_put(f);
    return rc ? rc : (int64_t)base;
}

static int64_t sys_msync(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    int flags = (int)a->a[2];
    if (flags & ~(COSMO_MS_ASYNC | COSMO_MS_INVALIDATE | COSMO_MS_SYNC))
        return -EINVAL;   /* an undefined bit: the native rule */
    if ((flags & COSMO_MS_ASYNC) && (flags & COSMO_MS_SYNC))
        return -EINVAL;   /* POSIX: one or the other */
    if (!is_page_aligned(addr) || len == 0 || !is_page_aligned(len) || !user_range_ok(addr, len))
        return -EINVAL;
    if (!(flags & COSMO_MS_SYNC)) {
        /* ASYNC: the dirty pages are the cache's already and reach the
         * filesystem by vfs_sync, the write-back thread or the last
         * close, which is what "scheduled" means here. INVALIDATE:
         * nothing is stale, the mapping IS the cache. Both still owe
         * the range check. */
        return vm_user_range_mapped(process_current()->space, addr, len, 0) ? 0 : -ENOMEM;
    }
    return vm_user_msync(process_current()->space, addr, len);
}

static int64_t sys_munmap(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    if (!is_page_aligned(addr) || len == 0 || !is_page_aligned(len) || !user_range_ok(addr, len))
        return -EINVAL;
    return vm_user_unmap(process_current()->space, addr, len, VM_UNMAP_STRICT);
}

static int64_t sys_mprotect(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    int prot = (int)a->a[2];

    /* The native rules, the same ones mmap and munmap keep: an
     * undefined prot bit is an error rather than ignored, and len is a
     * page multiple rather than rounded up. */
    if (prot & ~(COSMO_PROT_READ | COSMO_PROT_WRITE | COSMO_PROT_EXEC))
        return -EINVAL;
    if (!is_page_aligned(addr) || len == 0 || !is_page_aligned(len) || !user_range_ok(addr, len))
        return -EINVAL;
    vm_prot_t vprot = 0;
    if (prot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (prot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (prot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;

    /* W|X, a hole in the range and a range a replacement has claimed are
     * all decided in the VM layer, not here; this door only translates. */
    int rc = vm_user_protect(process_current()->space, addr, len, vprot);
    if (rc)
        return rc;
    /* Bytes written to this range as data may still be in the data
     * cache and stale in the instruction cache. User code cannot fix
     * that itself (SCTLR_EL1.UCI is clear), so the kernel does when the
     * range becomes executable -- for every caller, not just a test. */
    if (vprot & VM_PROT_EXEC)
        vm_user_sync_icache(process_current()->space, addr, len);
    return 0;
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
    unsigned flags = (unsigned)a->a[1];
    if (flags & ~(COSMO_O_ACCMODE | COSMO_O_CREAT | COSMO_O_EXCL | COSMO_O_TRUNC | COSMO_O_APPEND |
                  COSMO_O_DIRECTORY | COSMO_O_NOFOLLOW))
        return -EINVAL;   /* an unknown flag bit: see sys_mmap */
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    uint32_t mode = (uint32_t)a->a[2];
    struct file *f;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_open(cwd, path, flags, mode, &f);
    vnode_put(cwd);
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

/* The three symbolic-link calls. readlink copies without a terminator,
 * as POSIX says and as the walk wants (docs/kernel-services/vfs/api.md). */
static int64_t sys_symlink(struct syscall_args *a)
{
    char target[VFS_PATH_MAX], path[VFS_PATH_MAX];
    int rc = strncpy_from_user(target, a->a[0], sizeof(target));
    if (rc < 0)
        return rc;
    rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_symlink(cwd, path, target);
    vnode_put(cwd);
    return rc;
}

static int64_t sys_readlink(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    size_t len = (size_t)a->a[2];
    if (len == 0)
        return -EINVAL;
    if (len > VFS_PATH_MAX)
        len = VFS_PATH_MAX;
    char *buf = kmalloc(len, 0);
    if (buf == NULL)
        return -ENOMEM;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_readlink(cwd, path, buf, len);
    vnode_put(cwd);
    if (rc > 0 && copy_to_user(a->a[1], buf, (size_t)rc) != 0)
        rc = -EFAULT;
    kfree(buf);
    return rc;
}

static int64_t sys_lstat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_lstat(cwd, path, &st);
    vnode_put(cwd);
    if (rc)
        return rc;
    return copy_to_user(a->a[1], &st, sizeof(st)) ? -EFAULT : 0;
}

static int64_t sys_stat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_stat(cwd, path, &st);
    vnode_put(cwd);
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
    if (rc)
        return rc;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_mkdir(cwd, path, (uint32_t)a->a[1]);
    vnode_put(cwd);
    return rc;
}

static int64_t sys_unlink(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_unlink(cwd, path);
    vnode_put(cwd);
    return rc;
}

static int64_t sys_rmdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_rmdir(cwd, path);
    vnode_put(cwd);
    return rc;
}

static int64_t sys_rename(struct syscall_args *a)
{
    char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
    int rc = get_path(a->a[0], oldp);
    if (rc)
        return rc;
    rc = get_path(a->a[1], newp);
    if (rc)
        return rc;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_rename(cwd, oldp, newp);
    vnode_put(cwd);
    return rc;
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
    unsigned flags = (unsigned)a->a[3];
    if (flags & ~COSMO_MOUNT_RDONLY)
        return -EINVAL;   /* an unknown flag bit: see sys_mmap */
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
    unsigned flags = (unsigned)a->a[1];
    if (flags & ~COSMO_UMOUNT_FORCE)
        return -EINVAL;   /* an unknown flag bit: see sys_mmap */
    char target[VFS_PATH_MAX];
    int rc = get_path(a->a[0], target);
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

/*
 * --- the unix address at the door ----------------------------------------
 * `struct cosmo_sockaddr_un`: the family, then a NUL-terminated path (a
 * node in the filesystem) or a leading NUL and the bytes of an abstract
 * name, delimited by the length the caller passed. `struct any_addr` is
 * whichever shape the family said; every socket call parses one.
 */
struct any_addr {
    uint16_t family;
    struct netaddr na;
    struct unix_addr ua;
};

static int any_addr_from_user(uint64_t uptr, size_t len, struct any_addr *out)
{
    uint16_t family;
    if (len < sizeof(family))
        return -EINVAL;
    if (copy_from_user(&family, uptr, sizeof(family)))
        return -EFAULT;
    out->family = family;
    if (family == COSMO_AF_UNIX) {
        struct cosmo_sockaddr_un un;
        if (len > sizeof(un))
            return -EINVAL;
        if (copy_from_user(&un, uptr, len))
            return -EFAULT;
        return unix_addr_parse(un.path, len - sizeof(family), &out->ua);
    }
    return addr_from_user(uptr, len, &out->na);
}

static int unix_addr_to_user(uint64_t uptr, uint64_t ulen, const struct unix_addr *a)
{
    if (uptr == 0)
        return 0;
    struct cosmo_sockaddr_un un;
    size_t full = unix_addr_pack(a, COSMO_AF_UNIX, &un);
    size_t room = full;
    if (ulen && copy_from_user(&room, ulen, sizeof(room)))
        return -EFAULT;
    if (room > full)
        room = full;
    if (room && copy_to_user(uptr, &un, room))
        return -EFAULT;
    if (ulen && copy_to_user(ulen, &full, sizeof(full)))
        return -EFAULT;
    return 0;
}

/* A socket's address, in whichever shape its family has. */
static int sock_addr_to_user(struct socket *s, uint64_t uptr, uint64_t ulen, const struct any_addr *a)
{
    if (s->family == COSMO_AF_UNIX)
        return unix_addr_to_user(uptr, ulen, &a->ua);
    return addr_to_user(uptr, ulen, &a->na);
}

/*
 * The bytes of a send, for sendto and sendmsg: copied in through a kernel
 * buffer (a datagram whole, a stream in chunks) and handed to the family's
 * transport. `h` is the handles riding along -- a unix socket only -- and
 * they ride with the first chunk that goes; whatever the transport did not
 * take is the caller's to drop.
 */
static int64_t sock_send_bytes(struct socket *s, uint64_t ubuf, size_t len, const struct any_addr *to,
                               struct unix_handles *h, bool dontwait)
{
    bool un = s->family == COSMO_AF_UNIX;
    if (to != NULL && (un ? to->family != COSMO_AF_UNIX : to->family == COSMO_AF_UNIX))
        return -EAFNOSUPPORT;
    if (!un && h != NULL && h->nr > 0)
        return -EINVAL;   /* only a unix socket carries handles */
    uint8_t *tmp = kmalloc(len < SOCK_IO_CHUNK ? (len ? len : 1) : SOCK_IO_CHUNK, 0);
    if (tmp == NULL)
        return -ENOMEM;
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
            else if (un)
                rc = unix_send(s, tmp, len, to ? &to->ua : NULL, h, dontwait);
            else
                rc = ksock_sendto(s, tmp, len, to ? &to->na : NULL);
            done = rc > 0 ? rc : 0;
        }
    } else {
        while ((size_t)done < len) {
            size_t n = len - (size_t)done < SOCK_IO_CHUNK ? len - (size_t)done : SOCK_IO_CHUNK;
            if (copy_from_user(tmp, ubuf + (uint64_t)done, n)) {
                rc = -EFAULT;
                break;
            }
            int64_t w = un ? unix_send(s, tmp, n, to ? &to->ua : NULL, h, dontwait)
                           : ksock_sendto(s, tmp, n, to ? &to->na : NULL);
            if (w <= 0) {
                rc = w;
                break;
            }
            done += w;
            if ((size_t)w < n)
                break;
        }
        if (len == 0 && un)
            rc = unix_send(s, tmp, 0, to ? &to->ua : NULL, h, dontwait);   /* handles and no bytes */
    }
    kfree(tmp);
    return done > 0 ? done : rc;
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
    struct any_addr addr;
    int rc = any_addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_BIND, &serr);
    if (s == NULL)
        return serr;
    if (addr.family == COSMO_AF_UNIX)
        rc = s->family == COSMO_AF_UNIX ? unix_bind(s, &addr.ua) : -EAFNOSUPPORT;
    else
        rc = ksock_bind(s, &addr.na);
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
    struct any_addr peer;
    int rc = ksock_accept(s, &c, &peer.na);
    ksock_put(s);
    if (rc)
        return rc;
    if (c->family == COSMO_AF_UNIX)
        unix_getpeername(c, &peer.ua);
    rc = sock_addr_to_user(c, a->a[1], a->a[2], &peer);
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
    struct any_addr addr;
    int rc = any_addr_from_user(a->a[1], (size_t)a->a[2], &addr);
    if (rc)
        return rc;
    int serr = 0;
    struct socket *s = sock_of_err((int)a->a[0], HANDLE_RIGHT_SOCK_CONNECT, &serr);
    if (s == NULL)
        return serr;
    if (addr.family == COSMO_AF_UNIX)
        rc = s->family == COSMO_AF_UNIX ? unix_connect(s, &addr.ua) : -EAFNOSUPPORT;
    else
        rc = ksock_connect(s, &addr.na);
    ksock_put(s);
    return rc;
}

static int64_t sys_sendto(struct syscall_args *a)
{
    uint64_t ubuf = a->a[1];
    size_t len = (size_t)a->a[2];
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct any_addr to;
    bool have_to = a->a[3] != 0;
    if (have_to) {
        int rc = any_addr_from_user(a->a[3], (size_t)a->a[4], &to);
        if (rc)
            return rc;
    }
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (s == NULL)
        return -EBADF;
    int64_t rc = sock_send_bytes(s, ubuf, len, have_to ? &to : NULL, NULL, false);
    ksock_put(s);
    return rc;
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
    struct any_addr from;
    int64_t n;
    if (s->family == COSMO_AF_UNIX)
        n = unix_recv(s, tmp, chunk, &from.ua, NULL, NULL, false);
    else
        n = ksock_recvfrom(s, tmp, chunk, &from.na);
    if (n > 0 && copy_to_user(ubuf, tmp, (size_t)n))
        n = -EFAULT;
    kfree(tmp);
    if (n >= 0 && a->a[3]) {
        int rc = sock_addr_to_user(s, a->a[3], a->a[4], &from);
        if (rc)
            n = rc;
    }
    ksock_put(s);
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
    struct any_addr addr;
    int rc = s->family == COSMO_AF_UNIX ? unix_getsockname(s, &addr.ua) : ksock_getsockname(s, &addr.na);
    if (rc == 0)
        rc = sock_addr_to_user(s, a->a[1], a->a[2], &addr);
    ksock_put(s);
    return rc;
}

/* A socket's own answer about itself. One option: the pending error, which
 * SYS_ioready can say exists and cannot name. Reading it clears it (invariant
 * N21), so it is asked for rather than stumbled on -- no right beyond holding
 * the handle, because this reads a verdict rather than changing anything. */
static int64_t sys_getsockopt(struct syscall_args *a)
{
    struct socket *s = sock_of((int)a->a[0], 0);
    if (s == NULL)
        return -EBADF;
    int64_t rc;
    if ((int)a->a[1] == COSMO_SOL_SOCKET && (int)a->a[2] == COSMO_SO_PEERCRED) {
        /* Who is on the other end of a connected unix stream socket,
         * recorded when the connection was made. */
        struct cosmo_ucred uc;
        rc = s->family == COSMO_AF_UNIX ? unix_peercred(s, &uc) : -ENOPROTOOPT;
        ksock_put(s);
        if (rc)
            return rc;
        size_t room = sizeof(uc);
        if (a->a[4] && copy_from_user(&room, a->a[4], sizeof(room)))
            return -EFAULT;
        if (room < sizeof(uc))
            return -EINVAL;
        size_t len = sizeof(uc);
        if (copy_to_user(a->a[3], &uc, sizeof(uc)) || (a->a[4] && copy_to_user(a->a[4], &len, sizeof(len))))
            return -EFAULT;
        return 0;
    }
    if ((int)a->a[1] != COSMO_SOL_SOCKET || (int)a->a[2] != COSMO_SO_ERROR) {
        rc = -ENOPROTOOPT;   /* true of every other option this stack has */
        ksock_put(s);
        return rc;
    }
    /* Everything that can refuse this call is settled BEFORE the error is
     * read, because reading clears it: a call that fails must not be the
     * one delivery the verdict gets. */
    int val = 0;
    size_t room = sizeof(val);
    if (a->a[4] && copy_from_user(&room, a->a[4], sizeof(room))) {
        rc = -EFAULT;
    } else if (room < sizeof(val)) {
        rc = -EINVAL;   /* an int does not fit: say so rather than truncate a verdict */
    } else if (!user_range_ok(a->a[3], sizeof(val)) ||
               (a->a[4] && !user_range_ok(a->a[4], sizeof(room)))) {
        rc = -EFAULT;
    } else {
        /* The range checks above cannot promise the copy: the page may be
         * read-only, or unmapped by another thread between the check and
         * the write. So the verdict is read WITHOUT clearing and the clear
         * is committed only once it has reached the caller -- never taken
         * and put back, which would leave a window in which a concurrent
         * asker is told 0 while a verdict is pending and undelivered. */
        uint64_t token = 0;
        int e = ksock_error_peek(s, &token);
        val = e < 0 ? -e : e;     /* POSIX's sign: a positive errno, 0 for none */
        size_t len = sizeof(val);
        rc = copy_to_user(a->a[3], &val, sizeof(val)) ? -EFAULT
           : (a->a[4] && copy_to_user(a->a[4], &len, sizeof(len))) ? -EFAULT : 0;
        if (rc == 0)
            ksock_error_delivered(s, token);
    }
    ksock_put(s);
    return rc;
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

/*
 * tgkill with the process implied: the target is a thread of the calling
 * process, named by the id `SYS_thread_self` and `SYS_thread_create`
 * report (the pid for the first thread), and a tid of any other process
 * is -ESRCH by construction -- `kill` is the process-scoped door with its
 * own permission story, and this one cannot reach through it. No
 * credential check: a thread may signal its own process already. Signal
 * 0 probes, as at `kill`; the thread's own mask decides delivery, as it
 * does for a fault, and the handler runs on the targeted thread's frame.
 */
static int64_t sys_thread_kill(struct syscall_args *a)
{
    uint32_t tid = (uint32_t)a->a[0];
    int sig = (int)a->a[1];
    if (sig < 0 || sig >= COSMO_NSIG)
        return -EINVAL;
    struct process *cur = process_current();
    struct thread *t = process_find_thread(cur, tid);
    if (t == NULL)
        return -ESRCH;
    if (sig == 0)
        return 0;   /* it exists; nothing is sent */
    struct signal_info info = { .sig = sig, .source = SIGSRC_TKILL, .sender_pid = cur->pid,
                                .sender_uid = cur->cred.ruid };
    return signal_send_thread(t, sig, &info);
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

/*
 * --- messages: bytes, a name, handles ----------------------------------
 * A handle in a message is spawn's rule called again (handle_transfer_check):
 * TRANSFER held, the rights SAME or a subset. The message owns the
 * references while in flight; at recvmsg they are installed in order until
 * the first refusal, the rest released with COSMO_MSG_HTRUNC.
 */
static int64_t sys_sendmsg(struct syscall_args *a)
{
    struct cosmo_msg m;
    if (copy_from_user(&m, a->a[1], sizeof(m)))
        return -EFAULT;
    if (m.nr_handles > COSMO_UNIX_HANDLES_MAX || (m.flags & ~COSMO_MSG_DONTWAIT) != 0)
        return -EINVAL;
    if (!user_range_ok((uint64_t)(uintptr_t)m.buf, m.len))
        return -EFAULT;
    struct any_addr to;
    bool have_to = m.addr != NULL;
    if (have_to) {
        int rc = any_addr_from_user((uint64_t)(uintptr_t)m.addr, m.addrlen, &to);
        if (rc)
            return rc;
    }
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (s == NULL)
        return -EBADF;
    struct unix_handles hs = { .nr = 0 };
    struct handle_table *t = &process_current()->handles;
    int64_t rc = 0;
    for (unsigned i = 0; i < m.nr_handles; i++) {
        int hv;
        unsigned give = COSMO_RIGHTS_SAME;
        if (copy_from_user(&hv, (uint64_t)(uintptr_t)(m.handles + i), sizeof(hv)) ||
            (m.rights && copy_from_user(&give, (uint64_t)(uintptr_t)(m.rights + i), sizeof(give)))) {
            rc = -EFAULT;
            break;
        }
        rc = handle_transfer_check(t, hv, give, &hs.objs[i], &hs.rights[i]);
        if (rc)
            break;
        hs.nr++;
    }
    if (rc == 0)
        rc = sock_send_bytes(s, (uint64_t)(uintptr_t)m.buf, m.len, have_to ? &to : NULL, &hs,
                             (m.flags & COSMO_MSG_DONTWAIT) != 0);
    unix_handles_drop(&hs);   /* whatever no message took */
    ksock_put(s);
    return rc;
}

static int64_t sys_recvmsg(struct syscall_args *a)
{
    struct cosmo_msg m;
    if (copy_from_user(&m, a->a[1], sizeof(m)))
        return -EFAULT;
    if (m.nr_handles > COSMO_UNIX_HANDLES_MAX || (m.flags & ~COSMO_MSG_DONTWAIT) != 0)
        return -EINVAL;
    if (!user_range_ok((uint64_t)(uintptr_t)m.buf, m.len))
        return -EFAULT;
    struct socket *s = sock_of((int)a->a[0], HANDLE_RIGHT_READ);
    if (s == NULL)
        return -EBADF;
    size_t chunk = m.len < SOCK_IO_CHUNK * 16 ? m.len : SOCK_IO_CHUNK * 16;
    uint8_t *tmp = kmalloc(chunk ? chunk : 1, 0);
    if (tmp == NULL) {
        ksock_put(s);
        return -ENOMEM;
    }
    struct any_addr from;
    struct unix_handles hs = { .nr = m.handles ? m.nr_handles : 0 };
    unsigned flags = 0;
    int64_t n;
    if (s->family == COSMO_AF_UNIX) {
        n = unix_recv(s, tmp, chunk, &from.ua, &hs, &flags, (m.flags & COSMO_MSG_DONTWAIT) != 0);
    } else {
        hs.nr = 0;
        n = ksock_recvfrom(s, tmp, chunk, &from.na);
    }
    if (n > 0 && copy_to_user((uint64_t)(uintptr_t)m.buf, tmp, (size_t)n))
        n = -EFAULT;
    kfree(tmp);
    /* The handles, in message order, until the first refusal: those
     * installed are the receiver's now, the rest are released and the
     * flag says so -- Linux's MSG_CTRUNC, and no rollback. */
    unsigned installed = 0;
    struct handle_table *t = &process_current()->handles;
    for (unsigned i = 0; i < hs.nr; i++) {
        if (n < 0)
            break;
        int hv = handle_install(t, hs.objs[i], hs.rights[i]);
        if (hv < 0) {
            flags |= COSMO_MSG_HTRUNC;
            break;
        }
        if (copy_to_user((uint64_t)(uintptr_t)(m.handles + i), &hv, sizeof(hv))) {
            handle_close(t, hv);
            n = -EFAULT;
            break;
        }
        installed++;
    }
    unix_handles_drop(&hs);   /* the message's references; the table took its own */
    if (n >= 0 && m.addr) {
        /* The sender's name, bounded by the room the caller gave, the full
         * size reported back. */
        union {
            struct cosmo_sockaddr_un un;
            struct cosmo_sockaddr in;
        } out;
        size_t full;
        if (s->family == COSMO_AF_UNIX) {
            full = unix_addr_pack(&from.ua, COSMO_AF_UNIX, &out.un);
        } else {
            netaddr_to_user_shape(&out.in, &from.na);
            full = sizeof(out.in);
        }
        size_t room = m.addrlen < full ? m.addrlen : full;
        if (room && copy_to_user((uint64_t)(uintptr_t)m.addr, &out, room))
            n = -EFAULT;
        m.addrlen = full;
    }
    ksock_put(s);
    if (n < 0)
        return n;
    m.nr_handles = installed;
    m.flags = flags;
    /* The out fields, and only those. */
    if (copy_to_user(a->a[1] + offsetof(struct cosmo_msg, addrlen), &m.addrlen, sizeof(m.addrlen)) ||
        copy_to_user(a->a[1] + offsetof(struct cosmo_msg, nr_handles), &m.nr_handles, sizeof(m.nr_handles)) ||
        copy_to_user(a->a[1] + offsetof(struct cosmo_msg, flags), &m.flags, sizeof(m.flags)))
        return -EFAULT;
    return n;
}

static int64_t sys_socketpair(struct syscall_args *a)
{
    if ((int)a->a[0] != COSMO_AF_UNIX)
        return -EAFNOSUPPORT;
    int type = (int)a->a[1];
    bool nonblock = (type & COSMO_SOCK_NONBLOCK) != 0;
    if (!user_range_ok(a->a[2], 2 * sizeof(int)))
        return -EFAULT;
    struct socket *x, *y;
    int rc = unix_socketpair(type & ~COSMO_SOCK_NONBLOCK, &x, &y);
    if (rc)
        return rc;
    if (nonblock) {
        ksock_set_nonblock(x, true);
        ksock_set_nonblock(y, true);
    }
    struct handle_table *t = &process_current()->handles;
    int h[2];
    h[0] = handle_install(t, &x->obj, HANDLE_RIGHT_SOCK_CONNECTED);
    h[1] = h[0] < 0 ? -EMFILE : handle_install(t, &y->obj, HANDLE_RIGHT_SOCK_CONNECTED);
    ksock_put(x);
    ksock_put(y);
    if (h[0] < 0 || h[1] < 0) {
        if (h[0] >= 0)
            handle_close(t, h[0]);
        return -EMFILE;
    }
    if (copy_to_user(a->a[2], h, sizeof(h))) {
        handle_close(t, h[0]);
        handle_close(t, h[1]);
        return -EFAULT;
    }
    return 0;
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
    size_t n = strlcpy(buf, p->cwd_path_locked, sizeof(buf));
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
    "vm.cache_writebacks", "vm.cache_exec_syncs", "vm.file_faults", "vm.file_cow_faults", "vm.file_dirty_faults",
    "vm.file_fault_retries", "vm.file_sigbus", "vm.futex_shared_keys",
    "hv.backend", "hv.vms", "hv.vcpus", "hv.exits",
    "net.steer",
    "sysctl.names",
    "debug.faultinject",
    "debug.preempt_probe",
    "debug.file_fault_hold",
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
    if (strcmp(name, "vm.cache_writebacks") == 0 || strcmp(name, "vm.cache_exec_syncs") == 0) {
        struct pagecache_stats st;
        pagecache_get_stats(&st);
        return ksnprintf(out, n, "%llu", (unsigned long long)(name[9] == 'w' ? st.writebacks : st.exec_syncs));
    }
    /* The file-mapping counters (docs/audit/next-subsystem-file-regions.md):
     * what the mmap section of init --selftest reads to tell one
     * mutation of the fault from another. */
    if (strncmp(name, "vm.file_", 8) == 0) {
        struct vm_stats st;
        vm_get_stats(&st);
        const char *k = name + 8;
        uint64_t v;
        if (strcmp(k, "faults") == 0)
            v = st.file_faults;
        else if (strcmp(k, "cow_faults") == 0)
            v = st.file_cow_faults;
        else if (strcmp(k, "dirty_faults") == 0)
            v = st.file_dirty_faults;
        else if (strcmp(k, "fault_retries") == 0)
            v = st.file_fault_retries;
        else if (strcmp(k, "sigbus") == 0)
            v = st.file_sigbus;
        else
            return -ENOENT;
        return ksnprintf(out, n, "%llu", (unsigned long long)v);
    }
    if (strcmp(name, "vm.futex_shared_keys") == 0) {
        struct vm_stats st;
        vm_get_stats(&st);
        return ksnprintf(out, n, "%llu", (unsigned long long)st.futex_shared_keys);
    }
    if (strcmp(name, "debug.file_fault_hold") == 0) {
#if CONFIG_DEBUG
        return ksnprintf(out, n, "%u", vm_test_file_hold_state());
#else
        return -ENOENT;
#endif
    }
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
    if (strcmp(name, "debug.preempt_probe") == 0) {
#if CONFIG_SELFTEST
        /* The read is the system call under test
         * (docs/audit/next-subsystem-wake-preempt.md); it creates a
         * kernel thread, so only a privileged caller may ask. */
        if (!cred_privileged(cred_current()))
            return -EPERM;
        return sched_preempt_probe_sysctl(out, n);
#else
        return -ENOENT;
#endif
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
    [SYS_mprotect] = sys_mprotect,
    [SYS_log] = sys_log,
    [SYS_close] = sys_close,
    [SYS_open] = sys_open,
    [SYS_stat] = sys_stat,
    [SYS_symlink] = sys_symlink,
    [SYS_readlink] = sys_readlink,
    [SYS_lstat] = sys_lstat,
    [SYS_fstat] = sys_fstat,
    [SYS_lseek] = sys_lseek,
    [SYS_mkdir] = sys_mkdir,
    [SYS_unlink] = sys_unlink,
    [SYS_rmdir] = sys_rmdir,
    [SYS_rename] = sys_rename,
    [SYS_getdents] = sys_getdents,
    [SYS_sync] = sys_sync,
    [SYS_fsync] = sys_fsync,
    [SYS_thread_self] = sys_thread_self,
    [SYS_futex_wait] = sys_futex_wait,
    [SYS_futex_wake] = sys_futex_wake,
    [SYS_futex_requeue] = sys_futex_requeue,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_exit] = sys_thread_exit,
    [SYS_set_tls] = sys_set_tls,
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
    [SYS_getsockopt] = sys_getsockopt,
    [SYS_spawn] = sys_spawn,
    [SYS_wait] = sys_wait,
    [SYS_kill] = sys_kill,
    [SYS_thread_kill] = sys_thread_kill,
    [SYS_msync] = sys_msync,
    [SYS_sendmsg] = sys_sendmsg,
    [SYS_recvmsg] = sys_recvmsg,
    [SYS_socketpair] = sys_socketpair,
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
    [SYS_vcpu_stop] = sys_vcpu_stop,
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
static const uint16_t native_always_allowed[] = { SYS_exit, SYS_sigreturn, SYS_thread_exit, SYS_set_tls };
/* SYS_thread_exit is always allowed for the reason SYS_exit is: a thread
 * that cannot exit cannot be stopped, and a filter that traps one in the
 * kernel is a denial of service the filter unit did not intend.
 *
 * SYS_set_tls joins them for the same reason one step earlier: every native
 * program installs its thread block in `__libc_start`, before `main` and
 * before anything a filter could be about, so a filter that omitted it
 * would kill every child of a filtered process during startup -- a program
 * that cannot reach its own `main` cannot be confined, only destroyed. This
 * was not theoretical: the inherited-filter test's child died on number 87
 * in startup instead of on the call the test was about, and the status was
 * the same either way, so nothing failed. */

const struct personality personality_native = {
    .name = "native",
    .table = native_table,
    .count = SYS_COUNT,
    .always_allowed = native_always_allowed,
    .nr_always_allowed = sizeof(native_always_allowed) / sizeof(native_always_allowed[0]),
    .signal_frame = native_signal_frame,
};
