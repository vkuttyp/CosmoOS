/*
 * native.c - The CosmoOS native personality: system-call table and
 * implementations. Numbers come from uapi/cosmo/syscall.h.
 *
 * Every function validates its arguments first and returns a negative
 * errno on failure. User memory is touched only through uaccess.h.
 */

#include <kernel/compiler.h>
#include <kernel/aio.h>
#include <kernel/timerobj.h>
#include <kernel/blk.h>
#include <kernel/mountns.h>
#include <kernel/bootfs.h>
#include <kernel/bootinfo.h>
#include <kernel/cosmofs.h>   /* g_cosmofs_scrub_rate: sysctl fs.scrub_rate */
#include <kernel/storage.h>   /* g_pool_fault_errors: sysctl fs.fault_errors */
#include <kernel/elf.h>
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
#include <kernel/shutdown.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/unix.h>
#include <arch/user.h>
#include <kernel/futex.h>
#include <kernel/random.h>
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

/* --- the bounce, reading and writing (io.cee) -----------------------------
 *
 * syscall_bounce_get/_put and their counters, syscall_obj_read/_write,
 * syscall_handle_read/_write, sys_read, sys_write, sys_log and sys_close
 * are CeeSafe's (kernel/syscall/io.cee, roadmap 2.0 SA8; syscall.h). */

/* --- threads, futexes, sleeping and the clocks (threads.cee) ---------------
 *
 * sys_exit, sys_getpid, sys_thread_self, sys_futex_wait .. sys_futex_requeue,
 * sys_set_tls, sys_thread_exit, sys_thread_create, sys_yield, sys_sleep_ns
 * and sys_clock_ns are CeeSafe's (kernel/syscall/threads.cee, roadmap 2.0
 * SA8; syscall.h). */

/* --- mappings (mmap.cee) --------------------------------------------------
 *
 * sys_mmap, sys_msync, sys_munmap and sys_mprotect, and the file half of a
 * mapping request (mmap_file_check), are CeeSafe's (kernel/syscall/mmap.cee,
 * roadmap 2.0 SA8; syscall.h). */

/* --- Phase 7: files ------------------------------------------------------- */

static int get_path(uint64_t uptr, char *buf)
{
    int rc = strncpy_from_user(buf, uptr, VFS_PATH_MAX);
    if (rc < 0)
        return rc;
    return buf[0] == '\0' ? -ENOENT : 0;
}

/* --- paths and open files (files.cee) -------------------------------------
 *
 * sys_open, sys_symlink, sys_link, sys_chmod, sys_chown, sys_utimes,
 * sys_umask, sys_chroot, sys_readlink, sys_lstat, sys_stat, sys_fstat,
 * sys_lseek, sys_mkdir, sys_mknod, sys_unlink, sys_rmdir, sys_rename,
 * sys_getdents, sys_sync and sys_fsync are CeeSafe's
 * (kernel/syscall/files.cee, roadmap 2.0 SA8; syscall.h). The fstat both
 * doors share stays here. */

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

/* --- mounting and the root (mount.cee) -----------------------------------
 *
 * syscall_mount, syscall_umount, sys_mount, sys_umount and sys_switch_root
 * are CeeSafe's (kernel/syscall/mount.cee, roadmap 2.0 SA8; syscall.h). */

/* SYS_exec (roadmap M3): Linux execve's operation at the native door. */
static int64_t sys_exec(struct syscall_args *a)
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

/* --- sockets (sockets.cee) ---------------------------------------------------
 *
 * sys_socket, sys_bind, sys_listen, sys_accept, sys_connect, sys_sendto,
 * sys_recvfrom, sys_shutdown, sys_getsockname, sys_getsockopt, sys_sendmsg,
 * sys_recvmsg and sys_socketpair are CeeSafe's (kernel/syscall/sockets.cee,
 * roadmap 2.0 SA8; syscall.h). */

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
           COSMO_SPAWN_NEWMOUNTNS | COSMO_SPAWN_NEWUTSNS | COSMO_SPAWN_SETPGID | COSMO_SPAWN_SETSID |
           COSMO_SPAWN_DROPPRIV | COSMO_SPAWN_FOREGROUND)) ||
        req.path == NULL || req.argv == NULL)
        return -EINVAL;
    if ((req.flags & COSMO_SPAWN_SETSID) && (req.flags & COSMO_SPAWN_SETPGID))
        return -EINVAL;   /* a new session names its own group */
    if ((req.flags & COSMO_SPAWN_FOREGROUND) && !(req.flags & COSMO_SPAWN_SETPGID))
        return -EINVAL;   /* the foreground is a group's: the child must be put in one */
    size_t want = (req.flags & COSMO_SPAWN_DROPPRIV)  ? sizeof(req)
                  : (req.flags & COSMO_SPAWN_SETPGID) ? COSMO_SPAWN_SIZE_V3
                  : (req.flags & COSMO_SPAWN_SETROOT) ? COSMO_SPAWN_SIZE_V2
                                                      : COSMO_SPAWN_SIZE_V1;
    if (want > COSMO_SPAWN_SIZE_V1 && copy_from_user(&req, a->a[0], want))
        return -EFAULT;
    if ((req.flags & COSMO_SPAWN_DROPPRIV) && (req.priv_drop & ~COSMO_PRIV_ALL))
        return -EINVAL;
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
                       (req.flags & COSMO_SPAWN_SETPGID) != 0, (pid_t)req.pgid,
                       (req.flags & COSMO_SPAWN_SETSID) != 0, (req.flags & COSMO_SPAWN_FOREGROUND) != 0,
                       (req.flags & COSMO_SPAWN_DROPPRIV) ? req.priv_drop : 0, &pid);
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
    int rc = process_wait_child(pid, wf, &got, &status, NULL);
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
    if (cur == NULL)
        return -ESRCH;
    /* A process outside domain 0 cannot reach another domain, and is
     * told the target does not exist rather than that it may not touch
     * it: -EPERM would confirm the pid is in use, which is the one
     * thing the domain is meant not to tell it. */
    if (cur->domain != 0 && target->domain != cur->domain)
        return -ESRCH;
    if (!process_may_signal(cur, target, sig))
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

/* --- sessions, the terminal, credentials, limits (jobctl.cee) ---------------
 *
 * sys_setpgid .. sys_tcsetpgrp, sys_setresuid .. sys_getgroups,
 * sys_getrlimit, sys_setrlimit, sys_ioready and sys_setnonblock are
 * CeeSafe's (kernel/syscall/jobctl.cee, roadmap 2.0 SA8; syscall.h). */

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

static int64_t sys_timer_create(struct syscall_args *a)
{
    struct kobject *obj;
    int rc = timer_obj_create(a->a[0], a->a[1], &obj);
    if (rc)
        return rc;
    int h = handle_install(&process_current()->handles, obj, HANDLE_RIGHT_ALL);
    kobject_put(obj);
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
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
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

static const char *const sysctl_names[] = {
    "kernel.name", "kernel.version", "kernel.build", "kernel.arch", "kernel.uptime_ns", "kernel.nprocs",
    "kernel.hostname", "kernel.cmdline", "kernel.root", "kernel.rootdev", "kernel.verified_boot",
    "hw.ncpu", "vm.page_size", "vm.pages_total", "vm.pages_free", "vm.cache_pages", "vm.cache_limit",
    "vm.cache_writebacks", "vm.cache_exec_syncs", "vm.file_faults", "vm.file_cow_faults", "vm.file_dirty_faults",
    "vm.file_fault_retries", "vm.file_sigbus", "vm.futex_shared_keys", "vm.anon_fault_retries",
    "hv.backend", "hv.vms", "hv.vcpus", "hv.exits",
    "net.steer",
    "fs.scrub_rate",
    "fs.fault_errors",
    "fs.resilver_rate",
    "sysctl.names",
    "debug.faultinject",
    "debug.preempt_probe",
    "debug.file_fault_hold",
    "debug.anon_fault_hold",
    "debug.cwd_hold",
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
    /* The command line and what its root= names (kernel/core/bootfs.c):
     * init reads these to decide whether to switch to a disk root.
     * kernel.rootdev is resolved at the read, "" when root= names nothing
     * registered (or there is no root=). */
    if (strcmp(name, "kernel.cmdline") == 0)
        return ksnprintf(out, n, "%s", bootinfo_cmdline());
    if (strcmp(name, "kernel.root") == 0)
        return ksnprintf(out, n, "%s", bootfs_root_spec());
    if (strcmp(name, "kernel.rootdev") == 0) {
        char dev[BLKDEV_NAME_MAX];
        if (bootfs_root_device(dev, sizeof(dev)) != 0)
            dev[0] = '\0';
        return ksnprintf(out, n, "%s", dev);
    }
    /* Verified boot (boot protocol v8, docs/boot/design.md): "yes <key id>"
     * when the loader checked kernel, archive and command line against the
     * signed manifest, "no" for a VERIFY_BOOT=0 loader. */
    if (strcmp(name, "kernel.verified_boot") == 0) {
        uint8_t id[COSMOBOOT_KEY_ID_SIZE];
        if (!bootinfo_verified(id))
            return ksnprintf(out, n, "no");
        return ksnprintf(out, n, "yes %02x%02x%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5],
                         id[6], id[7]);
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
    if (strcmp(name, "vm.anon_fault_retries") == 0) {
        struct vm_stats st;
        vm_get_stats(&st);
        return ksnprintf(out, n, "%llu", (unsigned long long)st.anon_fault_retries);
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
    if (strcmp(name, "debug.anon_fault_hold") == 0) {
#if CONFIG_DEBUG
        return ksnprintf(out, n, "%u", vm_test_anon_hold_state());
#else
        return -ENOENT;
#endif
    }
    if (strcmp(name, "debug.cwd_hold") == 0) {
#if CONFIG_DEBUG
        return ksnprintf(out, n, "%u", vfs_test_cwd_hold_state());
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
    /* The scrub's bound, blocks a second (roadmap 1.2 B2): a scrub reads
     * it when it starts. 0 is no bound. */
    if (strcmp(name, "fs.scrub_rate") == 0)
        return ksnprintf(out, n, "%u", __atomic_load_n(&g_cosmofs_scrub_rate, __ATOMIC_RELAXED));
    /* Failures after which a mirror's copy is FAULTED (roadmap 1.2 B3);
     * 0 never faults one. */
    if (strcmp(name, "fs.fault_errors") == 0)
        return ksnprintf(out, n, "%u", __atomic_load_n(&g_pool_fault_errors, __ATOMIC_RELAXED));
    /* The resilver's bound, blocks a second (roadmap 1.2 B3): read at
     * each idle wake of the resilver thread. 0 is no bound. */
    if (strcmp(name, "fs.resilver_rate") == 0)
        return ksnprintf(out, n, "%u", __atomic_load_n(&g_cosmofs_resilver_rate, __ATOMIC_RELAXED));
    if (strcmp(name, "debug.faultinject") == 0) {
        int len = faultinject_sysctl(out, n);
        return len < 0 ? -ENOENT : len;   /* -ENOENT in release builds: the knob does not exist */
    }
    if (strcmp(name, "debug.preempt_probe") == 0) {
#if CONFIG_SELFTEST
        /* The read is the system call under test
         * (docs/audit/next-subsystem-wake-preempt.md); it creates a
         * kernel thread, so only a PRIV_SYS_ADMIN caller may ask. */
        if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
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

/*
 * Roadmap M5: a sysctl write. One name is writable, kernel.hostname (what
 * sethostname writes, in the caller's uts namespace); writing needs
 * PRIV_SYS_ADMIN. -ENOENT for an unknown name, -EROFS for one that is
 * read-only, -EPERM without the privilege.
 */
/* Shell completeness S5: init's door to kernel_reboot (the Linux
 * personality's reboot(2) is the other). */
static int64_t sys_reboot(struct syscall_args *a)
{
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
        return -EPERM;
    switch (a->a[0]) {
    case COSMO_REBOOT_RESTART:
        kernel_reboot(KERNEL_REBOOT_RESTART);
    case COSMO_REBOOT_HALT:
        kernel_reboot(KERNEL_REBOOT_HALT);
    case COSMO_REBOOT_POWER_OFF:
        kernel_reboot(KERNEL_REBOOT_POWER_OFF);
    default:
        return -EINVAL;
    }
}

/*
 * Roadmap 1.2 B5: what a key, a salt or a key file is made of, for the
 * native personality, which had only the sixteen AT_RANDOM bytes a
 * process starts with. The Linux door's getrandom without its flags: it
 * waits for the pool to be seeded (an unseeded pool's bytes are not key
 * material, invariant S18) and gives at most 256 bytes, as getentropy.
 */
static int64_t sys_getrandom(struct syscall_args *a)
{
    size_t len = (size_t)a->a[1];
    if (len > 256)
        return -EINVAL;
    if (!user_range_ok(a->a[0], len))
        return -EFAULT;
    int rc = random_wait_ready(RANDOM_WAIT_FOREVER);
    if (rc)
        return rc;
    uint8_t tmp[256];
    random_get_bytes(tmp, len);
    rc = copy_to_user(a->a[0], tmp, len) ? -EFAULT : 0;
    memset(tmp, 0, sizeof(tmp));
    return rc ? rc : (int64_t)len;
}

static int64_t sys_sysctl_set(struct syscall_args *a)
{
    char name[64];
    int rc = strncpy_from_user(name, a->a[0], sizeof(name));
    if (rc < 0)
        return rc == -ENAMETOOLONG ? -ENOENT : rc;
    bool known = false;
    for (size_t i = 0; i < ARRAY_SIZE(sysctl_names); i++)
        known = known || strcmp(name, sysctl_names[i]) == 0;
    /* Listed but not built in this kernel: absent, as a read says (PR
     * #370 review). The build conditions are sysctl_value's. */
    if (strcmp(name, "debug.preempt_probe") == 0)
        known = known && CONFIG_SELFTEST;
    else if (strcmp(name, "debug.faultinject") == 0)
        known = known && CONFIG_FAULTINJECT;
    else if (strncmp(name, "debug.", 6) == 0)
        known = known && CONFIG_DEBUG;
    if (!known)
        return -ENOENT;
    if (strcmp(name, "fs.scrub_rate") == 0 || strcmp(name, "fs.fault_errors") == 0 ||
        strcmp(name, "fs.resilver_rate") == 0) {
        if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
            return -EPERM;
        size_t len = (size_t)a->a[2];
        char buf[16];
        if (len == 0 || len >= sizeof(buf))
            return -EINVAL;
        if (copy_from_user(buf, a->a[1], len))
            return -EFAULT;
        buf[len] = '\0';
        uint64_t v = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] < '0' || buf[i] > '9')
                return -EINVAL;   /* digits only, as fsctl's ids */
            v = v * 10 + (uint64_t)(buf[i] - '0');
        }
        if (v > (1u << 24))
            return -EINVAL;   /* 64 GiB a second, or 16 M failures: past it the bound means nothing */
        if (strcmp(name, "fs.fault_errors") == 0)
            __atomic_store_n(&g_pool_fault_errors, (uint32_t)v, __ATOMIC_RELAXED);
        else if (strcmp(name, "fs.resilver_rate") == 0)
            __atomic_store_n(&g_cosmofs_resilver_rate, (unsigned)v, __ATOMIC_RELAXED);
        else
            __atomic_store_n(&g_cosmofs_scrub_rate, (unsigned)v, __ATOMIC_RELAXED);
        return 0;
    }
    if (strcmp(name, "kernel.hostname") != 0)
        return -EROFS;
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
        return -EPERM;
    size_t len = (size_t)a->a[2];
    if (len >= COSMO_HOST_NAME_MAX)
        return -EINVAL;
    char buf[COSMO_HOST_NAME_MAX];
    if (copy_from_user(buf, a->a[1], len))
        return -EFAULT;
    return utsns_sethostname(utsns_current(), buf, len);
}

_Static_assert(COSMO_ID_KEEP == VFS_ID_KEEP, "sys_chown passes the user's keep straight through");

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
    [SYS_mknod] = sys_mknod,
    [SYS_pipe] = sys_pipe,
    [SYS_dup] = sys_dup,
    [SYS_getppid] = sys_getppid,
    [SYS_chdir] = sys_chdir,
    [SYS_getcwd] = sys_getcwd,
    [SYS_switch_root] = sys_switch_root,
    [SYS_exec] = sys_exec,
    [SYS_procinfo] = sys_procinfo,
    [SYS_syscall_filter] = sys_syscall_filter,
    [SYS_gethostname] = sys_gethostname,
    [SYS_sethostname] = sys_sethostname,
    [SYS_klog] = sys_klog,
    [SYS_sysctl] = sys_sysctl,
    [SYS_sysctl_set] = sys_sysctl_set,
    [SYS_reboot] = sys_reboot,
    [SYS_getrandom] = sys_getrandom,
    [SYS_link] = sys_link,
    [SYS_chmod] = sys_chmod,
    [SYS_chown] = sys_chown,
    [SYS_utimes] = sys_utimes,
    [SYS_umask] = sys_umask,
    [SYS_chroot] = sys_chroot,
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
    [SYS_timer_create] = sys_timer_create,
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

static bool native_claims_elf(const struct elf_info *info)
{
    return info->cosmo_note;
}

/*
 * The native auxiliary vector. The program header table, so a program can
 * read its own headers -- which is how it finds its own PT_TLS, and why
 * nothing in this kernel knows what thread-local storage is. The values
 * are the ones the Linux door also passes as AT_PHDR/AT_PHENT; zero when
 * the headers are not inside a mapped segment.
 *
 * Zero is "the headers are not readable", **not** "the program has no
 * thread-local storage** -- and a reader must not collapse the two. libc
 * refuses to start a program it cannot answer that question for
 * (libc/src/tlsscan.c), because the other answer is a process whose
 * `__thread` variables were never initialised and whose per-thread
 * storage was sized as though it had none. Every native program links
 * with the one userland/user.ld, which keeps the header table in the text
 * segment, so this stays zero only for an image built some other way.
 */
static unsigned native_auxv(struct process *p, const struct elf_info *info, const struct personality_auxv_args *x,
                            uint64_t *w, unsigned max)
{
    (void)p;
    unsigned k = 0;
    KASSERT(max >= 14);
    w[k++] = COSMO_AT_PHDR;   w[k++] = info->phdr_vaddr;
    w[k++] = COSMO_AT_PHENT;  w[k++] = info->phent;
    w[k++] = COSMO_AT_PHNUM;  w[k++] = info->phnum;
    w[k++] = COSMO_AT_PAGESZ; w[k++] = PAGE_SIZE;
    w[k++] = COSMO_AT_ENTRY;  w[k++] = info->entry;
    w[k++] = COSMO_AT_RANDOM; w[k++] = x->random_addr;
    w[k++] = COSMO_AT_NULL;   w[k++] = 0;
    return k;
}

const struct personality personality_native = {
    .name = "native",
    .claims_elf = native_claims_elf,
    .auxv = native_auxv,
    .table = native_table,
    .count = SYS_COUNT,
    .always_allowed = native_always_allowed,
    .nr_always_allowed = sizeof(native_always_allowed) / sizeof(native_always_allowed[0]),
    .signal_frame = native_signal_frame,
};
