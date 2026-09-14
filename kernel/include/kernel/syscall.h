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

/* The native signal ABI (kernel/process/native_signal.c): the frame a
 * handler runs on, and the four calls that install and mask signals. */
int native_signal_frame(struct arch_user_regs *regs, const struct sigaction_k *act,
                        const struct signal_info *info, uint64_t blocked_before);
int64_t sys_sigaction(struct syscall_args *a);
int64_t sys_sigprocmask(struct syscall_args *a);
int64_t sys_sigpending(struct syscall_args *a);
int64_t sys_sigreturn(struct syscall_args *a);

#endif /* KERNEL_SYSCALL_H */
