/*
 * completion.h - One-shot "something finished" signal.
 *
 * complete() may be called from interrupt context; wait_for_completion()
 * may not. Once completed it stays completed; waiters after the fact
 * return immediately. When wait_for_completion returns, complete() has
 * finished touching the completion, so a caller may free it (it usually
 * lives on the caller's stack). completion_done() alone gives no such
 * guarantee: a poller that saw it true must still call
 * wait_for_completion before the memory goes.
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
void wait_for_completion(struct completion *c);
bool completion_done(struct completion *c);

#endif /* KERNEL_COMPLETION_H */
