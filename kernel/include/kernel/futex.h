/*
 * futex.h - Wait on and wake by a user-space word (docs/compat/linux/design.md).
 *
 * A native primitive used by the Linux personality today; native user
 * threads will use it when they exist. Keyed by (address space, user
 * address); the compare and the enqueue happen under one lock so a wake
 * between them cannot be lost.
 */

#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <kernel/types.h>

struct vm_space;

/* Block while *uaddr == val: 0 when woken, -EAGAIN when the word differs,
 * -ETIMEDOUT after timeout_ns (0: no timeout), -EINTR when killed,
 * -EFAULT when the word cannot be read. Thread context. */
/* Initialise the wait buckets; kernel_main calls it before the first process. */
void futex_init(void);

int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns);

/* Wake up to `n` waiters on (space, uaddr); returns how many. Any thread context. */
int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n);

#endif /* KERNEL_FUTEX_H */
