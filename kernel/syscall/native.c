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

/* --- paths and open files (files.cee) -------------------------------------
 *
 * sys_open, sys_symlink, sys_link, sys_chmod, sys_chown, sys_utimes,
 * sys_umask, sys_chroot, sys_readlink, sys_lstat, sys_stat, sys_fstat,
 * sys_lseek, sys_mkdir, sys_mknod, sys_unlink, sys_rmdir, sys_rename,
 * sys_getdents, sys_sync and sys_fsync are CeeSafe's
 * (kernel/syscall/files.cee, roadmap 2.0 SA8; syscall.h). */

/* --- mounting and the root (mount.cee) -----------------------------------
 *
 * syscall_mount, syscall_umount, sys_mount, sys_umount and sys_switch_root
 * are CeeSafe's (kernel/syscall/mount.cee, roadmap 2.0 SA8; syscall.h). */

/* --- sockets (sockets.cee) ---------------------------------------------------
 *
 * sys_socket, sys_bind, sys_listen, sys_accept, sys_connect, sys_sendto,
 * sys_recvfrom, sys_shutdown, sys_getsockname, sys_getsockopt, sys_sendmsg,
 * sys_recvmsg and sys_socketpair are CeeSafe's (kernel/syscall/sockets.cee,
 * roadmap 2.0 SA8; syscall.h). */

/* --- processes and the objects beside them (procs.cee) ---------------------
 *
 * sys_spawn, sys_exec, sys_wait, sys_kill, sys_thread_kill, sys_aio_create,
 * sys_timer_create, sys_aio_submit, sys_aio_wait, sys_pipe, sys_dup,
 * sys_getppid, sys_chdir, sys_getcwd, sys_procinfo, sys_klog,
 * sys_syscall_filter, sys_gethostname and sys_sethostname, and the fstat
 * both doors share (syscall_handle_stat), are CeeSafe's
 * (kernel/syscall/procs.cee, roadmap 2.0 SA8; syscall.h). */

/* --- sessions, the terminal, credentials, limits (jobctl.cee) ---------------
 *
 * sys_setpgid .. sys_tcsetpgrp, sys_setresuid .. sys_getgroups,
 * sys_getrlimit, sys_setrlimit, sys_ioready and sys_setnonblock are
 * CeeSafe's (kernel/syscall/jobctl.cee, roadmap 2.0 SA8; syscall.h). */

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
