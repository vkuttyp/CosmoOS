/*
 * completion.h - One-shot "something finished" signal.
 *
 * complete() may be called from interrupt context; wait_for_completion()
 * may not. Once completed it stays completed; waiters after the fact
 * return immediately.
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
