/*
 * completion.h - One-shot "something finished" signal.
 *
 * complete() may be called from interrupt context; wait_for_completion()
 * may not. Once completed it stays completed; waiters after the fact
 * return immediately. When wait_for_completion returns, complete() has
 * finished touching the completion, so a caller may free it (it usually
 * lives on the caller's stack). completion_done() alone gives no such
 * guarantee: it is a query, and a caller that must free the completion
 * waits with wait_for_completion or wait_for_completion_timeout, which
 * do the handshake -- polling completion_done() and then freeing races
 * complete() (docs/audit/next-subsystem-nvme-admin.md).
 */

#ifndef KERNEL_COMPLETION_H
#define KERNEL_COMPLETION_H

#include <kernel/spinlock.h>
#include <kernel/wait.h>

struct completion {
    spinlock_t lock;
    bool done;
    struct waitqueue wq;
};

void completion_init(struct completion *c, const char *name);
void complete(struct completion *c);
/* complete(), with a spin of `linger_ns` held between publishing `done`
 * and the wake -- the seam a test uses to open the window a poller races.
 * complete(c) is complete_linger(c, 0); nothing else passes non-zero. */
void complete_linger(struct completion *c, uint64_t linger_ns);
void wait_for_completion(struct completion *c);
/* Wait until `c` is completed or `timeout_ns` passes. True: completed,
 * and complete() has finished with `c` (the handshake), so the caller may
 * free it. False: timed out; `c` may still be completed later, so the
 * caller must stop whatever will complete it, or wait_for_completion,
 * before the memory goes. Sleeps; not for interrupt context. */
bool wait_for_completion_timeout(struct completion *c, uint64_t timeout_ns);
bool completion_done(struct completion *c);

#endif /* KERNEL_COMPLETION_H */
