/*
 * syscalls.c - The Linux personality: the x86-64 Linux system-call table
 * translating onto the native kernel services (docs/compat/linux/).
 *
 * Every entry validates its arguments, converts Linux structures and
 * flags (lxconv.cee), and calls the same VFS, socket, process and memory
 * services the native personality uses. Nothing here is reachable by a
 * native process, and nothing native depends on this file.
 */

#include <arch/cpu.h>
#include <kernel/blk.h>
#include <kernel/compiler.h>
#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/epoll.h>
#include <kernel/eventfd.h>
#include <kernel/timerobj.h>
#include <kernel/futex.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/object.h>
#include <kernel/percpu.h>
#include <kernel/pipe.h>
#include <kernel/pmm.h>
#include <kernel/poll.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/shm.h>
#include <kernel/signal.h>
#include <kernel/signalfd.h>
#include <kernel/socket.h>
#include <kernel/unix.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/pty.h>
#include <kernel/tty.h>
#include <kernel/thread.h>
#include <kernel/shutdown.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/version.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <arch/mmu.h>
#include <kernel/wait.h>
#include <kernel/utsns.h>
#include <arch/user.h>

#include "convert.h"
#include "linux_abi.h"
#include "linux_internal.h"

#define LX_BRK_MAX (1ull << 30)
#define IOV_MAX 1024
#define SOCK_CHUNK 4096

/* One System V shared-memory attach of the current process: the address it
 * was mapped at, its size, and a reference-holding pointer to the segment
 * record (not a shmid, which may be gone after IPC_RMID). */
struct lx_shm_attach {
    struct lx_shm_attach *next;
    uint64_t addr;
    size_t size;
    uint64_t tag;              /* the mapping's VM identity, for shmdt */
    struct shm_segment *seg;
};

struct linux_state {
    uint64_t brk_start, brk;
    uint64_t sigtramp;   /* the trampoline page, drawn per exec (SEC-4); 0 on x86-64 */
    unsigned unknown_syscalls;
    bool wx_logged;   /* lx_wx_refused has said why, once per exec */
    struct lx_shm_attach *shm_attaches;   /* singly linked; head insert */
    spinlock_t shm_lock;                  /* guards shm_attaches (threads share it) */
};

/* --- process hooks (kernel/process/process.c) --------------------------------- */

/* The calling process's file-creation mask (lx_umask). */
static uint32_t lx_umask_get(void)
{
    return __atomic_load_n(&process_current()->umask, __ATOMIC_RELAXED);
}

static const syscall_fn *linux_table_get(void);

static int linux_process_init(struct process *p, const struct elf_info *info)
{
    (void)linux_table_get();
    struct linux_state *ls = kzalloc(sizeof(*ls));
    if (ls == NULL)
        return -ENOMEM;
    /* The break starts a random number of pages above the image, up to
     * 2^ASLR_BRK_BITS (roadmap M5, as Linux's randomize_va_space=2); the
     * gap below it stays unmapped. Drawn once per exec. The image is
     * loaded already: a fixed-address interpreter there pushes the start
     * to the first free page above the draw, so brk(0) never names a page
     * someone else maps (PR #369 review). */
    uint64_t brk = page_align_up(info->hi) + ((random_u64() & (((uint64_t)1 << ASLR_BRK_BITS) - 1)) << PAGE_SHIFT);
    uint64_t free = vm_user_find_free(p->space, brk, PAGE_SIZE);
    ls->brk_start = free != 0 ? free : brk;
    ls->brk = ls->brk_start;
    spinlock_init(&ls->shm_lock, "lx-shm-attach");
    p->linux = ls;
    return linux_sigtramp_map(p, &ls->sigtramp);
}

uint64_t linux_sigtramp_addr(const struct process *p)
{
    const struct linux_state *ls = p->linux;
    return ls != NULL ? ls->sigtramp : 0;
}

static void linux_process_release(struct process *p)
{
    struct linux_state *ls = p->linux;
    if (ls == NULL)
        return;   /* the build failed before linux_process_init ran */
    /* Detach any shm segments still attached: drop each one's live-attach count
     * and the attach's reference on its record (freeing the record and its
     * backing if it was the last and the segment was removed). The mappings
     * themselves are freed by the address-space teardown. Detach the whole
     * list under the lock first, then drop the references outside it (the free
     * path must not run under a spinlock). */
    arch_irq_state_t st = spin_lock_irqsave(&ls->shm_lock);
    struct lx_shm_attach *a = ls->shm_attaches;
    ls->shm_attaches = NULL;
    spin_unlock_irqrestore(&ls->shm_lock, st);
    while (a != NULL) {
        struct lx_shm_attach *next = a->next;
        shm_detach(a->seg);
        kfree(a);
        a = next;
    }
    kfree(ls);
    p->linux = NULL;
}

/*
 * fork: the child's own Linux state. The break is where the parent's is
 * (the heap was copied with the space). Every SysV shm attach is copied
 * too: vm_space_fork gave the child each attach's mapping under the same
 * tag, so a record with that tag lets the child's shmdt remove exactly
 * it, and the segment counts the child as one more attach. A vfork child
 * borrows the parent's space, attaches and all, and records none.
 */
static int linux_process_fork(struct process *parent, struct process *child)
{
    struct linux_state *pls = parent->linux;
    struct linux_state *ls = kzalloc(sizeof(*ls));
    if (ls == NULL)
        return -ENOMEM;
    spinlock_init(&ls->shm_lock, "lx-shm-attach");
    ls->brk_start = pls->brk_start;
    ls->brk = pls->brk;
    ls->sigtramp = pls->sigtramp;   /* the page came with the copied space */
    child->linux = ls;   /* linux_process_release frees it, records and all, if the fork fails */
    if (child->space == parent->space)
        return 0;
    /* Records are allocated outside the parent's lock; a list that grew
     * meanwhile (another thread's shmat) sends the count round again. */
    for (;;) {
        unsigned n = 0;
        arch_irq_state_t st = spin_lock_irqsave(&pls->shm_lock);
        for (struct lx_shm_attach *a = pls->shm_attaches; a != NULL; a = a->next)
            n++;
        spin_unlock_irqrestore(&pls->shm_lock, st);
        struct lx_shm_attach *pool = NULL;
        for (unsigned i = 0; i < n; i++) {
            struct lx_shm_attach *r = kmalloc(sizeof(*r), 0);
            if (r == NULL) {
                while (pool != NULL) {
                    struct lx_shm_attach *next = pool->next;
                    kfree(pool);
                    pool = next;
                }
                return -ENOMEM;
            }
            r->next = pool;
            pool = r;
        }
        st = spin_lock_irqsave(&pls->shm_lock);
        unsigned m = 0;
        for (struct lx_shm_attach *a = pls->shm_attaches; a != NULL; a = a->next)
            m++;
        if (m <= n) {
            struct lx_shm_attach **tail = &ls->shm_attaches;   /* the parent's order */
            for (struct lx_shm_attach *a = pls->shm_attaches; a != NULL; a = a->next) {
                struct lx_shm_attach *r = pool;
                pool = r->next;
                r->next = NULL;
                r->addr = a->addr;
                r->size = a->size;
                r->tag = a->tag;
                r->seg = a->seg;
                shm_attach_dup(a->seg);
                *tail = r;
                tail = &r->next;
            }
        }
        spin_unlock_irqrestore(&pls->shm_lock, st);
        while (pool != NULL) {
            struct lx_shm_attach *next = pool->next;
            kfree(pool);
            pool = next;
        }
        if (m <= n)
            return 0;
    }
}

/* exec: the state for a new Linux image -- the old attaches detached (their
 * mappings went with the old space), a new break, the trampoline mapped in
 * the new space. */
static int linux_process_exec(struct process *p, const struct elf_info *info)
{
    linux_process_release(p);
    return linux_process_init(p, info);
}

static unsigned linux_auxv(struct process *p, const struct elf_info *info, const struct personality_auxv_args *x, uint64_t *w,
                    unsigned max)
{
    (void)p;   /* the credentials come snapshotted in x */
    unsigned k = 0;
    /* Every pair fits (PERSONALITY_AUXV_WORDS): a pair dropped for room
     * would leave the vector without its AT_NULL. */
#define AUX(t, v)                                                                        \
    do {                                                                                 \
        KASSERT(k + 2 <= max);                                                           \
        w[k++] = (t);                                                                    \
        w[k++] = (v);                                                                    \
    } while (0)
    /* AT_SECURE, as Linux decides it for an exec that changes no
     * credentials (setuid bits are ignored here): the effective ids are
     * not the real ones. musl's dynamic linker then ignores
     * LD_LIBRARY_PATH and LD_PRELOAD (roadmap 1.1 P4). */
    bool secure = x->euid != x->ruid || x->egid != x->rgid;
    AUX(LX_AT_PHDR, info->phdr_vaddr);
    AUX(LX_AT_PHENT, info->phent);
    AUX(LX_AT_PHNUM, info->phnum);
    AUX(LX_AT_PAGESZ, PAGE_SIZE);
    AUX(LX_AT_BASE, x->interp_base);
    AUX(LX_AT_ENTRY, info->entry);
    AUX(LX_AT_RANDOM, x->random_addr);
    AUX(LX_AT_EXECFN, x->execfn_addr);
    AUX(LX_AT_PLATFORM, x->platform_addr);
    AUX(LX_AT_UID, x->ruid);
    AUX(LX_AT_EUID, x->euid);
    AUX(LX_AT_GID, x->rgid);
    AUX(LX_AT_EGID, x->egid);
    AUX(LX_AT_SECURE, secure ? 1 : 0);
    AUX(LX_AT_HWCAP, arch_user_hwcap());
    AUX(LX_AT_HWCAP2, arch_user_hwcap2());
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

/*
 * The directory a relative path in an *at call resolves from, referenced
 * (docs/audit/next-subsystem-dirfd.md, invariant P31). AT_FDCWD, or an
 * absolute path -- which ignores its base -- gives the working directory;
 * any other descriptor must be an open directory: -EBADF if no handle or
 * not the right the call needs, -ENOTDIR if not a directory. The right
 * is the handle's capability, not the directory's permission bits: READ
 * to look a name up, WRITE to add, remove or rename an entry -- so a
 * directory handle delegated read-only stays read-only (found in review:
 * the first build demanded none). A directory opened at either door
 * carries both, which is what Linux's open-a-directory-read-only then
 * unlinkat relies on. The directory is referenced before the handle's reference goes,
 * so a sibling thread closing the descriptor mid-call cannot free the base
 * under the walk (V35). With `basepath`, also the base's name, from the
 * same snapshot: the cwd's path, or the directory file's recorded one
 * (empty if it has none), for a caller that must record a new name.
 * With `have`, the base's own rights, which bound what a handle derived
 * through it may carry (review: a READ-only base must not yield a child
 * directory handle with WRITE).
 *
 * This replaces a check that answered every real descriptor -ENOSYS on
 * the premise that the VFS could not resolve from one. It always could:
 * every entry point takes a start.
 */
static int at_base(int64_t dirfd, const char *path, unsigned rights, struct vnode **out, char *basepath, size_t n,
                   unsigned *have)
{
    if ((int)dirfd == LX_AT_FDCWD || path[0] == '/') {
        *out = basepath ? process_cwd_snapshot(basepath, n) : process_cwd_get();
        if (have)
            *have = HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE;   /* the process's own names */
        return 0;
    }
    unsigned held;
    struct kobject *obj = handle_get(&process_current()->handles, (int)dirfd, &held);
    if (obj == NULL)
        return -EBADF;
    if ((held & rights) != rights) {
        kobject_put(obj);
        return -EBADF;
    }
    if (have)
        *have = held;
    struct file *f = file_from_kobject(obj);
    if (f == NULL) {
        kobject_put(obj);
        return -ENOTDIR;   /* a socket or a pipe end is open and is not a directory */
    }
    if (f->vn->type != VNODE_DIR) {
        file_put(f);
        return -ENOTDIR;
    }
    /* Opened under another root -- before a chroot, or by a process with
     * a wider one: not a way out of this process's root (P39). */
    if (!file_dir_base_ok(f)) {
        file_put(f);
        return -EACCES;
    }
    vnode_get(f->vn);
    *out = f->vn;
    if (basepath)
        strlcpy(basepath, f->dir_path ? f->dir_path : "", n);
    file_put(f);
    return 0;
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

/*
 * A file whose metadata the caller changes through a descriptor (fchmod,
 * fchown, fchownat's AT_EMPTY_PATH): the handle needs MANAGE -- a change of
 * how the object behaves, not of what it holds -- so a handle delegated
 * without it cannot change the mode or the owner (found in review). Every
 * Linux open grants MANAGE (HANDLE_RIGHT_OWNER). *err is -EPERM for a
 * handle without the right, as sys_setnonblock, else -EBADF.
 */
static struct file *file_of_manage(int h, int *err)
{
    bool no_rights;
    struct kobject *obj = handle_lookup_rights(&process_current()->handles, h, HANDLE_RIGHT_MANAGE, &no_rights);
    *err = no_rights ? -EPERM : -EBADF;
    if (obj == NULL)
        return NULL;
    struct file *f = file_from_kobject(obj);
    if (f == NULL)
        kobject_put(obj);
    return f;
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
    /* The syscall layer's bounce (kernel/syscall.h): up to 64 KiB per
     * file call, the stack chunk when the heap refuses. */
    char stack[IO_CHUNK];
    struct io_bounce b;
    syscall_bounce_get(&b, stack, len);
    int64_t done = 0, rc = 0;
    while ((size_t)done < len) {
        size_t n = len - (size_t)done < b.cap ? len - (size_t)done : b.cap;
        if (write) {
            if (copy_from_user(b.buf, ubuf + (uint64_t)done, n)) {
                rc = -EFAULT;
                break;
            }
            rc = file_pwrite(f, b.buf, n, (uint64_t)(off + done));
        } else {
            rc = file_pread(f, b.buf, n, (uint64_t)(off + done));
            if (rc > 0 && copy_to_user(ubuf + (uint64_t)done, b.buf, (size_t)rc))
                rc = -EFAULT;
        }
        if (rc <= 0)
            break;
        done += rc;
        if ((size_t)rc < n)
            break;
    }
    syscall_bounce_put(&b);
    file_put(f);
    return done > 0 ? done : rc;
}

static int64_t lx_pread64(struct syscall_args *a) { return rw_at(a, false); }
static int64_t lx_pwrite64(struct syscall_args *a) { return rw_at(a, true); }

static int64_t lx_readv(struct syscall_args *a)
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
        int64_t rc = syscall_handle_read(h, iov.iov_base, (size_t)iov.iov_len);
        if (rc < 0)
            return done ? done : rc;
        done += rc;
        if ((uint64_t)rc < iov.iov_len)
            break;
    }
    return done;
}

/* Hands the gathered bytes to the object; false when writev must stop
 * (an error in *rc, or a short write). */
static bool writev_flush(const struct kobject_io_type *io, struct kobject *obj, const char *buf,
                         size_t *fill, int64_t *done, int64_t *rc)
{
    if (*fill == 0)
        return true;
    *rc = io->write(obj, buf, *fill);
    if (*rc < 0)
        return false;
    KASSERT(*rc <= (int64_t)*fill);
    *done += *rc;
    bool whole = (size_t)*rc == *fill;
    *fill = 0;
    return whole;
}

/* writev is one write, as on Linux: the vector is gathered into the
 * syscall layer's bounce and handed to the object whole, so a datagram
 * socket sends one datagram, and a terminal or pipe gets the pieces of a
 * line in one call, with no other writer's line between them. A vector
 * longer than the bounce (64 KiB) goes in bounce-sized calls, as one
 * long write does. */
static int64_t lx_writev(struct syscall_args *a)
{
    int h = (int)a->a[0];
    uint64_t uiov = a->a[1];
    int64_t cnt = (int64_t)a->a[2];
    if (cnt < 0 || cnt > IOV_MAX)
        return -EINVAL;
    struct kobject *obj = handle_lookup(&process_current()->handles, h, HANDLE_RIGHT_WRITE);
    if (obj == NULL)
        return -EBADF;
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->write == NULL) {
        kobject_put(obj);
        return -EBADF;
    }
    /* The vector is copied in once, as Linux imports it: the total that
     * sizes the bounce and the entries gathered are the same snapshot,
     * whatever another thread does to the user array meanwhile. A short
     * vector (Linux's UIO_FASTIOV, 8) stays on the stack. */
    struct lx_iovec fast[8];
    struct lx_iovec *vec = fast;
    if (cnt > (int64_t)(sizeof(fast) / sizeof(fast[0]))) {
        vec = kmalloc((size_t)cnt * sizeof(*vec), 0);
        if (vec == NULL) {
            kobject_put(obj);
            return -ENOMEM;
        }
    }
    int64_t done = 0, rc = 0;
    size_t total = 0;
    if (cnt > 0 && copy_from_user(vec, uiov, (size_t)cnt * sizeof(*vec)))
        rc = -EFAULT;
    for (int64_t i = 0; i < cnt && rc == 0; i++) {
        if (vec[i].iov_len > (uint64_t)INT64_MAX - total)
            rc = -EINVAL;
        else
            total += (size_t)vec[i].iov_len;
    }
    if (rc) {
        if (vec != fast)
            kfree(vec);
        kobject_put(obj);
        return rc;
    }
    char stack[IO_CHUNK];
    struct io_bounce b;
    syscall_bounce_get(&b, stack, total);
    size_t fill = 0;
    bool more = true;
    for (int64_t i = 0; i < cnt && more; i++) {
        struct lx_iovec iov = vec[i];
        for (uint64_t off = 0; off < iov.iov_len && more;) {
            size_t n = iov.iov_len - off < b.cap - fill ? (size_t)(iov.iov_len - off) : b.cap - fill;
            if (!user_range_ok(iov.iov_base + off, n) || copy_from_user(b.buf + fill, iov.iov_base + off, n)) {
                rc = -EFAULT;
                more = false;
                break;
            }
            fill += n;
            off += n;
            if (fill == b.cap)
                more = writev_flush(io, obj, b.buf, &fill, &done, &rc);
        }
    }
    /* What was gathered before a fault is still written, as Linux writes
     * what it copied. */
    if (rc >= 0 || rc == -EFAULT) {
        int64_t gathered = rc;
        if (writev_flush(io, obj, b.buf, &fill, &done, &rc))
            rc = gathered;
    }
    syscall_bounce_put(&b);
    if (vec != fast)
        kfree(vec);
    kobject_put(obj);
    return done > 0 ? done : rc;
}

/* The three path buffers (3 KiB) live on the heap, not the 16 KiB kernel
 * stack (scripts/frame-baseline.txt). */
static int64_t do_open(int64_t dirfd, uint64_t upath, unsigned lxflags, uint32_t mode)
{
    char *path = kmalloc(3 * VFS_PATH_MAX, 0);
    if (path == NULL)
        return -ENOMEM;
    char *base = path + VFS_PATH_MAX;
    char *name = base + VFS_PATH_MAX;
    int64_t rc = get_path(upath, path);
    if (rc)
        goto out;
    unsigned flags;
    if (lx_open_flags(lxflags, &flags) < 0) {
        rc = -EINVAL;
        goto out;
    }
    struct file *f;
    struct vnode *start;
    /* Creating, truncating or opening for writing puts data in through
     * the base: it needs WRITE, as an entry change does. */
    unsigned acc = flags & COSMO_O_ACCMODE, have;
    bool writes = (flags & (COSMO_O_CREAT | COSMO_O_TRUNC)) || acc == COSMO_O_WRONLY || acc == COSMO_O_RDWR;
    unsigned need = HANDLE_RIGHT_READ | (writes ? HANDLE_RIGHT_WRITE : 0);
    rc = at_base(dirfd, path, need, &start, base, VFS_PATH_MAX, &have);
    if (rc)
        goto out;
    /* A directory remembers the name its own walk took to it, for fchdir
     * (P31, P32); a relative path from a base with no name gives it
     * none, and fchdir then refuses it. */
    rc = vfs_open_named(start, base, path, flags, mode & 07777u & ~lx_umask_get(), &f, name, VFS_PATH_MAX);
    vnode_put(start);
    if (rc)
        goto out;
    if (f->vn->type == VNODE_DIR)
        file_set_dir_path(f, name);
    unsigned rights = HANDLE_RIGHT_OWNER;   /* as the native open: the file is the caller's to dup and pass */
    if (acc == COSMO_O_RDONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_READ;
    if (acc == COSMO_O_WRONLY || acc == COSMO_O_RDWR)
        rights |= HANDLE_RIGHT_WRITE;
    /* A directory opens read-only and is written through by the *at
     * calls: its handle carries WRITE, the right to change its entries,
     * which a delegation may narrow away (P31). Writing to it as a file
     * is still -EISDIR. */
    if (f->vn->type == VNODE_DIR)
        rights |= HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE;
    /* Rights only shrink on the way down: what is derived through a
     * descriptor carries no data right the descriptor lacks (P31). */
    rights &= have | ~(HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    rc = handle_install_flags(&process_current()->handles, &f->obj, rights,
                              (lxflags & LX_O_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    file_put(f);
out:
    kfree(path);
    return rc;
}

static __maybe_unused int64_t lx_open(struct syscall_args *a)
{
    return do_open(LX_AT_FDCWD, a->a[0], (unsigned)a->a[1], (uint32_t)a->a[2]);
}
static __maybe_unused int64_t lx_creat(struct syscall_args *a)
{
    return do_open(LX_AT_FDCWD, a->a[0], LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, (uint32_t)a->a[1]);
}
static int64_t lx_openat(struct syscall_args *a)
{
    return do_open((int64_t)a->a[0], a->a[1], (unsigned)a->a[2], (uint32_t)a->a[3]);
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
    struct vnode *cwd = process_cwd_get();
    rc = vfs_stat(cwd, path, &st);
    vnode_put(cwd);
    return rc ? rc : stat_out(&st, a->a[1]);
}

/* lstat: the link itself, which is the whole point of the call. Until
 * symbolic links existed this was an alias for stat, which answered the
 * opposite of what a caller asks for (docs/audit/next-subsystem-symlink.md). */
static __maybe_unused int64_t lx_lstat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct cosmo_stat st;
    struct vnode *cwd = process_cwd_get();
    rc = vfs_lstat(cwd, path, &st);
    vnode_put(cwd);
    return rc ? rc : stat_out(&st, a->a[1]);
}

/* readlink(2) and readlinkat(2): the bytes, never terminated, truncated
 * to the caller's buffer. */
static int64_t readlink_common(int64_t dirfd, uint64_t upath, uint64_t ubuf, size_t len)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(upath, path);
    if (rc)
        return rc;
    if (len == 0)
        return -EINVAL;
    if (len > VFS_PATH_MAX)
        len = VFS_PATH_MAX;
    struct vnode *start;
    rc = at_base(dirfd, path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    char *buf = kmalloc(len, 0);
    if (buf == NULL) {
        vnode_put(start);
        return -ENOMEM;
    }
    rc = vfs_readlink(start, path, buf, len);
    vnode_put(start);
    if (rc > 0 && copy_to_user(ubuf, buf, (size_t)rc) != 0)
        rc = -EFAULT;
    kfree(buf);
    return rc;
}

static __maybe_unused int64_t lx_readlink(struct syscall_args *a)
{
    return readlink_common(LX_AT_FDCWD, a->a[0], a->a[1], (size_t)a->a[2]);
}

static int64_t lx_readlinkat(struct syscall_args *a)
{
    return readlink_common((int64_t)a->a[0], a->a[1], a->a[2], (size_t)a->a[3]);
}

/* The link is made relative to `dirfd`; its target is stored verbatim and
 * resolved when the link is followed, as Linux does. */
static int64_t symlink_common(uint64_t utarget, int64_t dirfd, uint64_t upath)
{
    char *target = kmalloc(2 * VFS_PATH_MAX, 0);   /* heap: see do_open */
    if (target == NULL)
        return -ENOMEM;
    char *path = target + VFS_PATH_MAX;
    int rc = strncpy_from_user(target, utarget, VFS_PATH_MAX);
    if (rc >= 0)
        rc = get_path(upath, path);
    struct vnode *start;
    if (rc >= 0)
        rc = at_base(dirfd, path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
    if (rc >= 0) {
        rc = vfs_symlink(start, path, target);
        vnode_put(start);
    }
    kfree(target);
    return rc;
}

static __maybe_unused int64_t lx_symlink(struct syscall_args *a)
{
    return symlink_common(a->a[0], LX_AT_FDCWD, a->a[1]);
}

static int64_t lx_symlinkat(struct syscall_args *a)
{
    return symlink_common(a->a[0], (int64_t)a->a[1], a->a[2]);
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
    struct vnode *start;
    rc = at_base((int64_t)a->a[0], path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    struct cosmo_stat st;
    /* AT_SYMLINK_NOFOLLOW was read and dropped until links existed. */
    rc = (flags & LX_AT_SYMLINK_NOFOLLOW) ? vfs_lstat(start, path, &st) : vfs_stat(start, path, &st);
    vnode_put(start);
    return rc ? rc : stat_out(&st, a->a[2]);
}

static int64_t lx_statx(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[2];
    uint32_t mask = (uint32_t)a->a[3];
    uint64_t ubuf = a->a[4];
    /* Linux rejects an impossible sync request (both FORCE and DONT) and any
     * reserved mask bit before touching the file. */
    if ((flags & LX_AT_STATX_SYNC_TYPE) == LX_AT_STATX_SYNC_TYPE || (mask & LX_STATX__RESERVED))
        return -EINVAL;
    char path[VFS_PATH_MAX];
    int rc = strncpy_from_user(path, a->a[1], VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    struct cosmo_stat st;
    if (path[0] == '\0') {
        if (!(flags & LX_AT_EMPTY_PATH))
            return -ENOENT;
        if ((int)a->a[0] == LX_AT_FDCWD) {
            /* Empty path + AT_FDCWD: the current directory itself. Stat the
             * vnode directly -- a "." lookup would demand search permission on
             * the cwd, which an empty-path stat must not require. */
            struct vnode *cwd = process_cwd_get();
            mutex_lock(&cwd->lock);   /* vnode_stat reads several fields; the lock the other stat paths hold keeps them one moment's values */
            vnode_stat(cwd, &st);
            mutex_unlock(&cwd->lock);
            vnode_put(cwd);
            rc = 0;
        } else {
            rc = syscall_handle_stat((int)a->a[0], &st);   /* the fd itself */
        }
    } else {
        struct vnode *start;
        rc = at_base((int64_t)a->a[0], path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
        if (rc)
            return rc;
        rc = (flags & LX_AT_SYMLINK_NOFOLLOW) ? vfs_lstat(start, path, &st) : vfs_stat(start, path, &st);
        vnode_put(start);
    }
    if (rc)
        return rc;
    struct lx_statx sx;
    lx_statx_from_native(&st, mask, &sx);
    return copy_to_user(ubuf, &sx, sizeof(sx)) ? -EFAULT : 0;
}

static int64_t do_eventfd(uint32_t initval, unsigned flags)
{
    if (flags & ~(unsigned)(LX_EFD_SEMAPHORE | LX_EFD_NONBLOCK | LX_EFD_CLOEXEC))
        return -EINVAL;
    struct kobject *obj;
    /* Linux's initval is an unsigned int: only the low 32 bits count, so a
     * caller leaving junk in the high half of the register starts at the
     * value it asked for, not that junk. */
    int rc = eventfd_obj_create(initval, (flags & LX_EFD_SEMAPHORE) != 0, &obj);
    if (rc)
        return rc;
    if (flags & LX_EFD_NONBLOCK)
        kobject_set_nonblock(obj, 1);
    int h = handle_install_flags(&process_current()->handles, obj,
                                 HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_OWNER,
                                 (flags & LX_EFD_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    kobject_put(obj);   /* the handle holds its own reference */
    return h;
}

static int64_t lx_eventfd2(struct syscall_args *a) { return do_eventfd((uint32_t)a->a[0], (unsigned)a->a[1]); }
static __maybe_unused int64_t lx_eventfd(struct syscall_args *a) { return do_eventfd((uint32_t)a->a[0], 0); }   /* the older call: no flags */

/* signalfd: a signal-reading fd on the signalfd kobject (kernel/io/signalfd.c).
 * See docs/audit/next-subsystem-signalfd.md. */
static int64_t do_signalfd(int fd, uint64_t umask, size_t sizemask, unsigned flags)
{
    if (flags & ~(unsigned)(LX_SFD_CLOEXEC | LX_SFD_NONBLOCK))
        return -EINVAL;
    if (sizemask != sizeof(uint64_t))
        return -EINVAL;
    uint64_t mask;
    if (copy_from_user(&mask, umask, sizeof(mask)))
        return -EFAULT;
    mask &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));   /* never reportable via a signalfd */

    if (fd != -1) {
        /* Update an existing signalfd's mask; only -1 creates, as Linux
         * documents (a negative fd other than -1 is -EBADF). The holder must
         * have the read right, so a handle whose rights were narrowed cannot
         * change what another holder reads. */
        if (fd < 0)
            return -EBADF;
        struct kobject *obj = handle_lookup(&process_current()->handles, fd, HANDLE_RIGHT_READ);
        if (obj == NULL)
            return -EBADF;
        int rc = signalfd_obj_set_mask(obj, mask);
        kobject_put(obj);
        return rc ? rc : fd;
    }

    struct kobject *obj;
    int rc = signalfd_obj_create(mask, (flags & LX_SFD_NONBLOCK) != 0, &obj);
    if (rc)
        return rc;
    int h = handle_install_flags(&process_current()->handles, obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_OWNER,
                                 (flags & LX_SFD_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    kobject_put(obj);
    return h;
}
static int64_t lx_signalfd4(struct syscall_args *a)
{
    return do_signalfd((int)a->a[0], a->a[1], (size_t)a->a[2], (unsigned)a->a[3]);
}
static __maybe_unused int64_t lx_signalfd(struct syscall_args *a)
{
    return do_signalfd((int)a->a[0], a->a[1], (size_t)a->a[2], 0);   /* the older call: no flags */
}

/* timerfd: a waitable timer fd on the timer kobject (kernel/io/timerobj.c).
 * See docs/audit/next-subsystem-timerfd.md. */
static int64_t do_timerfd_create(unsigned clockid, unsigned flags)
{
    if (clockid != LX_CLOCK_MONOTONIC && clockid != LX_CLOCK_REALTIME && clockid != LX_CLOCK_BOOTTIME)
        return -EINVAL;   /* BOOTTIME is accepted and aliased to monotonic (no suspend) */
    if (flags & ~(unsigned)(LX_TFD_NONBLOCK | LX_TFD_CLOEXEC))
        return -EINVAL;
    struct kobject *obj;
    int rc = timer_obj_create_disarmed((flags & LX_TFD_NONBLOCK) != 0,
                                       clockid == LX_CLOCK_REALTIME, &obj);
    if (rc)
        return rc;
    /* A timerfd is read-only. */
    int h = handle_install_flags(&process_current()->handles, obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_OWNER,
                                 (flags & LX_TFD_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    kobject_put(obj);
    return h;
}

static int64_t do_timerfd_settime(int fd, unsigned flags, uint64_t unew, uint64_t uold)
{
    if (flags & ~(unsigned)LX_TFD_TIMER_ABSTIME)
        return -EINVAL;
    struct kobject *obj = handle_lookup(&process_current()->handles, fd, HANDLE_RIGHT_READ);
    if (obj == NULL)
        return -EBADF;
    if (timer_obj_from_kobject(obj) == NULL) {
        kobject_put(obj);
        return -EINVAL;   /* not a timerfd */
    }
    uint64_t value_ns, interval_ns;
    int rc = ns_from_timespec(unew + offsetof(struct lx_itimerspec, it_value), &value_ns);
    if (!rc)
        rc = ns_from_timespec(unew + offsetof(struct lx_itimerspec, it_interval), &interval_ns);
    if (rc) {
        kobject_put(obj);
        return rc;
    }
    /* The disarm decision is the raw it_value, never a computed delay: a zero
     * it_value disarms, a non-zero one always arms. */
    uint64_t initial_ns;
    if (value_ns == 0) {
        initial_ns = 0;
    } else if (flags & LX_TFD_TIMER_ABSTIME) {
        uint64_t now = timer_obj_is_realtime(obj) ? clock_realtime_ns() : clock_now_ns();
        /* A deadline already in the past fires at once (minimum 1 ns, never 0 --
         * 0 would be read as a disarm), matching Linux. */
        initial_ns = value_ns > now ? value_ns - now : 1;
    } else {
        initial_ns = value_ns;
    }
    uint64_t old_rem = 0, old_int = 0;
    /* Pass the interval even when disarming: Linux keeps it, and
     * timerfd_gettime reports it after a disarm. */
    timer_obj_settime(obj, initial_ns, interval_ns, &old_rem, &old_int);
    kobject_put(obj);
    if (uold) {
        if (put_timespec(uold + offsetof(struct lx_itimerspec, it_value), old_rem) ||
            put_timespec(uold + offsetof(struct lx_itimerspec, it_interval), old_int))
            return -EFAULT;
    }
    return 0;
}

static int64_t do_timerfd_gettime(int fd, uint64_t ucurr)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, fd, HANDLE_RIGHT_READ);
    if (obj == NULL)
        return -EBADF;
    if (timer_obj_from_kobject(obj) == NULL) {
        kobject_put(obj);
        return -EINVAL;
    }
    uint64_t rem = 0, intv = 0;
    timer_obj_gettime(obj, &rem, &intv);
    kobject_put(obj);
    if (put_timespec(ucurr + offsetof(struct lx_itimerspec, it_value), rem) ||
        put_timespec(ucurr + offsetof(struct lx_itimerspec, it_interval), intv))
        return -EFAULT;
    return 0;
}

static int64_t lx_timerfd_create(struct syscall_args *a) { return do_timerfd_create((unsigned)a->a[0], (unsigned)a->a[1]); }
static int64_t lx_timerfd_settime(struct syscall_args *a) { return do_timerfd_settime((int)a->a[0], (unsigned)a->a[1], a->a[2], a->a[3]); }
static int64_t lx_timerfd_gettime(struct syscall_args *a) { return do_timerfd_gettime((int)a->a[0], a->a[1]); }

/* memfd_create: an anonymous memory-backed file (a ramfs regular file with no
 * name). See docs/audit/next-subsystem-memfd.md. */
static int64_t lx_memfd_create(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[1];
    if (flags & ~(unsigned)LX_MFD_CLOEXEC)
        return -EINVAL;   /* MFD_ALLOW_SEALING / MFD_HUGETLB unsupported */
    /* The name is advisory (Linux uses it only for /proc/self/fd and
     * accounting): validate that it is a readable, bounded string, then
     * ignore it. Linux caps it at 249 bytes and returns -EINVAL for a longer
     * one, so a 250-byte buffer turns strncpy_from_user's -ENAMETOOLONG into
     * -EINVAL; a faulting pointer stays -EFAULT. */
    char name[250];
    int rc = strncpy_from_user(name, a->a[0], sizeof(name));
    if (rc == -EFAULT)
        return -EFAULT;
    if (rc < 0)
        return -EINVAL;   /* too long (> 249), as Linux reports it */
    struct vnode *vn;
    rc = ramfs_anon_reg(0600, &vn);
    if (rc)
        return rc;
    struct file *f;
    rc = vfs_open_vnode(vn, COSMO_O_RDWR, &f);   /* consumes vn's reference */
    if (rc)
        return rc;
    int h = handle_install_flags(&process_current()->handles, &f->obj,
                           HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_OWNER,
                                 (flags & LX_MFD_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    file_put(f);   /* the table holds its own reference */
    return h;
}

static int64_t lx_ftruncate(struct syscall_args *a)
{
    int64_t len = (int64_t)a->a[1];
    if (len < 0)
        return -EINVAL;
    struct file *f = file_of((int)a->a[0], HANDLE_RIGHT_WRITE);   /* must be open for writing */
    if (f == NULL)
        return -EBADF;
    int rc = (int)vfs_ftruncate(f->vn, (uint64_t)len);
    file_put(f);
    return rc;
}

/* epoll: an interest set of fds, waited on together (kernel/io/epoll.c).
 * See docs/audit/next-subsystem-epoll.md. Level-triggered. */
#define LX_EPOLL_WAIT_MAX 1024u   /* per-call cap on reported events (level-triggered re-reports) */

static unsigned epoll_events_to_io(uint32_t ev)
{
    unsigned io = 0;
    if (ev & LX_EPOLLIN)
        io |= COSMO_IO_READABLE;
    if (ev & LX_EPOLLOUT)
        io |= COSMO_IO_WRITABLE;
    return io;   /* ERR/HUP are always reported, never requested */
}

static uint32_t epoll_events_from_io(unsigned io, uint32_t asked)
{
    uint32_t ev = 0;
    if (io & COSMO_IO_READABLE)
        ev |= LX_EPOLLIN;
    if (io & COSMO_IO_WRITABLE)
        ev |= LX_EPOLLOUT;
    if (io & COSMO_IO_HANGUP)
        ev |= LX_EPOLLHUP | (asked & LX_EPOLLRDHUP);   /* RDHUP only if the caller asked, as poll does */
    if (io & COSMO_IO_ERROR)
        ev |= LX_EPOLLERR;
    return ev;
}

/* Resolve an epoll fd to its object, referenced. NULL with *err set. */
static struct kobject *epoll_of_fd(int epfd, int *err)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, epfd, 0);
    if (obj == NULL) {
        *err = -EBADF;
        return NULL;
    }
    if (epoll_obj_from_kobject(obj) == NULL) {
        kobject_put(obj);
        *err = -EINVAL;   /* not an epoll */
        return NULL;
    }
    return obj;
}

static int64_t do_epoll_create(unsigned flags)
{
    if (flags & ~(unsigned)LX_EPOLL_CLOEXEC)
        return -EINVAL;
    struct kobject *obj;
    int rc = epoll_obj_create(&obj);
    if (rc)
        return rc;
    int h = handle_install_flags(&process_current()->handles, obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_OWNER,
                                 (flags & LX_EPOLL_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
    kobject_put(obj);
    return h;
}

static int64_t lx_epoll_create1(struct syscall_args *a) { return do_epoll_create((unsigned)a->a[0]); }

static __maybe_unused int64_t lx_epoll_create(struct syscall_args *a)
{
    if ((int)a->a[0] <= 0)
        return -EINVAL;   /* the legacy call wants a positive size hint, then ignores it */
    return do_epoll_create(0);
}

static int64_t lx_epoll_ctl(struct syscall_args *a)
{
    int op = (int)a->a[1];
    int fd = (int)a->a[2];
    int err = 0;
    struct kobject *ep = epoll_of_fd((int)a->a[0], &err);
    if (ep == NULL)
        return err;
    int64_t rc;
    if (op == LX_EPOLL_CTL_DEL) {
        rc = epoll_obj_del(ep, fd);
        kobject_put(ep);
        return rc;
    }
    if (op != LX_EPOLL_CTL_ADD && op != LX_EPOLL_CTL_MOD) {
        kobject_put(ep);
        return -EINVAL;
    }
    struct lx_epoll_event ev;
    if (copy_from_user(&ev, a->a[3], sizeof(ev))) {
        kobject_put(ep);
        return -EFAULT;
    }
    unsigned want = epoll_events_to_io(ev.events);
    bool oneshot = (ev.events & LX_EPOLLONESHOT) != 0;
    bool edge = (ev.events & LX_EPOLLET) != 0;   /* edge-triggered: report only on a transition */
    if (op == LX_EPOLL_CTL_MOD) {
        rc = epoll_obj_mod(ep, fd, want, ev.events, ev.data, oneshot, edge);
        kobject_put(ep);
        return rc;
    }
    /* ADD: the member must be a valid fd. A set may be a member of a set
     * (the epoll-callback unit): the object refuses the set itself (-EINVAL)
     * and a loop or a chain deeper than EPOLL_MAX_NESTS (-ELOOP), as Linux. */
    struct kobject *target = handle_lookup(&process_current()->handles, fd, 0);
    if (target == NULL) {
        kobject_put(ep);
        return -EBADF;
    }
    rc = epoll_obj_add(ep, fd, target, want, ev.events, ev.data, oneshot, edge);   /* takes target's ref on success */
    if (rc)
        kobject_put(target);
    kobject_put(ep);
    return rc;
}

static int64_t do_epoll_wait(int epfd, uint64_t uevents, int maxevents, int timeout_ms)
{
    if (maxevents <= 0)
        return -EINVAL;
    unsigned k = (unsigned)maxevents;
    if (k > LX_EPOLL_WAIT_MAX)
        k = LX_EPOLL_WAIT_MAX;
    if (!user_range_ok(uevents, (size_t)k * sizeof(struct lx_epoll_event)))
        return -EFAULT;
    int err = 0;
    struct kobject *ep = epoll_of_fd(epfd, &err);
    if (ep == NULL)
        return err;
    struct epoll_ready *buf = kmalloc((size_t)k * sizeof(*buf), 0);
    if (buf == NULL) {
        kobject_put(ep);
        return -ENOMEM;
    }
    uint64_t timeout_ns = timeout_ms < 0 ? EPOLL_WAIT_FOREVER : (uint64_t)timeout_ms * 1000000ull;
    int64_t n = epoll_obj_wait(ep, buf, k, timeout_ns);
    int64_t rc = n;
    if (n > 0) {
        int64_t i = 0;
        for (; i < n; i++) {
            struct lx_epoll_event ev = { .events = epoll_events_from_io(buf[i].io, buf[i].events),
                                         .data = buf[i].data };
            if (copy_to_user(uevents + (uint64_t)i * sizeof(ev), &ev, sizeof(ev)))
                break;
        }
        if (i < n) {
            /* The copy failed at i: re-arm what we could not deliver, so the
             * event is not lost -- a one-shot collect disabled, and an edge
             * member collect disarmed (its transition would otherwise be gone
             * until the member is drained and refills). */
            for (int64_t j = i; j < n; j++)
                if (buf[j].oneshot || buf[j].edge)
                    epoll_obj_rearm(ep, buf[j].fd, buf[j].id);
            rc = i ? i : -EFAULT;
        }
    }
    kfree(buf);
    kobject_put(ep);
    return rc;
}

static __maybe_unused int64_t lx_epoll_wait(struct syscall_args *a)
{
    return do_epoll_wait((int)a->a[0], a->a[1], (int)a->a[2], (int)a->a[3]);
}

static int64_t lx_epoll_pwait(struct syscall_args *a)
{
    /* Swap in the caller's signal mask for the wait and restore it after, the
     * way lx_ppoll does; a NULL mask is the common path. */
    bool swap = a->a[4] != 0;
    uint64_t mask = 0, old = 0;
    if (swap) {
        if (a->a[5] != 8)
            return -EINVAL;
        if (copy_from_user(&mask, a->a[4], 8))
            return -EFAULT;
        old = signal_blocked();
        signal_set_blocked(mask);
    }
    int64_t rc = do_epoll_wait((int)a->a[0], a->a[1], (int)a->a[2], (int)a->a[3]);
    if (swap)
        signal_set_blocked_saved(old);
    return rc;
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
    /* One reference for all four arms -- and one release, which is why
     * each arm sets `rc` instead of returning: an early return here would
     * leak the reference the walk is holding. */
    struct vnode *cwd = process_cwd_get();
    switch (which) {
    case 0: rc = vfs_mkdir(cwd, path, (uint32_t)a->a[1] & 07777u & ~lx_umask_get()); break;
    case 1: rc = vfs_rmdir(cwd, path); break;
    case 2: rc = vfs_unlink(cwd, path); break;
    case 3: {
        struct cosmo_stat st;
        rc = vfs_stat(cwd, path, &st);   /* access: existence */
        break;
    }
    default: rc = -ENOSYS; break;
    }
    vnode_put(cwd);
    return rc;
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
    struct vnode *start;
    rc = at_base((int64_t)a->a[0], path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    rc = vfs_mkdir(start, path, (uint32_t)a->a[2] & 07777u & ~lx_umask_get());
    vnode_put(start);
    return rc;
}

/* mknodat: a FIFO is the one node an unprivileged Linux caller may make
 * (S_IFIFO); a socket's name is made by bind (S_IFSOCK -EINVAL, as the
 * native mknod says); every other type is -EPERM, Linux's answer to a
 * caller without CAP_MKNOD. */
static int64_t do_mknod(int64_t dirfd, const char *path, uint32_t mode)
{
    int rc;
    switch (mode & LX_S_IFMT) {
    case LX_S_IFIFO:
        break;
    case LX_S_IFSOCK:
        return -EINVAL;
    default:
        return -EPERM;
    }
    struct vnode *start;
    rc = at_base(dirfd, path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    struct vnode *vn;
    rc = vfs_mknod(start, path, mode & 07777u & ~lx_umask_get(), VNODE_FIFO, &vn);
    vnode_put(start);
    if (rc == 0)
        vnode_put(vn);
    return rc;
}

static int64_t lx_mknodat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    return do_mknod((int64_t)a->a[0], path, (uint32_t)a->a[2]);
}

/* The legacy mknod (x86-64 only; AArch64 has the *at form alone), as
 * mkdir beside mkdirat. */
static __maybe_unused int64_t lx_mknod(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    return do_mknod(LX_AT_FDCWD, path, (uint32_t)a->a[1]);
}

/* --- modes and times (roadmap M3; BusyBox chmod and touch) ------------------- */

/* The node a path names from a directory descriptor, following a final
 * symbolic link unless `nofollow`. */
static int lookup_at(int64_t dirfd, uint64_t upath, bool nofollow, struct vnode **out)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(upath, path);
    if (rc)
        return rc;
    struct vnode *start;
    rc = at_base(dirfd, path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    /* A trailing slash makes the last component a directory to enter, so
     * a link there is followed even under AT_SYMLINK_NOFOLLOW, as Linux. */
    size_t plen = strlen(path);
    if (nofollow && plen > 0 && path[plen - 1] == '/')
        nofollow = false;
    rc = nofollow ? vfs_lookup_nofollow(start, path, out) : vfs_lookup(start, path, out);
    vnode_put(start);
    return rc;
}

static int64_t do_fchmodat(int64_t dirfd, uint64_t upath, uint32_t mode, unsigned flags)
{
    if (flags & ~(unsigned)LX_AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    struct vnode *vn;
    int rc = lookup_at(dirfd, upath, (flags & LX_AT_SYMLINK_NOFOLLOW) != 0, &vn);
    if (rc)
        return rc;
    /* A link's own mode means nothing: Linux refuses the change. */
    rc = vn->type == VNODE_LNK ? -EOPNOTSUPP : vfs_setattr(vn, VFS_SET_MODE, mode, 0);
    vnode_put(vn);
    return rc;
}

#ifdef LX_chmod
static int64_t lx_chmod(struct syscall_args *a) { return do_fchmodat(LX_AT_FDCWD, a->a[0], (uint32_t)a->a[1], 0); }
#endif
static int64_t lx_fchmodat(struct syscall_args *a)
{
    return do_fchmodat((int64_t)a->a[0], a->a[1], (uint32_t)a->a[2], 0);
}
static int64_t lx_fchmodat2(struct syscall_args *a)
{
    return do_fchmodat((int64_t)a->a[0], a->a[1], (uint32_t)a->a[2], (unsigned)a->a[3]);
}

static int64_t lx_fchmod(struct syscall_args *a)
{
    int err;
    struct file *f = file_of_manage((int)a->a[0], &err);
    if (f == NULL)
        return err;
    int rc = vfs_setattr(f->vn, VFS_SET_MODE, (uint32_t)a->a[1], 0);
    file_put(f);
    return rc;
}

/* --- ownership and the root (shell completeness S4) ----------------------------- */

/* Linux's uid_t and gid_t -1, "leave it": the VFS's VFS_ID_KEEP. */
static uint32_t lx_owner_id(uint64_t v)
{
    return (uint32_t)v == 0xffffffffu ? VFS_ID_KEEP : (uint32_t)v;
}

/*
 * fchownat(dirfd, path, uid, gid, flags), and chown, lchown and fchown
 * through it: vfs_chown's rule (PRIV_SYS_ADMIN for a new owner; the owner
 * may choose among its own groups). AT_SYMLINK_NOFOLLOW changes a link
 * itself, which is lchown; AT_EMPTY_PATH with an empty path changes the
 * open file `dirfd`.
 */
static int64_t do_fchownat(int64_t dirfd, uint64_t upath, uint64_t uid, uint64_t gid, unsigned flags)
{
    /* `flags` is Linux's `int`: the register's upper half is not part of
     * it, as Linux's entry truncates it (review: a first fix checked all 64
     * bits, which Linux does not). */
    if (flags & ~(unsigned)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))
        return -EINVAL;
    /* Only whether the path is empty: lookup_at copies it in again, and two
     * path buffers would not fit the kernel stack's frame limit. */
    char first;
    if (copy_from_user(&first, upath, 1))
        return -EFAULT;
    int rc;
    if (first == '\0') {
        if (!(flags & LX_AT_EMPTY_PATH))
            return -ENOENT;
        if ((int)dirfd == LX_AT_FDCWD) {   /* the working directory itself, as Linux */
            struct vnode *cwd = process_cwd_get();
            rc = vfs_chown(cwd, lx_owner_id(uid), lx_owner_id(gid));
            vnode_put(cwd);
            return rc;
        }
        struct file *f = file_of_manage((int)dirfd, &rc);
        if (f == NULL)
            return rc;
        rc = vfs_chown(f->vn, lx_owner_id(uid), lx_owner_id(gid));
        file_put(f);
        return rc;
    }
    struct vnode *vn;
    rc = lookup_at(dirfd, upath, (flags & LX_AT_SYMLINK_NOFOLLOW) != 0, &vn);
    if (rc)
        return rc;
    rc = vfs_chown(vn, lx_owner_id(uid), lx_owner_id(gid));
    vnode_put(vn);
    return rc;
}

static int64_t lx_fchownat(struct syscall_args *a)
{
    return do_fchownat((int64_t)a->a[0], a->a[1], a->a[2], a->a[3], (unsigned)a->a[4]);
}
#ifdef LX_chown
static int64_t lx_chown(struct syscall_args *a) { return do_fchownat(LX_AT_FDCWD, a->a[0], a->a[1], a->a[2], 0); }
#endif
#ifdef LX_lchown
static int64_t lx_lchown(struct syscall_args *a)
{
    return do_fchownat(LX_AT_FDCWD, a->a[0], a->a[1], a->a[2], LX_AT_SYMLINK_NOFOLLOW);
}
#endif

static int64_t lx_fchown(struct syscall_args *a)
{
    int err;
    struct file *f = file_of_manage((int)a->a[0], &err);
    if (f == NULL)
        return err;
    int rc = vfs_chown(f->vn, lx_owner_id(a->a[1]), lx_owner_id(a->a[2]));
    file_put(f);
    return rc;
}

/* chroot(path): process_chroot's rule (PRIV_MOUNT; the working directory
 * moves to the new root). */
static int64_t lx_chroot(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    if (rc)
        return rc;
    struct vnode *start = process_cwd_get();
    struct vnode *dir;
    rc = vfs_lookup(start, path, &dir);
    if (start)
        vnode_put(start);
    if (rc)
        return rc;
    rc = process_chroot(dir);
    vnode_put(dir);
    return rc;
}

/*
 * utimensat(dirfd, path, times[2], flags): the modification time, times[1]
 * (the access time is not kept: times[0] is checked and ignored).
 * No times, or UTIME_NOW, is "now"; UTIME_OMIT leaves it. A NULL path
 * is the descriptor's own file (futimens).
 */
static int64_t lx_utimensat(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[3];
    if (flags & ~(unsigned)LX_AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    unsigned what = VFS_SET_MTIME_NOW;
    uint64_t mtime = 0;
    if (a->a[2]) {
        struct lx_timespec ts[2];
        if (copy_from_user(ts, a->a[2], sizeof(ts)))
            return -EFAULT;
        for (int i = 0; i < 2; i++)
            if (ts[i].tv_nsec != LX_UTIME_NOW && ts[i].tv_nsec != LX_UTIME_OMIT &&
                (ts[i].tv_nsec < 0 || ts[i].tv_nsec >= 1000000000L || ts[i].tv_sec < 0))
                return -EINVAL;
        if (ts[0].tv_nsec == LX_UTIME_OMIT && ts[1].tv_nsec == LX_UTIME_OMIT)
            return 0;
        if (ts[1].tv_nsec == LX_UTIME_OMIT) {
            what = 0;   /* the change time only */
        } else if (ts[1].tv_nsec != LX_UTIME_NOW) {
            what = VFS_SET_MTIME;
            mtime = (uint64_t)ts[1].tv_sec * 1000000000ull + (uint64_t)ts[1].tv_nsec;
        }
    }
    struct vnode *vn;
    int rc;
    if (a->a[1] == 0) {
        struct file *f = file_of((int)a->a[0], 0);
        if (f == NULL)
            return -EBADF;
        vn = f->vn;
        vnode_get(vn);
        file_put(f);
    } else {
        rc = lookup_at((int64_t)a->a[0], a->a[1], (flags & LX_AT_SYMLINK_NOFOLLOW) != 0, &vn);
        if (rc)
            return rc;
    }
    rc = vfs_setattr(vn, what, 0, mtime);
    vnode_put(vn);
    return rc;
}

static int64_t lx_unlinkat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    struct vnode *start;
    rc = at_base((int64_t)a->a[0], path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    rc = (a->a[2] & LX_AT_REMOVEDIR) ? vfs_rmdir(start, path) : vfs_unlink(start, path);
    vnode_put(start);
    return rc;
}

static int64_t lx_faccessat(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[1], path);
    if (rc)
        return rc;
    struct vnode *start;
    rc = at_base((int64_t)a->a[0], path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    struct cosmo_stat st;
    rc = vfs_stat(start, path, &st);
    vnode_put(start);
    return rc;
}

static __maybe_unused int64_t lx_rename(struct syscall_args *a)
{
    char *oldp = kmalloc(2 * VFS_PATH_MAX, 0);   /* heap: see do_open */
    if (oldp == NULL)
        return -ENOMEM;
    char *newp = oldp + VFS_PATH_MAX;
    int rc = get_path(a->a[0], oldp);
    if (rc == 0)
        rc = get_path(a->a[1], newp);
    if (rc == 0) {
        struct vnode *cwd = process_cwd_get();
        rc = vfs_rename(cwd, oldp, newp);
        vnode_put(cwd);
    }
    kfree(oldp);
    return rc;
}

/*
 * linkat(olddirfd, oldpath, newdirfd, newpath, flags) (VFS-4): as Linux,
 * a final symbolic link is linked itself unless AT_SYMLINK_FOLLOW.
 * AT_EMPTY_PATH (linking a descriptor's file) needs CAP_DAC_READ_SEARCH
 * on Linux; here an empty path is -ENOENT whatever the flags say.
 */
static int64_t linkat_common(int64_t odirfd, uint64_t uold, int64_t ndirfd, uint64_t unew, unsigned flags)
{
    if (flags & ~(unsigned)(LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH))
        return -EINVAL;
    char *oldp = kmalloc(2 * VFS_PATH_MAX, 0);   /* heap: see do_open */
    if (oldp == NULL)
        return -ENOMEM;
    char *newp = oldp + VFS_PATH_MAX;
    struct vnode *ostart, *nstart = NULL;
    int rc = get_path(uold, oldp);
    if (rc == 0)
        rc = get_path(unew, newp);
    if (rc == 0)
        rc = at_base(odirfd, oldp, HANDLE_RIGHT_READ, &ostart, NULL, 0, NULL);
    if (rc == 0) {
        rc = at_base(ndirfd, newp, HANDLE_RIGHT_WRITE, &nstart, NULL, 0, NULL);
        if (rc == 0)
            rc = vfs_link(ostart, oldp, nstart, newp, (flags & LX_AT_SYMLINK_FOLLOW) != 0);
        vnode_put(ostart);
        if (nstart != NULL)
            vnode_put(nstart);
    }
    kfree(oldp);
    return rc;
}

static int64_t lx_linkat(struct syscall_args *a)
{
    return linkat_common((int64_t)a->a[0], a->a[1], (int64_t)a->a[2], a->a[3], (unsigned)a->a[4]);
}

#ifdef LX_link
static int64_t lx_link(struct syscall_args *a)
{
    return linkat_common(LX_AT_FDCWD, a->a[0], LX_AT_FDCWD, a->a[1], 0);
}
#endif

static int64_t lx_renameat(struct syscall_args *a)
{
    char *oldp = kmalloc(2 * VFS_PATH_MAX, 0);   /* heap: see do_open */
    if (oldp == NULL)
        return -ENOMEM;
    char *newp = oldp + VFS_PATH_MAX;
    struct vnode *ostart, *nstart = NULL;
    int rc = get_path(a->a[1], oldp);
    if (rc == 0)
        rc = get_path(a->a[3], newp);
    if (rc == 0)   /* both directories change */
        rc = at_base((int64_t)a->a[0], oldp, HANDLE_RIGHT_WRITE, &ostart, NULL, 0, NULL);
    if (rc == 0) {
        rc = at_base((int64_t)a->a[2], newp, HANDLE_RIGHT_WRITE, &nstart, NULL, 0, NULL);
        if (rc == 0)
            rc = vfs_rename2(ostart, oldp, nstart, newp);
        vnode_put(ostart);
        if (nstart != NULL)
            vnode_put(nstart);
    }
    kfree(oldp);
    return rc;
}

static int64_t lx_chdir(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(a->a[0], path);
    return rc ? rc : process_chdir(path);
}

/* fchdir: the directory a descriptor names becomes the working directory,
 * with the name the directory file recorded when it was opened (P31). */
static int64_t lx_fchdir(struct syscall_args *a)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], HANDLE_RIGHT_READ);
    if (obj == NULL)
        return -EBADF;
    struct file *f = file_from_kobject(obj);
    if (f == NULL) {
        kobject_put(obj);
        return -ENOTDIR;
    }
    int rc = f->vn->type == VNODE_DIR && !file_dir_base_ok(f) ? -EACCES   /* P39, as at_base */
                                                                : process_fchdir(f->vn, f->dir_path);
    file_put(f);
    return rc;
}

static int64_t lx_getcwd(struct syscall_args *a)
{
    struct process *p = process_current();
    size_t len = (size_t)a->a[1];
    char buf[VFS_PATH_MAX];
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    size_t n = strlcpy(buf, p->cwd_path_locked, sizeof(buf));
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

static int64_t dup_to(int h, int target, unsigned flags)
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
        rc = handle_install_at_flags(t, target, obj, rights, flags);
    }
    kobject_put(obj);
    return rc;
}

static __maybe_unused int64_t lx_dup2(struct syscall_args *a) { return dup_to((int)a->a[0], (int)a->a[1], 0); }
static int64_t lx_dup3(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[2];
    if ((int)a->a[0] == (int)a->a[1] || (flags & ~(unsigned)LX_O_CLOEXEC))
        return -EINVAL;
    return dup_to((int)a->a[0], (int)a->a[1], (flags & LX_O_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
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
    /* The owner rights too, as the native pipe gives: a Linux descriptor
     * can be dup'd and passed in a message (SCM_RIGHTS needs TRANSFER). */
    unsigned hf = (flags & LX_O_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0;
    h[0] = handle_install_flags(t, rd, HANDLE_RIGHT_READ | HANDLE_RIGHT_OWNER, hf);
    h[1] = h[0] < 0 ? -EMFILE : handle_install_flags(t, wr, HANDLE_RIGHT_WRITE | HANDLE_RIGHT_OWNER, hf);
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
static int64_t lx_pipe2(struct syscall_args *a) { return do_pipe(a->a[0], (unsigned)a->a[1]); }

static int64_t lx_fcntl(struct syscall_args *a)
{
    int h = (int)a->a[0];
    unsigned cmd = (unsigned)a->a[1];
    struct handle_table *t = &process_current()->handles;
    unsigned rights, hflags;
    struct kobject *obj = handle_get_flags(t, h, &rights, &hflags);
    if (obj == NULL)
        return -EBADF;
    int64_t rc;
    switch (cmd) {
    case LX_F_GETFD:
        rc = (hflags & HANDLE_FLAG_CLOEXEC) ? LX_FD_CLOEXEC : 0;
        break;
    case LX_F_SETFD:
        rc = handle_set_flags(t, h, (a->a[2] & LX_FD_CLOEXEC) ? HANDLE_FLAG_CLOEXEC : 0);
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
            int r = handle_install_at_flags(t, i, obj, rights, cmd == LX_F_DUPFD_CLOEXEC ? HANDLE_FLAG_CLOEXEC : 0);
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

/*
 * `ioctl`, for what a terminal needs, and on a socket the interface
 * requests (netioctl.cee, shell completeness S1).
 * Until the terminal-modes unit every request answered -ENOTTY, and the
 * cost was not fidelity but correctness: a libc told the console is not a
 * terminal fully buffers its output, so a hosted program's prompt does not
 * appear until something flushes it. Roadmap M4 adds what a shell's job
 * control, a pty's opener (musl's posix_openpt, grantpt, unlockpt and
 * ptsname) and an ssh server ask of a terminal.
 *
 * The native ABI has typed calls for these (docs/kernel/tty/design.md,
 * "Modes"); this is the translation layer, and it is the only place in
 * the tree where a call's argument type depends on another argument.
 * That is a property of Linux's ABI, which this personality exists to
 * imitate, not one the tree adopts.
 */
static int64_t lx_ioctl_tty(struct tty *t, struct file *f, unsigned req, uint64_t arg)
{
    /* A pty's master sees the pair from the other end: its input is the
     * slave's output and its output the slave's input. */
    unsigned idx;
    bool master = f != NULL && pty_get_index(f, &idx) == 0;
    switch (req) {
    case LX_TCGETS: {
        struct cosmo_termios k;
        tty_get_termios(t, &k);
        struct lx_termios lt;
        lx_termios_from_native(&lt, &k);
        return copy_to_user(arg, &lt, sizeof(lt)) ? -EFAULT : 0;
    }
    case LX_TCSETS:
    case LX_TCSETSW:
    case LX_TCSETSF: {
        /* SETSW and SETSF first wait for the output written so far to go
         * (a slave's: until the master has read it; the console's sinks
         * hold nothing); SETSF then discards unread input. */
        struct lx_termios lt;
        if (copy_from_user(&lt, arg, sizeof(lt)))
            return -EFAULT;
        struct cosmo_termios k;
        lx_termios_to_native(&k, &lt);
        if (req != LX_TCSETS && !master) {
            int rc = tty_drain_output(t);
            if (rc)
                return rc;
        }
        if (req == LX_TCSETSF)
            tty_flush_input(t);
        tty_set_termios(t, &k);
        return 0;
    }
    case LX_TIOCGWINSZ: {
        struct cosmo_ttysize sz;
        tty_get_size(t, &sz);
        struct lx_winsize w = { .ws_row = sz.rows, .ws_col = sz.cols };
        return copy_to_user(arg, &w, sizeof(w)) ? -EFAULT : 0;
    }
    case LX_TIOCSWINSZ: {
        struct lx_winsize w;
        if (copy_from_user(&w, arg, sizeof(w)))
            return -EFAULT;
        struct cosmo_ttysize sz = { .cols = w.ws_col, .rows = w.ws_row };
        tty_set_size(t, &sz);
        return 0;
    }
    case LX_TIOCSCTTY:
        return tty_set_controlling(t);   /* arg 1 (steal, privileged) is not offered: -EPERM as any other */
    case LX_TIOCNOTTY:
        return tty_disassociate(t);
    case LX_TIOCGPGRP: {
        pid_t pgid = 0;
        int rc = tty_get_pgrp(t, &pgid);
        if (rc)
            return rc;
        int32_t v = (int32_t)pgid;
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_TIOCSPGRP: {
        int32_t v;
        if (copy_from_user(&v, arg, sizeof(v)))
            return -EFAULT;
        if (v <= 0)
            return -EINVAL;
        return tty_set_pgrp(t, (pid_t)v);
    }
    case LX_TIOCGSID: {
        pid_t sid = 0;
        int rc = tty_get_sid(t, &sid);
        if (rc)
            return rc;
        int32_t v = (int32_t)sid;
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_TIOCGPTN: {
        unsigned n = 0;
        int rc = f != NULL ? pty_get_index(f, &n) : -ENOTTY;
        if (rc)
            return rc;
        uint32_t v = n;
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_TIOCSPTLCK: {
        int32_t v;
        if (copy_from_user(&v, arg, sizeof(v)))
            return -EFAULT;
        return f != NULL ? pty_set_lock(f, v != 0) : -ENOTTY;
    }
    case LX_TIOCGPTLCK: {
        bool locked = false;
        int rc = f != NULL ? pty_get_lock(f, &locked) : -ENOTTY;
        if (rc)
            return rc;
        int32_t v = locked ? 1 : 0;
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_FIONREAD: {
        /* What this end's read would get: a master the slave's output, a
         * terminal its queued input. */
        uint32_t v = master ? tty_out_queued(t) : tty_queued(t);
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_TIOCOUTQ: {
        /* What this end wrote that the other has not taken: a slave's
         * output not yet read by the master; a master's writes not yet read
         * from the slave; the console holds nothing. */
        uint32_t v = master ? tty_queued(t) : tty_out_queued(t);
        return copy_to_user(arg, &v, sizeof(v)) ? -EFAULT : 0;
    }
    case LX_TCFLSH: {
        if (arg != LX_TCIFLUSH && arg != LX_TCOFLUSH && arg != LX_TCIOFLUSH)
            return -EINVAL;
        bool in = arg != LX_TCOFLUSH, out = arg != LX_TCIFLUSH;
        /* "Input" and "output" are this end's: swapped on a master. */
        if (master ? out : in)
            tty_flush_input(t);
        if (master ? in : out)
            tty_flush_output(t);
        return 0;
    }
    case LX_TCSBRK:   /* tcdrain (arg 1), and a break -- no line to break: wait, as TCSETSW does,
                       * until the master has read a slave's output (the console holds none) */
        return master ? 0 : tty_drain_output(t);
    case LX_TCXONC:   /* no flow control on this side */
        return 0;
    default:
        return -ENOTTY;
    }
}

static int64_t lx_ioctl(struct syscall_args *a)
{
    unsigned req = (unsigned)a->a[1];
    struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], 0);
    if (obj == NULL)
        return -EBADF;
    struct tty *t = tty_of_open(obj);   /* the console object, an open /dev/tty, a pty's master or slave */
    struct file *f = file_from_kobject(obj);
    /* A character device with requests of its own (/dev/rtc0). */
    const struct chrdev_ops *chr = t == NULL && f != NULL ? ramfs_chr_ops_of(f->vn) : NULL;
    int64_t rc = t != NULL ? lx_ioctl_tty(t, f, req, a->a[2])
                 : chr != NULL && chr->ioctl != NULL ? chr->ioctl(f->vn, f, req, a->a[2])
                 : socket_from_kobject(obj) != NULL ? lx_ioctl_net(req, a->a[2])   /* netioctl.cee */
                                                    : -ENOTTY;
    if (t != NULL)
        tty_put(t);
    kobject_put(obj);
    return rc;
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
/* umask (roadmap M3): the process's own, inherited by fork and spawn
 * (struct process.umask); it was a constant 022 that nothing applied. */
static int64_t lx_umask(struct syscall_args *a)
{
    struct process *p = process_current();
    return (int64_t)__atomic_exchange_n(&p->umask, (uint32_t)a->a[0] & 0777u, __ATOMIC_RELAXED);
}

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

/*
 * W^X (decision 4): no page is writable and executable at once, so a
 * mapping or a protection asking for both is -EINVAL. The one program that
 * asks in earnest is a dynamic linker relocating a library with text
 * relocations (musl's mprotect(PROT_READ|PROT_WRITE|PROT_EXEC) for
 * DT_TEXTREL): its dlopen fails with EINVAL, and this line says why --
 * once per exec, so a program probing for RWX in a loop (a JIT) cannot
 * flood the console and the dmesg ring (#414's review).
 */
static int64_t lx_wx_refused(const char *call)
{
    struct linux_state *ls = lx();
    if (ls && !__atomic_exchange_n(&ls->wx_logged, true, __ATOMIC_RELAXED))
        kinfo("linux: pid %u: %s with PROT_WRITE|PROT_EXEC refused (W^X): a library with text relocations cannot "
              "be loaded here; build it -fPIC", process_current()->pid, call);
    return -EINVAL;
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
        return lx_wx_refused("mmap");
    vm_prot_t vprot = 0;
    if (nprot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (nprot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (nprot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    /*
     * A file: a FILE region over its page cache (docs/audit/next-subsystem-file-regions.md).
     * MAP_SHARED maps the file's own pages -- a write through the mapping
     * is a write to the file, seen by read() and every other mapping;
     * MAP_PRIVATE is copy-on-write. The same checks the native door makes
     * (kernel/syscall/mmap.cee, mmapFileCheck), with Linux's errnos: a
     * regular file (-ENODEV), the READ right (-EBADF), and for a shared
     * writable mapping a file opened for writing through a handle with
     * the WRITE right (-EACCES). The eager copy this used to make is gone.
     */
    struct file *f = NULL;
    /* Linux's mapping type is the low four bits: MAP_SHARED 1,
     * MAP_PRIVATE 2, MAP_SHARED_VALIDATE 3 (shared, every flag checked);
     * no type, or any other value, is -EINVAL -- anonymous mappings
     * included, which need one too. Review found this door deciding on
     * the SHARED bit alone. */
    unsigned type = flags & 0x0f;
    if (type == 0 || type > 3)
        return -EINVAL;
    bool shared = type != LX_MAP_PRIVATE;
    vm_prot_t maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC;
    if (!(flags & LX_MAP_ANONYMOUS)) {
        if (!is_page_aligned(off) || off + len < off)
            return -EINVAL;
        /* One lookup carrying the rights, as the native door: a second
         * lookup of the number could resolve to another file if a thread
         * closed and reopened it in between (review). */
        unsigned rights = 0;
        struct kobject *obj = handle_get(&p->handles, (int)a->a[4], &rights);
        if (obj == NULL)
            return -EBADF;
        f = file_from_kobject(obj);
        if (f == NULL || !(rights & HANDLE_RIGHT_READ)) {
            kobject_put(obj);
            return -EBADF;
        }
        if (f->vn->type != VNODE_REG) {
            file_put(f);
            return -ENODEV;
        }
        bool writable = (f->flags & COSMO_O_ACCMODE) != COSMO_O_RDONLY && (rights & HANDLE_RIGHT_WRITE);
        if (shared && (nprot & COSMO_PROT_WRITE) && !writable) {
            file_put(f);
            return -EACCES;
        }
        if (shared && !writable)
            maxprot &= ~VM_PROT_WRITE;
    }
    /*
     * MAP_SHARED|MAP_ANONYMOUS: Linux's shmem, an unnamed file, so the
     * pages stay shared with a fork child -- a private anonymous region
     * would be copied on write (docs/compat/linux/design.md, "fork").
     */
    struct vnode *anon_vn = NULL;
    if (shared && (flags & LX_MAP_ANONYMOUS)) {
        int arc = ramfs_anon_reg(0600, &anon_vn);
        if (arc == 0) {
            arc = (int)vfs_ftruncate(anon_vn, len);
            if (arc) {
                vnode_put(anon_vn);
                anon_vn = NULL;
            }
        }
        if (arc)
            return arc;
        off = 0;
    }
    struct vnode *vn = f ? f->vn : anon_vn;
    unsigned fflags = shared ? VM_MAP_SHARED : 0;
    uint64_t base;
    int rc;
    if (flags & LX_MAP_FIXED) {
        if (!is_page_aligned(hint) || !user_range_ok(hint, len)) {
            rc = -EINVAL;
            goto out;
        }
        base = hint;
        /*
         * Linux replaces what was there, and this used to do it as an
         * unmap followed by a map -- two holds of space->lock with the
         * range belonging to nobody in between. Another thread's
         * mmap(NULL, ...) could be handed that gap, after which the map
         * failed with -EEXIST: a failure Linux never produces. One
         * operation now, which owns the range throughout
         * (docs/audit/next-subsystem-map-fixed.md), for a file too.
         */
        rc = vn ? vm_user_map_file(p->space, base, len, vprot, maxprot, fflags | VM_MAP_REPLACE, vn, off,
                                   "mmap-file")
               : vm_user_map_anon_replace(p->space, base, len, vprot, 0, "mmap");
        goto out;
    }
    /* As the native door: each attempt chooses and inserts under one hold
     * (M46), because Linux never answers mmap(NULL, ...) with EEXIST and
     * two holds did about half the time two threads placed at once. */
    const uint64_t mbase = user_mmap_base(p->space);
    uint64_t from = (hint >= USER_LO && is_page_aligned(hint)) ? hint : mbase;
    for (;;) {
        rc = vn ? vm_user_map_file_free(p->space, from, len, vprot, maxprot, fflags, vn, off, "mmap-file", &base)
               : vm_user_map_anon_free(p->space, from, len, vprot, 0, "mmap", &base);
        if (rc != -ENOMEM || from == mbase)
            break;
        from = mbase;
    }
out:
    if (f)
        file_put(f);
    if (anon_vn)
        vnode_put(anon_vn);   /* the mapping holds its own reference */
    return rc ? rc : (int64_t)base;
}

static int64_t lx_msync(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = page_align_up((size_t)a->a[1]);
    int flags = (int)a->a[2];
    if (flags & ~(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC))
        return -EINVAL;
    if ((flags & LX_MS_ASYNC) && (flags & LX_MS_SYNC))
        return -EINVAL;
    if (!is_page_aligned(addr) || len == 0 || !user_range_ok(addr, len))
        return -EINVAL;
    if (!(flags & LX_MS_SYNC))   /* ASYNC and INVALIDATE: see sys_msync */
        return vm_user_range_mapped(process_current()->space, addr, len, 0) ? 0 : -ENOMEM;
    return vm_user_msync(process_current()->space, addr, len);
}

static int64_t lx_munmap(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = (size_t)a->a[1];
    if (!is_page_aligned(addr) || len == 0 || !user_range_ok(addr, page_align_up(len)))
        return -EINVAL;
    return vm_user_unmap(process_current()->space, addr, page_align_up(len), 0);
}

/* mremap: resize a whole anonymous mapping in place (grow or shrink).
 * See docs/audit/next-subsystem-mremap.md. */
static int64_t lx_mremap(struct syscall_args *a)
{
    uint64_t old_addr = a->a[0];
    size_t raw_old = (size_t)a->a[1], raw_new = (size_t)a->a[2];
    unsigned flags = (unsigned)a->a[3];
    if (flags & ~(unsigned)(LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED | LX_MREMAP_DONTUNMAP))
        return -EINVAL;
    if (flags & (LX_MREMAP_FIXED | LX_MREMAP_DONTUNMAP))
        return -EINVAL;   /* v1 places nothing at a chosen address and always unmaps a shrink */
    if (!is_page_aligned(old_addr))
        return -EINVAL;
    /* Bound the lengths against the user window BEFORE rounding: a size
     * near UINT64_MAX would make page_align_up wrap to 0, and a 0 old_size
     * is the shareable-dup form v1 does not support. (As lx_mmap does.) */
    if (raw_old == 0 || raw_old > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    if (raw_new == 0 || raw_new > (size_t)(USER_HI - USER_LO))
        return -EINVAL;
    size_t old_size = page_align_up(raw_old);
    size_t new_size = page_align_up(raw_new);

    /* vm_user_remap finds and validates the mapping (whole anonymous
     * region) and resizes it -- all guarding the space lock -- so even an
     * equal-size request reports -EFAULT for an unmapped address rather
     * than a bogus success. A concurrent munmap/MAP_FIXED cannot slip a
     * different mapping into the range mid-resize. MAYMOVE is accepted but
     * never relocates: a grow the space after cannot absorb is -ENOMEM
     * (realloc falls back to allocate-copy-free). */
    int rc = vm_user_remap(process_current()->space, old_addr, old_size, new_size);
    return rc ? rc : (int64_t)old_addr;
}

/* System V shared memory (shmget/shmat/shmdt/shmctl). A segment is an
 * anonymous ramfs file (kernel/ipc/shm.c) that shmat maps MAP_SHARED. See
 * docs/audit/next-subsystem-shm.md. */
static int64_t lx_shmget(struct syscall_args *a)
{
    unsigned shmflg = (unsigned)a->a[2];
    /* v1 honours only IPC_CREAT/IPC_EXCL and the mode; SHM_HUGETLB and any
     * other flag must fail, not silently create ordinary memory. */
    if (shmflg & ~(unsigned)(LX_IPC_CREAT | LX_IPC_EXCL | 0777))
        return -EINVAL;
    struct process *p = process_current();
    return shm_get((int32_t)a->a[0], (size_t)a->a[1], shmflg, &p->cred);
}

static int64_t lx_shmat(struct syscall_args *a)
{
    unsigned shmflg = (unsigned)a->a[2];
    if (shmflg & LX_SHM_REMAP)
        return -EINVAL;   /* v1 does not remap over an existing mapping */
    struct shm_segment *seg;
    int rc = shm_lookup_ref((int)a->a[0], &seg);
    if (rc)
        return rc;        /* -EINVAL: no such live segment */

    struct process *p = process_current();
    bool rdonly = (shmflg & LX_SHM_RDONLY) != 0;
    /* ipc_perm: read is always needed, write unless SHM_RDONLY. */
    rc = shm_access(seg, &p->cred, 04 | (rdonly ? 0 : 02));
    if (rc) {
        shm_unref(seg);
        return rc;        /* -EACCES */
    }

    size_t size = shm_size(seg);
    struct vnode *vn = shm_vnode(seg);
    vm_prot_t prot = VM_PROT_READ | (rdonly ? 0 : VM_PROT_WRITE);
    /* A read-only attach keeps a read-only ceiling, so a later mprotect cannot
     * add write access to the shared segment. */
    vm_prot_t maxprot = prot;
    struct vm_space *space = p->space;
    uint64_t shmaddr = a->a[1];
    uint64_t base, tag;
    if (shmaddr == 0) {
        base = user_mmap_base(space);
        rc = vm_user_map_shm(space, 0, &base, size, prot, maxprot, vn, "shm", &tag);
    } else {
        if (!is_page_aligned(shmaddr)) {   /* SHM_RND rounding is deferred */
            shm_unref(seg);
            return -EINVAL;
        }
        base = shmaddr;
        rc = vm_user_map_shm(space, base, NULL, size, prot, maxprot, vn, "shm", &tag);
        if (rc == -EEXIST)
            rc = -ENOMEM;   /* the requested address is occupied; no silent relocation */
    }
    if (rc) {
        shm_unref(seg);
        return rc;
    }

    struct lx_shm_attach *at = kzalloc(sizeof(*at));
    if (at == NULL) {
        vm_user_unmap_tag(space, tag);
        shm_unref(seg);
        return -ENOMEM;
    }
    at->addr = base;
    at->size = size;
    at->tag = tag;
    at->seg = seg;   /* the lookup's reference becomes the attach's */
    /* Count the attach BEFORE publishing the record: once it is on the list a
     * sibling thread can detach it, and that detach drops the attach's
     * reference -- so `seg` must not be touched afterwards. */
    shm_attached(seg);   /* nattch++ */
    struct linux_state *ls = lx();
    arch_irq_state_t st = spin_lock_irqsave(&ls->shm_lock);
    at->next = ls->shm_attaches;
    ls->shm_attaches = at;
    spin_unlock_irqrestore(&ls->shm_lock, st);
    return (int64_t)base;
}

static int64_t lx_shmdt(struct syscall_args *a)
{
    uint64_t shmaddr = a->a[0];
    struct linux_state *ls = lx();
    struct lx_shm_attach *at = NULL;
    arch_irq_state_t st = spin_lock_irqsave(&ls->shm_lock);
    for (struct lx_shm_attach **pp = &ls->shm_attaches; *pp != NULL; pp = &(*pp)->next) {
        if ((*pp)->addr == shmaddr) {
            at = *pp;
            *pp = at->next;
            break;
        }
    }
    spin_unlock_irqrestore(&ls->shm_lock, st);
    if (at == NULL)
        return -EINVAL;   /* no attach at that address */

    struct vm_space *space = process_current()->space;
    /* Unmap exactly this attach's mapping(s), by the tag the map handed back:
     * all its remaining pieces (after any partial self-unmap) go, while a
     * re-attach of the same segment or a replacement mapping -- which carry a
     * different tag or none -- is left alone. Atomic against a concurrent
     * map/unmap of the ranges. */
    vm_user_unmap_tag(space, at->tag);
    shm_detach(at->seg);   /* nattch-- and drop the attach's reference */
    kfree(at);
    return 0;
}

static int64_t lx_shmctl(struct syscall_args *a)
{
    struct process *p = process_current();
    int shmid = (int)a->a[0];
    switch ((int)a->a[1]) {
    case LX_IPC_RMID:
        return shm_rmid(shmid, &p->cred);
    case LX_IPC_STAT: {
        struct shm_stat st;
        int rc = shm_stat_id(shmid, &p->cred, &st);
        if (rc)
            return rc;
        struct lx_shmid_ds ds;
        memset(&ds, 0, sizeof(ds));
        ds.shm_perm.key = st.key;
        ds.shm_perm.uid = ds.shm_perm.cuid = st.cuid;
        ds.shm_perm.gid = ds.shm_perm.cgid = st.cgid;
        ds.shm_perm.mode = st.mode;
        ds.shm_segsz = st.size;
        ds.shm_nattch = st.nattch;
        return copy_to_user(a->a[2], &ds, sizeof(ds)) ? -EFAULT : 0;
    }
    default:
        return -EINVAL;   /* IPC_SET / SHM_LOCK / SHM_INFO deferred */
    }
}

static int64_t lx_mprotect(struct syscall_args *a)
{
    uint64_t addr = a->a[0];
    size_t len = page_align_up((size_t)a->a[1]);
    int nprot;
    if (!is_page_aligned(addr) || len == 0 || lx_prot((unsigned)a->a[2], &nprot) < 0)
        return -EINVAL;
    if ((nprot & COSMO_PROT_WRITE) && (nprot & COSMO_PROT_EXEC))
        return lx_wx_refused("mprotect");
    vm_prot_t vprot = 0;
    if (nprot & COSMO_PROT_READ)
        vprot |= VM_PROT_READ;
    if (nprot & COSMO_PROT_WRITE)
        vprot |= VM_PROT_WRITE;
    if (nprot & COSMO_PROT_EXEC)
        vprot |= VM_PROT_EXEC;
    if (!user_range_ok(addr, len))
        return -ENOMEM;
    int rc = vm_user_protect(process_current()->space, addr, len, vprot);
    if (rc)
        return rc;
    /* The same rule as the native door: a range made executable gets
     * its instruction stream synchronised by the kernel, because a
     * Linux JIT cannot do it from EL0 here either. */
    if (vprot & VM_PROT_EXEC)
        vm_user_sync_icache(process_current()->space, addr, len);
    return 0;
}

static int64_t lx_madvise(struct syscall_args *a) { (void)a; return 0; }

/* --- process, identity, signals (lxident.cee) ----------------------------------
 *
 * The pid and the ids, the resource limits, the credentials and the
 * supplementary groups, sessions and process groups, arch_prctl, wait4,
 * getrusage, lx_zero and lx_nosys are CeeSafe's (compat/linux/lxident.cee, roadmap 1.3
 * K1; linux_internal.h). */

/* --- the clocks, sleeping, nice values and rebooting (lxtime.cee) -------------
 *
 * clock_gettime, gettimeofday, time, times, settimeofday, clock_settime,
 * nanosleep, clock_nanosleep, sched_yield, getpriority, setpriority and
 * reboot are CeeSafe's (compat/linux/lxtime.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- mounting, statfs, sysinfo, getrandom, uname, the host name and futex ---- */

/*
 * mount(2) and umount2(2) (shell completeness S6; docs/compat/linux/
 * design.md, "Mounting"): the native mount's operation, with its flags
 * and under PRIV_MOUNT as the native calls are (syscall_mount).
 *
 * The source is a path: "/dev/NAME" names the block device NAME, as
 * /proc/mounts prints it; anything without a slash is the label a
 * filesystem with no device ignores ("none", "proc", or none at all);
 * any other path is -ENOENT, since /dev holds no block device nodes. "proc" is Linux's name
 * for procfs. MS_RDONLY is the one flag a mount takes; MS_SILENT (BusyBox
 * sets it always) and the old MS_MGC_VAL marker in the top half change
 * nothing, and every other bit -- a remount, a bind, nosuid -- is
 * -EINVAL, as an unknown native bit is. No filesystem here takes options,
 * so a non-empty data string is -EINVAL too.
 */
#define LX_MS_RDONLY 1ul
#define LX_MS_SILENT 32768ul
#define LX_MS_MGC_VAL 0xc0ed0000ul
#define LX_MS_MGC_MSK 0xffff0000ul
#define LX_MNT_FORCE 1u

static int64_t lx_mount(struct syscall_args *a)
{
    if (!cred_has(cred_current(), PRIV_MOUNT))
        return -EPERM;
    unsigned long flags = (unsigned long)a->a[3];
    if ((flags & LX_MS_MGC_MSK) == LX_MS_MGC_VAL)
        flags &= ~LX_MS_MGC_MSK;
    if (flags & ~(LX_MS_RDONLY | LX_MS_SILENT))
        return -EINVAL;
    /* A source longer than any device path or label here is refused
     * rather than given a second VFS_PATH_MAX on this stack. */
    char source[64] = "", target[VFS_PATH_MAX], fstype[16];
    int rc = a->a[0] != 0 ? strncpy_from_user(source, a->a[0], sizeof(source)) : 0;
    if (rc < 0)
        return rc;
    rc = get_path(a->a[1], target);
    if (rc)
        return rc;
    rc = strncpy_from_user(fstype, a->a[2], sizeof(fstype));
    if (rc < 0)
        return rc;
    if (a->a[4] != 0) {
        char first;   /* only whether the string is empty */
        if (copy_from_user(&first, a->a[4], 1))
            return -EFAULT;
        if (first != '\0')
            return -EINVAL;
    }
    const char *device = NULL;
    if (strncmp(source, "/dev/", 5) == 0 && source[5] != '\0' && strchr(source + 5, '/') == NULL) {
        device = source + 5;
        struct blkdev *bd = blk_find(device);
        if (bd == NULL)
            return -ENOENT;   /* no such path, as Linux says; -ENODEV is for an unknown type */
        blkdev_put(bd);
    } else if (strchr(source, '/') != NULL) {
        return -ENOENT;
    }
    return syscall_mount(device, target, strcmp(fstype, "proc") == 0 ? "procfs" : fstype,
                         (flags & LX_MS_RDONLY) ? MOUNT_RDONLY : 0);
}

/* MNT_FORCE is the native force; MNT_DETACH, MNT_EXPIRE and
 * UMOUNT_NOFOLLOW are -EINVAL. */
static int64_t lx_umount2(struct syscall_args *a)
{
    if (!cred_has(cred_current(), PRIV_MOUNT))
        return -EPERM;
    unsigned flags = (unsigned)a->a[1];
    if (flags & ~LX_MNT_FORCE)
        return -EINVAL;
    char target[VFS_PATH_MAX];
    int rc = get_path(a->a[0], target);
    if (rc)
        return rc;
    return syscall_umount(target, (flags & LX_MNT_FORCE) ? VFS_UMOUNT_FORCE : 0);
}

/* statfs(2) and fstatfs(2) over vnode_statfs (df). The fsid is the
 * mount's id, which names one mount for as long as it is mounted. */
static int put_statfs(uint64_t ubuf, struct vnode *vn)
{
    struct vfs_statfs sf;
    int rc = vnode_statfs(vn, &sf);
    if (rc)
        return rc;
    struct lx_statfs st;
    memset(&st, 0, sizeof(st));
    st.f_type = sf.magic;
    st.f_bsize = st.f_frsize = sf.bsize;
    st.f_blocks = sf.blocks;
    st.f_bfree = sf.bfree;
    st.f_bavail = sf.bavail;
    st.f_files = sf.files;
    st.f_ffree = sf.ffree;
    st.f_fsid[0] = (int32_t)vn->mnt->id;
    st.f_fsid[1] = (int32_t)(vn->mnt->id >> 32);
    st.f_namelen = sf.namelen;
    st.f_flags = (sf.flags & MOUNT_RDONLY) ? LX_ST_RDONLY : 0;
    return copy_to_user(ubuf, &st, sizeof(st)) ? -EFAULT : 0;
}

static int64_t lx_statfs(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = strncpy_from_user(path, a->a[0], VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    if (path[0] == '\0')
        return -ENOENT;
    struct vnode *start, *vn;
    rc = at_base(LX_AT_FDCWD, path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
    if (rc)
        return rc;
    rc = vfs_lookup(start, path, &vn);
    vnode_put(start);
    if (rc)
        return rc;
    rc = put_statfs(a->a[1], vn);
    vnode_put(vn);
    return rc;
}

static int64_t lx_fstatfs(struct syscall_args *a)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, (int)a->a[0], 0);
    if (obj == NULL)
        return -EBADF;
    struct file *f = file_from_kobject(obj);
    /* A pipe, socket or other object is on no filesystem here (Linux
     * answers with pipefs's or sockfs's figures). */
    int rc = f != NULL && f->vn != NULL ? put_statfs(a->a[1], f->vn) : -EINVAL;
    kobject_put(obj);
    return rc;
}

/* sysinfo(2): uptime and a coarse memory/process snapshot, marshalled from the
 * stats the kernel already keeps. See docs/audit/next-subsystem-sysinfo.md. */
static int64_t lx_sysinfo(struct syscall_args *a)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    struct lx_sysinfo si;
    memset(&si, 0, sizeof(si));   /* swap, shared/buffer, high, pad: none here, so zero */
    si.uptime = (int64_t)(clock_now_ns() / 1000000000ull);
    unsigned long loads[3];
    sched_loadavg(loads, 16);   /* Linux's SI_LOAD_SHIFT */
    for (unsigned i = 0; i < 3; i++)
        si.loads[i] = loads[i];
    si.mem_unit = 1;              /* report bytes */
    si.totalram = st.total_pages * PAGE_SIZE;
    si.freeram = st.free_pages * PAGE_SIZE;
    si.procs = (uint16_t)process_count();
    return copy_to_user(a->a[0], &si, sizeof(si)) ? -EFAULT : 0;
}

/*
 * Linux's semantics (docs/kernel/security/design.md §6): block until the
 * pool is seeded, unless GRND_NONBLOCK (-EAGAIN) or GRND_INSECURE (never
 * waits). GRND_RANDOM is the default. The wait comes before the length
 * is looked at, as in Linux.
 */
static int64_t lx_getrandom(struct syscall_args *a)
{
    unsigned flags = (unsigned)a->a[2];
    if ((flags & ~(unsigned)(LX_GRND_NONBLOCK | LX_GRND_RANDOM | LX_GRND_INSECURE)) != 0 ||
        (flags & (LX_GRND_INSECURE | LX_GRND_RANDOM)) == (LX_GRND_INSECURE | LX_GRND_RANDOM))
        return -EINVAL;
    if (!(flags & LX_GRND_INSECURE) && !random_ready()) {
        if (flags & LX_GRND_NONBLOCK)
            return -EAGAIN;
        int rc = random_wait_ready(RANDOM_WAIT_FOREVER);
        if (rc)
            return rc;
    }
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
    /* From the caller's uts namespace: a contained process asking the
     * machine its name must be told its container's
     * (docs/kernel/security/design.md §1e). */
    utsns_gethostname(utsns_current(), u.nodename, sizeof(u.nodename));
    strlcpy(u.release, KERNEL_LINUX_RELEASE, sizeof(u.release));
    ksnprintf(u.version, sizeof(u.version), "%s %s %s", KERNEL_NAME, KERNEL_VERSION, COSMO_BUILD_ID);
    strlcpy(u.machine, LX_MACHINE, sizeof(u.machine));
    strlcpy(u.domainname, "(none)", sizeof(u.domainname));
    return copy_to_user(a->a[0], &u, sizeof(u)) ? -EFAULT : 0;
}

/* sethostname (shell completeness S1): the caller's uts namespace's name,
 * as the native call sets it and under its rule (PRIV_SYS_ADMIN: a name is
 * not a secret, but who can rename the machine can make another's logs say
 * what it likes). Linux has no gethostname call on these architectures:
 * musl reads the name from uname's nodename. Linux takes a name of up to
 * 64 bytes (__NEW_UTS_LEN); this kernel keeps 63 and a NUL. */
static int64_t lx_sethostname(struct syscall_args *a)
{
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
        return -EPERM;
    size_t len = (size_t)a->a[1];
    if (len >= COSMO_HOST_NAME_MAX)
        return -EINVAL;
    char buf[COSMO_HOST_NAME_MAX];
    if (copy_from_user(buf, a->a[0], len))
        return -EFAULT;
    return utsns_sethostname(utsns_current(), buf, len);
}

static int64_t lx_futex(struct syscall_args *a)
{
    uint64_t uaddr = a->a[0];
    unsigned op = (unsigned)a->a[1] & (unsigned)LX_FUTEX_CMD_MASK;
    bool realtime = ((unsigned)a->a[1] & LX_FUTEX_CLOCK_REALTIME) != 0;
    /* The flag is the program's promise that nobody outside this
     * process can see the word: the private key, no lookup. Without it
     * the word is classified by what it maps, so a process-shared mutex
     * in a MAP_SHARED page is shared (the shared-futex unit; the flag
     * used to be masked out and every futex was private). */
    bool private = ((unsigned)a->a[1] & LX_FUTEX_PRIVATE_FLAG) != 0;
    uint32_t val = (uint32_t)a->a[2];
    struct vm_space *space = process_current()->space;
    if (!user_range_ok(uaddr, 4))
        return -EFAULT;
    /* Once, for every operation: the past-deadline WAIT_BITSET path below
     * reads the word itself and used to answer -EAGAIN/-ETIMEDOUT for a
     * misaligned address the futex would have refused (review). */
    if (uaddr & 3)
        return -EINVAL;
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
        return futex_wait(space, uaddr, val, timeout, private);
    }
    case LX_FUTEX_WAKE:
    case LX_FUTEX_WAKE_BITSET:
        if (op == LX_FUTEX_WAKE_BITSET && (uint32_t)a->a[5] != LX_FUTEX_BITSET_MATCH_ANY)
            return -ENOSYS;
        return futex_wake(space, uaddr, val, private);
    case LX_FUTEX_REQUEUE:
    case LX_FUTEX_CMP_REQUEUE: {
        uint64_t uaddr2 = a->a[4];
        if (!user_range_ok(uaddr2, 4))
            return -EFAULT;
        unsigned nr_requeue = (unsigned)a->a[3];
        /* Both report woken + requeued, as the Linux kernel does for both
         * operations (its man page's "woken" for REQUEUE notwithstanding). */
        return futex_requeue(space, uaddr, uaddr2, val, nr_requeue, op == LX_FUTEX_CMP_REQUEUE, (uint32_t)a->a[5],
                             private);
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

/*
 * select and pselect6 (the device-readiness unit): three fd_sets of `nfds`
 * bits, one io_pollfd per fd that has a read or write bit, io_poll, and
 * the sets rewritten with what came back. Linux's own sets are: readable
 * = POLLIN|POLLHUP|POLLERR, writable = POLLOUT|POLLERR, exceptional =
 * POLLPRI -- and no object in this tree reports a priority event (there
 * is no urgent-data path; COSMO_IO_ERROR is POLLERR, which select never
 * puts in the except set), so the except set is polled for nothing and
 * always comes back clear. A bit for a closed fd is -EBADF, as Linux
 * answers; nfds above LX_FD_SETSIZE is -EINVAL. The result is the number
 * of bits set across the three sets.
 */
#define LX_FD_WORDS (LX_FD_SETSIZE / 64)

struct lx_fdset_io {
    uint64_t bits[LX_FD_WORDS];
    uint64_t uptr;      /* the user's set, or 0 */
};

static int fdset_in(struct lx_fdset_io *s, uint64_t uptr, unsigned words)
{
    memset(s->bits, 0, sizeof(s->bits));
    s->uptr = uptr;
    if (uptr == 0 || words == 0)
        return 0;
    return copy_from_user(s->bits, uptr, words * sizeof(uint64_t)) ? -EFAULT : 0;
}

static bool fdset_test(const struct lx_fdset_io *s, unsigned fd)
{
    return (s->bits[fd / 64] >> (fd % 64)) & 1u;
}

static int64_t do_select(int nfds, uint64_t urd, uint64_t uwr, uint64_t uex, uint64_t timeout_ns)
{
    if (nfds < 0 || nfds > LX_FD_SETSIZE)
        return -EINVAL;
    unsigned words = ((unsigned)nfds + 63) / 64;
    struct lx_fdset_io rd, wr, ex;
    int rc = fdset_in(&rd, urd, words);
    if (rc == 0)
        rc = fdset_in(&wr, uwr, words);
    if (rc == 0)
        rc = fdset_in(&ex, uex, words);
    if (rc)
        return rc;
    /* One entry per fd with a read or write bit. An fd in the except set
     * alone is checked for existence and not polled: with no event of
     * interest, io_poll would still end the wait on its HANGUP or ERROR
     * and select would report 0 bits at once -- a timeout that never
     * waited. */
    unsigned n = 0;
    for (int fd = 0; fd < nfds; fd++)
        if (fdset_test(&rd, (unsigned)fd) || fdset_test(&wr, (unsigned)fd))
            n++;
    struct io_pollfd *fds = NULL;
    int *fdnum = NULL;
    if (n) {
        fds = kmalloc(n * sizeof(*fds), KMEM_ZERO);
        fdnum = kmalloc(n * sizeof(*fdnum), 0);
        if (fds == NULL || fdnum == NULL) {
            kfree(fds);
            kfree(fdnum);
            return -ENOMEM;
        }
    }
    struct process *p = process_current();
    unsigned k = 0;
    int64_t result = 0;
    for (int fd = 0; fd < nfds; fd++) {
        bool r = fdset_test(&rd, (unsigned)fd), w = fdset_test(&wr, (unsigned)fd), x = fdset_test(&ex, (unsigned)fd);
        if (!r && !w && !x)
            continue;
        unsigned rights;
        struct kobject *obj = handle_get(&p->handles, fd, &rights);
        if (obj == NULL || kobject_io_of(obj) == NULL) {
            if (obj)
                kobject_put(obj);
            result = -EBADF;   /* Linux: a bit for a closed fd */
            break;
        }
        if (!r && !w) {
            kobject_put(obj);   /* except only: exists, and that is all select can say of it */
            continue;
        }
        fds[k].obj = obj;
        fds[k].events = (r ? COSMO_IO_READABLE : 0) | (w ? COSMO_IO_WRITABLE : 0);
        fdnum[k] = fd;
        k++;
    }
    if (result == 0)
        result = io_poll(fds, k, timeout_ns);
    /* The sets, rewritten: Linux's readable set is POLLIN|POLLHUP|POLLERR,
     * its writable set POLLOUT|POLLERR; the except set is always clear. */
    if (result >= 0) {
        memset(rd.bits, 0, sizeof(rd.bits));
        memset(wr.bits, 0, sizeof(wr.bits));
        memset(ex.bits, 0, sizeof(ex.bits));
        result = 0;
        for (unsigned i = 0; i < k; i++) {
            unsigned ev = fds[i].revents;
            unsigned fd = (unsigned)fdnum[i];
            if ((fds[i].events & COSMO_IO_READABLE) && (ev & (COSMO_IO_READABLE | COSMO_IO_HANGUP | COSMO_IO_ERROR))) {
                rd.bits[fd / 64] |= 1ull << (fd % 64);
                result++;
            }
            if ((fds[i].events & COSMO_IO_WRITABLE) && (ev & (COSMO_IO_WRITABLE | COSMO_IO_ERROR))) {
                wr.bits[fd / 64] |= 1ull << (fd % 64);
                result++;
            }
        }
    }
    for (unsigned i = 0; i < k; i++)
        kobject_put(fds[i].obj);
    kfree(fds);
    kfree(fdnum);
    if (result >= 0 && words) {
        if ((rd.uptr && copy_to_user(rd.uptr, rd.bits, words * sizeof(uint64_t))) ||
            (wr.uptr && copy_to_user(wr.uptr, wr.bits, words * sizeof(uint64_t))) ||
            (ex.uptr && copy_to_user(ex.uptr, ex.bits, words * sizeof(uint64_t))))
            result = -EFAULT;
    }
    return result;
}

/* pselect6's sixth argument: Linux's pair { const sigset_t *ss; size_t ss_len }. */
struct lx_sigset_arg {
    uint64_t ss;
    uint64_t ss_len;
};

static int64_t lx_pselect6(struct syscall_args *a)
{
    uint64_t timeout_ns = IO_POLL_FOREVER;
    if (a->a[4]) {
        int rc = ns_from_timespec(a->a[4], &timeout_ns);
        if (rc)
            return rc;
    }
    uint64_t mask = 0, old = 0;
    bool swap = false;
    if (a->a[5]) {
        struct lx_sigset_arg sa;
        if (copy_from_user(&sa, a->a[5], sizeof(sa)))
            return -EFAULT;
        if (sa.ss) {
            if (sa.ss_len != 8)
                return -EINVAL;
            if (copy_from_user(&mask, sa.ss, 8))
                return -EFAULT;
            swap = true;
        }
    }
    if (swap) {
        old = signal_blocked();
        signal_set_blocked(mask);
    }
    int64_t rc = do_select((int)a->a[0], a->a[1], a->a[2], a->a[3], timeout_ns);
    if (swap)
        signal_set_blocked_saved(old);
    return rc;
}

/* The legacy select (x86-64 only): a timeval, which Linux writes the time
 * left back into and this door leaves as given (documented deviation). */
static __maybe_unused int64_t lx_select(struct syscall_args *a)
{
    uint64_t timeout_ns = IO_POLL_FOREVER;
    if (a->a[4]) {
        struct lx_timeval tv;
        if (copy_from_user(&tv, a->a[4], sizeof(tv)))
            return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
            return -EINVAL;
        /* Clamped as ns_from_timespec clamps: beyond 2^62 ns is "never", and
         * the multiplication below must not wrap a huge wait into a short one. */
        if ((uint64_t)tv.tv_sec >= (1ull << 62) / 1000000000ull)
            timeout_ns = 1ull << 62;
        else
            timeout_ns = (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
    }
    return do_select((int)a->a[0], a->a[1], a->a[2], a->a[3], timeout_ns);
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

/*
 * fork, vfork, and clone without CLONE_THREAD (docs/compat/linux/design.md,
 * "fork"): a new process through process_fork. CLONE_VM is accepted only
 * with CLONE_VFORK -- the child borrows the space and the caller waits
 * until it execs or exits; CLONE_VFORK alone is a fork whose caller
 * waits the same way. The exit signal must be SIGCHLD.
 */
static int64_t lx_fork_common(struct syscall_args *a, uint64_t flags, unsigned exit_sig, uint64_t newsp, uint64_t ptid,
                              uint64_t ctid, uint64_t tls)
{
    if (flags & ~LX_CLONE_FORK_ALLOWED)
        return -EINVAL;
    if ((flags & LX_CLONE_VM) && !(flags & LX_CLONE_VFORK))
        return -EINVAL;   /* a second process on one space, running alongside: not built */
    if (exit_sig != LX_CLONE_EXIT_SIGCHLD)
        return -EINVAL;
    if ((flags & LX_CLONE_PARENT_SETTID) && !user_range_ok(ptid, 4))
        return -EFAULT;
    if ((flags & (LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID)) && !user_range_ok(ctid, 4))
        return -EFAULT;
    struct arch_user_regs regs;
    arch_user_regs_from_syscall(a->frame, &regs);
    arch_user_regs_set_result(&regs, 0);
    if (newsp)
        arch_user_regs_set_sp(&regs, (uintptr_t)newsp);
    struct process_fork_args fa = {
        .regs = &regs,
        .tls = (flags & LX_CLONE_SETTLS) ? (uintptr_t)tls : arch_get_tls_base(),
        .set_child_tid = (flags & LX_CLONE_CHILD_SETTID) ? ctid : 0,
        .clear_child_tid = (flags & LX_CLONE_CHILD_CLEARTID) ? ctid : 0,
        .flags = (flags & LX_CLONE_VM) ? PROCESS_FORK_SHARE_SPACE : 0,
    };
    struct process *child;
    int rc = process_fork(&fa, &child);
    if (rc)
        return rc;
    uint32_t pid = child->pid;
    if (flags & LX_CLONE_PARENT_SETTID)
        (void)copy_to_user(ptid, &pid, sizeof(pid));   /* Linux ignores a fault here too */
    if (flags & LX_CLONE_VFORK)
        (void)process_vfork_wait(child);   /* -EINTR: this caller is being killed and leaves at its return */
    process_put(child);
    return pid;
}

/* execve (docs/compat/linux/design.md, "execve"): the image replaced in
 * place, through the same operation as the native SYS_exec. */
static int64_t lx_execve(struct syscall_args *a)
{
    struct exec_args *ea = exec_args_alloc();
    if (ea == NULL)
        return -ENOMEM;
    int rc = exec_args_copy(ea, a->a[0], a->a[1], a->a[2]);
    if (rc == 0)
        rc = process_execve(ea, a->frame);
    exec_args_free(ea);
    return rc;
}

#ifdef LX_fork
static int64_t lx_fork(struct syscall_args *a) { return lx_fork_common(a, 0, LX_CLONE_EXIT_SIGCHLD, 0, 0, 0, 0); }
#endif
#ifdef LX_vfork
static int64_t lx_vfork(struct syscall_args *a)
{
    return lx_fork_common(a, LX_CLONE_VM | LX_CLONE_VFORK, LX_CLONE_EXIT_SIGCHLD, 0, 0, 0, 0);
}
#endif

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
        return lx_fork_common(a, flags, (unsigned)(a->a[0] & 0xff), newsp, ptid, ctid, tls);
    if ((flags & LX_CLONE_THREAD_REQUIRED) != LX_CLONE_THREAD_REQUIRED || (flags & ~LX_CLONE_THREAD_ALLOWED))
        return -EINVAL;   /* Linux: CLONE_THREAD needs CLONE_SIGHAND, which needs CLONE_VM */
    if ((flags & LX_CLONE_PARENT_SETTID) && !user_range_ok(ptid, 4))
        return -EFAULT;
    if ((flags & (LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID)) && !user_range_ok(ctid, 4))
        return -EFAULT;
    struct process *p = process_current();

    /* The child is the caller at this instant: the same registers, the
     * result 0, its own stack and thread pointer. */
    struct arch_user_regs regs;
    arch_user_regs_from_syscall(a->frame, &regs);
    arch_user_regs_set_result(&regs, 0);
    if (newsp)
        arch_user_regs_set_sp(&regs, (uintptr_t)newsp);
    uintptr_t tls_base = (flags & LX_CLONE_SETTLS) ? (uintptr_t)tls : arch_get_tls_base();

    struct thread *t;
    int rc = process_add_thread(p, &regs, tls_base, &t);
    if (rc)
        return rc;
    uint32_t tid = t->user_tid;
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

/* --- sockets (lxsock.cee) ----------------------------------------------------
 *
 * lx_socket, lx_bind, lx_connect, lx_listen, lx_accept, lx_sendto,
 * lx_recvfrom, lx_shutdown, lx_getsockname, lx_getpeername, lx_setsockopt,
 * lx_getsockopt, lx_sendmsg, lx_recvmsg and lx_socketpair are CeeSafe's
 * (compat/linux/lxsock.cee, roadmap 2.0 SA8; linux_internal.h). */

/* prctl: PR_SET_DUMPABLE and PR_GET_DUMPABLE (OpenSSH's sftp-server makes
 * itself undumpable and refuses to run when it cannot). The flag is kept on
 * the process and reported back; nothing reads it, there being no core
 * dumps or ptrace. Linux's answer for an option it does not know is
 * -EINVAL, and so is this one's for every other option. */
static int64_t lx_prctl(struct syscall_args *a)
{
    struct process *p = process_current();
    switch ((int)a->a[0]) {
    case LX_PR_GET_DUMPABLE:
        return __atomic_load_n(&p->dumpable, __ATOMIC_RELAXED);
    case LX_PR_SET_DUMPABLE:
        if (a->a[1] > 1)
            return -EINVAL;   /* 2 (SUID_DUMP_ROOT) is not settable on Linux either */
        __atomic_store_n(&p->dumpable, (uint8_t)a->a[1], __ATOMIC_RELAXED);
        return 0;
    default:
        return -EINVAL;
    }
}

/* --- interval timers (lxtime.cee) ------------------------------------------------
 *
 * setitimer, getitimer and alarm are CeeSafe's (compat/linux/lxtime.cee). */

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
    [LX_lstat] = lx_lstat,
#endif
    [LX_lseek] = lx_lseek,
    [LX_mmap] = lx_mmap,
    [LX_mprotect] = lx_mprotect,
    [LX_munmap] = lx_munmap,
    [LX_mremap] = lx_mremap,
    [LX_msync] = lx_msync,
    [LX_shmget] = lx_shmget,
    [LX_shmat] = lx_shmat,
    [LX_shmdt] = lx_shmdt,
    [LX_shmctl] = lx_shmctl,
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
    [LX_pselect6] = lx_pselect6,
#ifdef LX_select
    [LX_select] = lx_select,
#endif
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
    [LX_sendmsg] = lx_sendmsg,
    [LX_recvmsg] = lx_recvmsg,
    [LX_socketpair] = lx_socketpair,
    [LX_clone] = lx_clone,
#ifdef LX_fork
    [LX_fork] = lx_fork,
#endif
#ifdef LX_vfork
    [LX_vfork] = lx_vfork,
#endif
    [LX_execve] = lx_execve,
    [LX_exit] = lx_exit,
    [LX_wait4] = lx_wait4,
    [LX_kill] = lx_kill,
    [LX_uname] = lx_uname,
    [LX_sethostname] = lx_sethostname,
    [LX_fcntl] = lx_fcntl,
    [LX_fsync] = lx_fsync,
    [LX_fdatasync] = lx_fsync,
    [LX_getcwd] = lx_getcwd,
    [LX_chdir] = lx_chdir,
    [LX_fchdir] = lx_fchdir,
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
#ifdef LX_readlink
    [LX_readlink] = lx_readlink,
#endif
#endif
    [LX_umask] = lx_umask,
    [LX_gettimeofday] = lx_gettimeofday,
    [LX_getrlimit] = lx_getrlimit,
    [LX_sysinfo] = lx_sysinfo,
    [LX_statfs] = lx_statfs,
    [LX_setitimer] = lx_setitimer,
    [LX_prctl] = lx_prctl,
    [LX_getitimer] = lx_getitimer,
    [LX_getrusage] = lx_getrusage,
    [LX_times] = lx_times,
    [LX_getpriority] = lx_getpriority,
    [LX_setpriority] = lx_setpriority,
#ifdef LX_alarm
    [LX_alarm] = lx_alarm,
#endif
    [LX_fstatfs] = lx_fstatfs,
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
    [LX_setpgid] = lx_setpgid,
    [LX_getpgid] = lx_getpgid,
    [LX_getsid] = lx_getsid,
    [LX_getppid] = lx_getppid,
#ifdef LX_getpgrp
    [LX_getpgrp] = lx_getpgrp,
#endif
    [LX_setsid] = lx_setsid,
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
    [LX_mknodat] = lx_mknodat,
#ifdef LX_mknod
    [LX_mknod] = lx_mknod,
#endif
    [LX_newfstatat] = lx_newfstatat,
    [LX_statx] = lx_statx,
    [LX_eventfd2] = lx_eventfd2,
#ifdef LX_eventfd
    [LX_eventfd] = lx_eventfd,   /* x86-64 only: the older number, no flags */
#endif
    [LX_signalfd4] = lx_signalfd4,
#ifdef LX_signalfd
    [LX_signalfd] = lx_signalfd,   /* x86-64 only: the older number, no flags */
#endif
    [LX_timerfd_create] = lx_timerfd_create,
    [LX_timerfd_settime] = lx_timerfd_settime,
    [LX_timerfd_gettime] = lx_timerfd_gettime,
    [LX_memfd_create] = lx_memfd_create,
    [LX_ftruncate] = lx_ftruncate,
    [LX_epoll_create1] = lx_epoll_create1,
    [LX_epoll_ctl] = lx_epoll_ctl,
    [LX_epoll_pwait] = lx_epoll_pwait,
#ifdef LX_epoll_create
    [LX_epoll_create] = lx_epoll_create,   /* x86-64: the legacy create and wait */
    [LX_epoll_wait] = lx_epoll_wait,
#endif
    [LX_unlinkat] = lx_unlinkat,
#ifdef LX_chown
    [LX_chown] = lx_chown,
#endif
#ifdef LX_lchown
    [LX_lchown] = lx_lchown,
#endif
    [LX_fchown] = lx_fchown,
    [LX_fchownat] = lx_fchownat,
    [LX_chroot] = lx_chroot,
    [LX_settimeofday] = lx_settimeofday,
    [LX_clock_settime] = lx_clock_settime,
    [LX_reboot] = lx_reboot,
    [LX_mount] = lx_mount,
    [LX_umount2] = lx_umount2,
#ifdef LX_chmod
    [LX_chmod] = lx_chmod,
#endif
    [LX_fchmod] = lx_fchmod,
    [LX_fchmodat] = lx_fchmodat,
    [LX_fchmodat2] = lx_fchmodat2,
    [LX_utimensat] = lx_utimensat,
    [LX_renameat] = lx_renameat,
    [LX_linkat] = lx_linkat,
#ifdef LX_link
    [LX_link] = lx_link,
#endif
    [LX_readlinkat] = lx_readlinkat,
#ifdef LX_symlink
    [LX_symlink] = lx_symlink,
#endif
    [LX_symlinkat] = lx_symlinkat,
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
/* As for the native personality, plus rt_sigreturn: a signal handler
 * must be able to return, or the first signal after a filter is
 * installed is fatal for a reason that has nothing to do with the
 * filter. */
static const uint16_t linux_always_allowed[] = { LX_exit, LX_exit_group, LX_rt_sigreturn };

/* Every ELF without the CosmoOS note: the fallback, asked after native. */
static bool linux_claims_elf(const struct elf_info *info)
{
    (void)info;
    return true;
}

#if defined(ARCH_X86_64)
#define LINUX_PLATFORM "x86_64"
#else
#define LINUX_PLATFORM "aarch64"
#endif

const struct personality personality_linux = {
    .name = "linux",
    .claims_elf = linux_claims_elf,
    .init = linux_process_init,
    .release = linux_process_release,
    .fork = linux_process_fork,
    .exec = linux_process_exec,
    .platform = LINUX_PLATFORM,   /* AT_PLATFORM: the string Linux gives on this machine */
    .auxv = linux_auxv,
    .table = g_table,
    .count = LX_NR_MAX,
    .always_allowed = linux_always_allowed,
    .nr_always_allowed = sizeof(linux_always_allowed) / sizeof(linux_always_allowed[0]),
    .signal_frame = linux_signal_frame,
};
