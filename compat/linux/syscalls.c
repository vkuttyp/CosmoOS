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

/* --- reads, writes and opens (lxio.cee) ---------------------------------------
 *
 * read, write, pread64, pwrite64, readv, writev, open, creat, openat, close
 * and lseek are CeeSafe's (compat/linux/lxio.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- stat and symbolic links (lxstat.cee) ------------------------------------
 *
 * stat, lstat, fstat, newfstatat, statx, readlink, readlinkat, symlink and
 * symlinkat are CeeSafe's (compat/linux/lxstat.cee, roadmap 1.3 K1;
 * linux_internal.h). */

/* --- descriptor objects (lxfd.cee) -------------------------------------------
 *
 * eventfd, eventfd2, signalfd, signalfd4, the timerfd calls, memfd_create,
 * ftruncate and the epoll calls are CeeSafe's (compat/linux/lxfd.cee,
 * roadmap 1.3 K1; linux_internal.h). */

/* --- directories (lxdir.cee) -------------------------------------------------
 *
 * getdents64, mkdir, mkdirat, rmdir, unlink, access, mknod and mknodat are
 * CeeSafe's (compat/linux/lxdir.cee, roadmap 1.3 K1; linux_internal.h). */

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

/* --- the terminal (lxtty.cee) ------------------------------------------------
 *
 * ioctl is CeeSafe's (compat/linux/lxtty.cee, roadmap 1.3 K1;
 * linux_internal.h). */

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
