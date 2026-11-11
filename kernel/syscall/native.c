/*
 * native.c - The CosmoOS native personality: its system-call table and its
 * record. Numbers come from uapi/cosmo/syscall.h; the calls are CeeSafe's,
 * in the files the notes below name (roadmap 2.0 SA8).
 */

#include <kernel/compiler.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>

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

/* --- sysctl, reboot, getrandom (sysctl.cee) --------------------------------
 *
 * sys_sysctl, sys_sysctl_set, sys_reboot and sys_getrandom are CeeSafe's
 * (kernel/syscall/sysctl.cee, roadmap 2.0 SA8; syscall.h). */

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

/* --- what the native personality claims, and its auxiliary vector ----------
 *
 * native_claims_elf and native_auxv are CeeSafe's
 * (kernel/syscall/personality.cee, roadmap 2.0 SA8; syscall.h). */

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
