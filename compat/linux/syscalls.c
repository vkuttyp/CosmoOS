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

#define IOV_MAX 1024
#define SOCK_CHUNK 4096

/* struct linux_state and struct lx_shm_attach are in linux_internal.h. */

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

/* A path from user memory (lx_get_path), the directory an *at call
 * resolves from (lx_at_base) and the file a descriptor names (lx_file_of,
 * lx_file_of_manage) are CeeSafe's (compat/linux/lxbase.cee, roadmap 1.3
 * K1; linux_internal.h). */

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
    struct file *f = lx_file_of((int)a->a[0], write ? HANDLE_RIGHT_WRITE : HANDLE_RIGHT_READ);
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
    int64_t rc = lx_get_path(upath, path);
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
    rc = lx_at_base(dirfd, path, need, &start, base, VFS_PATH_MAX, &have);
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
    struct file *f = lx_file_of((int)a->a[0], 0);
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
    int rc = lx_get_path(a->a[0], path);
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
    int rc = lx_get_path(a->a[0], path);
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
    int rc = lx_get_path(upath, path);
    if (rc)
        return rc;
    if (len == 0)
        return -EINVAL;
    if (len > VFS_PATH_MAX)
        len = VFS_PATH_MAX;
    struct vnode *start;
    rc = lx_at_base(dirfd, path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
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
        rc = lx_get_path(upath, path);
    struct vnode *start;
    if (rc >= 0)
        rc = lx_at_base(dirfd, path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
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
    rc = lx_at_base((int64_t)a->a[0], path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
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
        rc = lx_at_base((int64_t)a->a[0], path, HANDLE_RIGHT_READ, &start, NULL, 0, NULL);
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
    int rc = lx_ns_from_timespec(unew + offsetof(struct lx_itimerspec, it_value), &value_ns);
    if (!rc)
        rc = lx_ns_from_timespec(unew + offsetof(struct lx_itimerspec, it_interval), &interval_ns);
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
        if (lx_put_timespec(uold + offsetof(struct lx_itimerspec, it_value), old_rem) ||
            lx_put_timespec(uold + offsetof(struct lx_itimerspec, it_interval), old_int))
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
    if (lx_put_timespec(ucurr + offsetof(struct lx_itimerspec, it_value), rem) ||
        lx_put_timespec(ucurr + offsetof(struct lx_itimerspec, it_interval), intv))
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
    struct file *f = lx_file_of((int)a->a[0], HANDLE_RIGHT_WRITE);   /* must be open for writing */
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
    struct file *f = lx_file_of((int)a->a[0], HANDLE_RIGHT_READ);
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
    int rc = lx_get_path(upath, path);
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
    int rc = lx_get_path(a->a[1], path);
    if (rc)
        return rc;
    struct vnode *start;
    rc = lx_at_base((int64_t)a->a[0], path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
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
    rc = lx_at_base(dirfd, path, HANDLE_RIGHT_WRITE, &start, NULL, 0, NULL);
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
    int rc = lx_get_path(a->a[1], path);
    if (rc)
        return rc;
    return do_mknod((int64_t)a->a[0], path, (uint32_t)a->a[2]);
}

/* The legacy mknod (x86-64 only; AArch64 has the *at form alone), as
 * mkdir beside mkdirat. */
static __maybe_unused int64_t lx_mknod(struct syscall_args *a)
{
    char path[VFS_PATH_MAX];
    int rc = lx_get_path(a->a[0], path);
    if (rc)
        return rc;
    return do_mknod(LX_AT_FDCWD, path, (uint32_t)a->a[1]);
}

/* --- modes, owners, times and the root (lxattr.cee) -----------------------------
 *
 * chmod, fchmodat, fchmodat2, fchmod, chown, lchown, fchownat, fchown,
 * chroot and utimensat are CeeSafe's (compat/linux/lxattr.cee, roadmap 1.3
 * K1; linux_internal.h). */

/* --- names and descriptors (lxname.cee) --------------------------------------
 *
 * unlinkat, faccessat, rename, renameat, link, linkat, chdir, fchdir,
 * getcwd, dup, dup2, dup3, pipe, pipe2, fcntl, sync, fsync and umask are
 * CeeSafe's (compat/linux/lxname.cee, roadmap 1.3 K1; linux_internal.h). */

/* --- the terminal ------------------------------------------------------------ */

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

/* --- memory (lxmem.cee) ------------------------------------------------------
 *
 * brk, mmap, msync, munmap, mremap, mprotect, madvise and System V shared
 * memory are CeeSafe's (compat/linux/lxmem.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- process, identity, signals (lxident.cee) ----------------------------------
 *
 * The pid and the ids, the resource limits, the credentials and the
 * supplementary groups, sessions and process groups, arch_prctl, wait4,
 * getrusage, lx_zero and lx_nosys are CeeSafe's (compat/linux/lxident.cee, roadmap 1.3
 * K1; linux_internal.h). */

/* --- the clocks, sleeping, nice values and rebooting (lxtime.cee) -------------
 *
 * clock_gettime, gettimeofday, time, times, settimeofday, clock_settime,
 * nanosleep, clock_nanosleep, sched_yield, futex, getpriority, setpriority
 * and reboot are CeeSafe's (compat/linux/lxtime.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- the system (lxsys.cee) ---------------------------------------------------
 *
 * sysinfo, uname, sethostname and getrandom are CeeSafe's
 * (compat/linux/lxsys.cee, roadmap 1.3 K1; linux_internal.h). */

/* --- mounting and statfs (lxmount.cee) ---------------------------------------
 *
 * mount, umount2, statfs and fstatfs are CeeSafe's
 * (compat/linux/lxmount.cee, roadmap 1.3 K1; linux_internal.h). */

/* --- poll, ppoll, select, pselect6 (lxpoll.cee) -----------------------------------
 *
 * They are CeeSafe's (compat/linux/lxpoll.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- processes and threads (lxproc.cee) ------------------------------------------
 *
 * fork, vfork, clone, execve, sched_setaffinity and sched_getaffinity are
 * CeeSafe's (compat/linux/lxproc.cee, roadmap 1.3 K1; linux_internal.h). */

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
