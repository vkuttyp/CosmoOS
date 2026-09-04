/*
 * sched_internal.h - Private interfaces between thread.c, sched.c, and
 * wait.c.
 */

#ifndef KERNEL_SCHEDULER_INTERNAL_H
#define KERNEL_SCHEDULER_INTERNAL_H

#include <kernel/thread.h>

/* thread.c */
void thread_init_subsystem(void);
struct thread *thread_alloc(const char *name, int priority, unsigned flags);
/* Allocate a thread with stack and initial context but do not enqueue. */
struct thread *thread_prepare(void (*entry)(void *arg), void *arg, const char *name, int priority,
                              unsigned flags);

/* sched.c */
void sched_finish_switch(void);
void sched_enqueue_new(struct thread *t);
/* After a wait completes: if a wake already queued us, dequeue; mark RUNNING. */
void sched_set_running_current(void);

#endif /* KERNEL_SCHEDULER_INTERNAL_H */
