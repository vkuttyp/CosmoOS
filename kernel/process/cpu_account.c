/*
 * cpu_account.c - CPU time split into user and system time, and the CPU-time
 * interval timers ITIMER_VIRTUAL and ITIMER_PROF (docs/kernel/process/design.md,
 * "CPU time"; kernel/include/kernel/cputime.h).
 *
 * The process side: the tick's hook, the totals a report reads, a waited-for
 * child's time reaching its parent, and the timers' system-call entry
 * points. The arithmetic and the timers' state machine are Nova's
 * (kernel/process/cputime.c, generated), called under the process's lock.
 *
 * The tick samples the thread it interrupted: TICK_NS of user time if it
 * interrupted user mode, of system time otherwise. That runs in interrupt
 * context, before the tick takes the run-queue lock (a leaf, S2): the
 * process's lock is taken there already by signal_send (tty_input, the
 * ITIMER_REAL callback), and it is released before a timer's signal is sent.
 */

#include <kernel/cputime.h>
#include <kernel/list.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <arch/irq.h>
#include <arch/user.h>

static void send_timer_signal(struct process *p, int sig)
{
    struct signal_info info = { .sig = sig, .source = SIGSRC_KERNEL };
    signal_send(p, sig, &info);
}

void process_cputime_tick(const struct arch_trap_frame *frame)
{
    struct thread *t = thread_current();
    struct process *p = t != NULL ? t->proc : NULL;
    if (p == NULL)
        return;   /* a kernel thread, or the idle thread: no process to charge */
    bool user = arch_trap_frame_is_user(frame);
    /* Only the tick of the CPU the thread runs on writes its sample;
     * thread_cpu_times loads it, from the thread itself. */
    uint64_t *slot = user ? &t->cpu_sample.user_ns : &t->cpu_sample.system_ns;
    __atomic_store_n(slot, __atomic_load_n(slot, __ATOMIC_RELAXED) + TICK_NS, __ATOMIC_RELAXED);
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    unsigned due = cputime_tick(&p->cputime, user, TICK_NS);
    spin_unlock_irqrestore(&p->lock, s);
    if (due & CPUTIME_DUE_VIRT)
        send_timer_signal(p, SIGVTALRM);
    if (due & CPUTIME_DUE_PROF)
        send_timer_signal(p, SIGPROF);
}

/* Under p->lock: the run time of every thread the process has had, with
 * the running ones' current stretch (sched_thread_run_ns; rq->lock nests
 * inside p->lock). A thread's own is folded into gone_run_ns when it
 * leaves the process (thread_put), under the same lock. */
static uint64_t run_ns_locked(struct process *p, uint64_t now_ns)
{
    uint64_t run = p->cputime.gone_run_ns;
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link)
        run += sched_thread_run_ns(t, now_ns);
    return run;
}

uint64_t process_cpu_run_ns(struct process *p)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    uint64_t run = run_ns_locked(p, now);
    spin_unlock_irqrestore(&p->lock, s);
    return run;
}

void process_cpu_times_locked(struct process *p, uint64_t now_ns, struct cputime_pair *self,
                              struct cputime_pair *children)
{
    cputime_adjust(run_ns_locked(p, now_ns), &p->cputime.sample, &p->cputime.reported);
    *self = p->cputime.reported;
    if (children != NULL)
        *children = p->cputime.children;
}

void process_cpu_times(struct process *p, struct cputime_pair *self, struct cputime_pair *children)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    process_cpu_times_locked(p, now, self, children);
    spin_unlock_irqrestore(&p->lock, s);
}

/* The child's lock, then the parent's, never both: a waiter holds neither
 * when it calls this (process_wait_child, after taking the child off its
 * list). Another thread of the parent reading RUSAGE_CHILDREN meanwhile
 * sees the total without this child, as it would a moment earlier. */
void process_cputime_reap(struct process *parent, struct process *child, struct cputime_pair *usage)
{
    struct cputime_pair self, kids;
    process_cpu_times(child, &self, &kids);
    struct cputime_pair all = { self.user_ns + kids.user_ns, self.system_ns + kids.system_ns };
    arch_irq_state_t s = spin_lock_irqsave(&parent->lock);
    parent->cputime.children.user_ns += all.user_ns;
    parent->cputime.children.system_ns += all.system_ns;
    spin_unlock_irqrestore(&parent->lock, s);
    if (usage != NULL)
        *usage = all;
}

void thread_cpu_times(struct cputime_pair *self)
{
    struct thread *t = thread_current();
    struct cputime_pair sample = {
        __atomic_load_n(&t->cpu_sample.user_ns, __ATOMIC_RELAXED),
        __atomic_load_n(&t->cpu_sample.system_ns, __ATOMIC_RELAXED),
    };
    cputime_adjust(sched_thread_run_ns(t, clock_now_ns()), &sample, &t->cpu_reported);
    *self = t->cpu_reported;
}

bool process_cputimer_set(struct process *p, unsigned which, uint64_t value_ns, uint64_t interval_ns,
                          uint64_t *old_value, uint64_t *old_interval)
{
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    bool ok = cputime_itimer_set(&p->cputime, which, value_ns, interval_ns, TICK_NS, old_value, old_interval);
    spin_unlock_irqrestore(&p->lock, s);
    return ok;
}

bool process_cputimer_get(struct process *p, unsigned which, uint64_t *value, uint64_t *interval)
{
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    bool ok = cputime_itimer_get(&p->cputime, which, TICK_NS, value, interval);
    spin_unlock_irqrestore(&p->lock, s);
    return ok;
}
