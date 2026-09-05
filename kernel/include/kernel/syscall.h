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
/* fstat on any I/O object; 0 or -errno with *st filled. */
struct cosmo_stat;
int syscall_handle_stat(int h, struct cosmo_stat *st);

/* Diagnostics: calls and unknown numbers seen. */
uint64_t syscall_count(void);
uint64_t syscall_unknown_count(void);

#endif /* KERNEL_SYSCALL_H */
