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

/* Diagnostics: calls and unknown numbers seen. */
uint64_t syscall_count(void);
uint64_t syscall_unknown_count(void);

#endif /* KERNEL_SYSCALL_H */
