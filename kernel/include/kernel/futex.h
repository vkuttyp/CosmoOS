/*
 * futex.h - Wait on and wake by a user word (kernel/ipc/futex.c;
 * docs/kernel/ipc/design.md "futex", docs/compat/linux/design.md).
 *
 * A futex's identity is what the word maps (the shared-futex unit,
 * docs/audit/next-subsystem-shared-futex.md): a word in the process's own
 * memory is keyed by (space, uaddr); a word in a MAP_SHARED file mapping
 * is keyed by (vnode, file offset), so two processes sharing a page share
 * the futex. `private` skips the classification (Linux's
 * FUTEX_PRIVATE_FLAG: the program's promise that nobody else can see the
 * word, and the cheaper path); the native calls always classify.
 */

#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <kernel/types.h>

struct vm_space;
struct vnode;

struct futex_key {
    const void *obj;     /* private: the vm_space; shared: the vnode */
    uint64_t off;        /* private: uaddr; shared: the file offset of the word */
    struct vnode *held;  /* shared: the reference this key holds; NULL for a private key */
};

void futex_init(void);

/* 0 woken, -EAGAIN (the word differs from val), -ETIMEDOUT, -EINTR
 * (killed), -EFAULT, -EINVAL (misaligned). timeout_ns 0: no timeout. */
int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns, bool private);
/* Wakes up to n waiters on the word; returns how many. */
int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n, bool private);
/* Wakes up to nr_wake waiters on uaddr1 and moves up to nr_requeue more
 * onto uaddr2; with cmp, only if uaddr1 still holds cmpval (-EAGAIN).
 * Returns woken + requeued. A word requeued onto itself is counted and
 * left where it is. */
int futex_requeue(struct vm_space *space, uint64_t uaddr1, uint64_t uaddr2, unsigned nr_wake, unsigned nr_requeue,
                  bool cmp, uint32_t cmpval, bool private);

#endif /* KERNEL_FUTEX_H */
