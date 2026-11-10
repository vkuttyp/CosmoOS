/*
 * syscall.h - Generic system-call dispatch.
 *
 * The architecture entry captures registers into an arch-specific frame
 * and calls syscall_dispatch() with interrupts enabled on the calling
 * thread's kernel stack. The dispatcher builds struct syscall_args and
 * routes the number through the current process's personality.
 */

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <kernel/process.h>

struct syscall_args {
    uint64_t nr;
    uint64_t a[6];
    void *frame;    /* arch frame, opaque to generic code */
};

/* Called by the arch entry. Returns the value to place in the user's
 * result register. */
int64_t syscall_dispatch(uint64_t nr, const uint64_t args[6], void *frame);

/* Handle I/O shared by the personalities (kernel/syscall/native.c):
 * validate the user range, look the handle up with the right, copy
 * through a bounded kernel buffer. Bytes or -errno. */
int64_t syscall_handle_read(int h, uint64_t ubuf, size_t len);
int64_t syscall_handle_write(int h, uint64_t ubuf, size_t len);
/* The same on an object the caller already holds (the I/O ring): the
 * copy goes through a kernel bounce buffer, bounded by the returned count. */
struct kobject;
int64_t syscall_obj_read(struct kobject *obj, uint64_t ubuf, size_t len);
int64_t syscall_obj_write(struct kobject *obj, uint64_t ubuf, size_t len);

/*
 * The bounce every user copy goes through, sized to the request
 * (docs/kernel/syscall/architecture.md, "The bounce"): the caller's stack chunk
 * for a kilobyte and less, the heap above it up to IO_BOUNCE_MAX -- one
 * object call for a big read, where a 64 KiB read used to be sixty-four
 * -- and a heap allocation that fails degrades to the stack chunk, never
 * to an error. Both personalities use it (native read/write, Linux
 * read/write/pread/pwrite and the private file mapping's fill).
 */
#define IO_CHUNK      1024u              /* on the stack: a console line, a small read */
#define IO_BOUNCE_MAX (64u * 1024u)      /* from the heap: the ceiling of one object call */
struct io_bounce {
    char *buf;
    size_t cap;
    bool heap;
};
void syscall_bounce_get(struct io_bounce *b, char *stack, size_t len);
void syscall_bounce_put(struct io_bounce *b);
/* Diagnostics: bounces taken from the heap, and heap refusals that fell
 * back to the stack chunk. */
uint64_t syscall_bounce_heap_count(void);
uint64_t syscall_bounce_fallback_count(void);
/* fstat on any I/O object; 0 or -errno with *st filled. */
struct cosmo_stat;
int syscall_handle_stat(int h, struct cosmo_stat *st);

/* Mount and unmount for both doors (shell completeness S6): the
 * PRIV_MOUNT check, then the target resolved from the caller's directory
 * (P27). `device` is a block device name or NULL; `flags` are MOUNT_*
 * and VFS_UMOUNT_*, already translated from the caller's ABI. */
int syscall_mount(const char *device, const char *target, const char *fstype, unsigned flags);
int syscall_umount(const char *target, unsigned flags);

/* Diagnostics: calls and unknown numbers seen. */
uint64_t syscall_count(void);
uint64_t syscall_unknown_count(void);
/* Calls refused by a filter, over the life of the system. */
uint64_t syscall_filtered_count(void);

struct process;
/* Whether `p` may make call `nr` (docs/kernel/security/design.md §1f).
 * Numbers past the mask are denied: unknown fails safe. */
bool syscall_allowed(const struct process *p, uint64_t nr);
/* Intersect `p`'s filter with `mask` (`words` of it, the rest treated
 * as zero). Only ever narrows. */
void syscall_filter_install(struct process *p, const uint64_t *mask, unsigned words);

/* Virtualization system calls (kernel-services/virtualization/hvsys.c). */
int64_t sys_vm_create(struct syscall_args *a);
int64_t sys_vm_mem(struct syscall_args *a);
int64_t sys_vm_mem_rw(struct syscall_args *a);
int64_t sys_vm_raise_spi(struct syscall_args *a);
int64_t sys_vm_lower_spi(struct syscall_args *a);
int64_t sys_vcpu_create(struct syscall_args *a);
int64_t sys_vcpu_regs(struct syscall_args *a);
int64_t sys_vcpu_run(struct syscall_args *a);
int64_t sys_vcpu_stop(struct syscall_args *a);
int64_t sys_vcpu_irq(struct syscall_args *a);

/* The native calls on mappings (kernel/syscall/mmap.cee). */
int64_t sys_mmap(struct syscall_args *a);
int64_t sys_msync(struct syscall_args *a);
int64_t sys_munmap(struct syscall_args *a);
int64_t sys_mprotect(struct syscall_args *a);

/* The native calls on mounting and the root (kernel/syscall/mount.cee). */
int64_t sys_mount(struct syscall_args *a);
int64_t sys_umount(struct syscall_args *a);
int64_t sys_switch_root(struct syscall_args *a);

/* The native calls on paths and open files (kernel/syscall/files.cee). */
int64_t sys_open(struct syscall_args *a);
int64_t sys_symlink(struct syscall_args *a);
int64_t sys_link(struct syscall_args *a);
int64_t sys_chmod(struct syscall_args *a);
int64_t sys_chown(struct syscall_args *a);
int64_t sys_utimes(struct syscall_args *a);
int64_t sys_umask(struct syscall_args *a);
int64_t sys_chroot(struct syscall_args *a);
int64_t sys_readlink(struct syscall_args *a);
int64_t sys_lstat(struct syscall_args *a);
int64_t sys_stat(struct syscall_args *a);
int64_t sys_fstat(struct syscall_args *a);
int64_t sys_lseek(struct syscall_args *a);
int64_t sys_mkdir(struct syscall_args *a);
int64_t sys_mknod(struct syscall_args *a);
int64_t sys_unlink(struct syscall_args *a);
int64_t sys_rmdir(struct syscall_args *a);
int64_t sys_rename(struct syscall_args *a);
int64_t sys_getdents(struct syscall_args *a);
int64_t sys_sync(struct syscall_args *a);
int64_t sys_fsync(struct syscall_args *a);

/* The native calls on processes' threads, futexes, sleeping and the
 * clocks (kernel/syscall/threads.cee). */
int64_t sys_exit(struct syscall_args *a);
int64_t sys_getpid(struct syscall_args *a);
int64_t sys_thread_self(struct syscall_args *a);
int64_t sys_futex_wait(struct syscall_args *a);
int64_t sys_futex_wake(struct syscall_args *a);
int64_t sys_futex_requeue(struct syscall_args *a);
int64_t sys_set_tls(struct syscall_args *a);
int64_t sys_thread_exit(struct syscall_args *a);
int64_t sys_thread_create(struct syscall_args *a);
int64_t sys_yield(struct syscall_args *a);
int64_t sys_sleep_ns(struct syscall_args *a);
int64_t sys_clock_ns(struct syscall_args *a);

/* The native calls on sessions, the terminal, credentials, limits,
 * readiness and the blocking mode (kernel/syscall/jobctl.cee). */
int64_t sys_setpgid(struct syscall_args *a);
int64_t sys_getpgid(struct syscall_args *a);
int64_t sys_setsid(struct syscall_args *a);
int64_t sys_getsid(struct syscall_args *a);
int64_t sys_tcgetpgrp(struct syscall_args *a);
int64_t sys_tcgetattr(struct syscall_args *a);
int64_t sys_tcsetattr(struct syscall_args *a);
int64_t sys_ttysize(struct syscall_args *a);
int64_t sys_tcsetpgrp(struct syscall_args *a);
int64_t sys_setresuid(struct syscall_args *a);
int64_t sys_setresgid(struct syscall_args *a);
int64_t sys_getresuid(struct syscall_args *a);
int64_t sys_getresgid(struct syscall_args *a);
int64_t sys_setgroups(struct syscall_args *a);
int64_t sys_getgroups(struct syscall_args *a);
int64_t sys_getrlimit(struct syscall_args *a);
int64_t sys_setrlimit(struct syscall_args *a);
int64_t sys_ioready(struct syscall_args *a);
int64_t sys_setnonblock(struct syscall_args *a);

/* The native signal ABI (kernel/process/native_signal.c): the frame a
 * handler runs on, and the four calls that install and mask signals. */
int native_signal_frame(struct arch_user_regs *regs, const struct sigaction_k *act,
                        const struct signal_info *info, uint64_t blocked_before);
int64_t sys_sigaction(struct syscall_args *a);
int64_t sys_sigprocmask(struct syscall_args *a);
int64_t sys_sigpending(struct syscall_args *a);
int64_t sys_sigreturn(struct syscall_args *a);

#endif /* KERNEL_SYSCALL_H */
