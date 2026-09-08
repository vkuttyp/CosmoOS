/*
 * signal.c - The signal core (docs/kernel/process/design.md §11).
 *
 * Sending queues a bit on a thread or on the process and wakes a target;
 * delivery happens only when the receiving thread returns to user mode,
 * where the personality builds the handler frame. Defaults: terminate
 * (through process_kill, which the kill flag already delivered at the
 * same points), or ignore. Every set is under the process lock.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>
#include <arch/irq.h>
#include <arch/user.h>

#define UNBLOCKABLE (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP))

int signal_default_is_ignore(int sig)
{
    switch (sig) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    /* SIGCONT resumes a stopped process and then does nothing more:
     * route_locked performs the continue before it consults this table,
     * so what is left for the default action to do is nothing. Leaving
     * it out here would make an uncaught SIGCONT *terminate* the process
     * it had just continued. */
    case SIGCONT:
        return 1;
    default:
        return 0;
    }
}

/* The stop signals. Their default is to stop the process, which is a
 * third outcome beside terminate and ignore (audit finding #30, closed
 * by the job-control unit). SIGSTOP additionally cannot be caught or
 * blocked, which UNBLOCKABLE enforces above. */
int signal_default_is_stop(int sig)
{
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

int signal_process_init(struct process *p)
{
    p->sigactions = kzalloc((size_t)SIG_MAX * sizeof(struct sigaction_k));
    if (p->sigactions == NULL)
        return -ENOMEM;
    p->sig_shared_pending = 0;
    return 0;
}

void signal_process_release(struct process *p)
{
    kfree(p->sigactions);
    p->sigactions = NULL;
}

/* p->lock held. */
static const struct sigaction_k *action_locked(struct process *p, int sig)
{
    return &p->sigactions[sig - 1];
}

int signal_set_action(struct process *p, int sig, const struct sigaction_k *act, struct sigaction_k *old)
{
    if (sig < 1 || sig > SIG_MAX)
        return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP))
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (old)
        *old = p->sigactions[sig - 1];
    if (act)
        p->sigactions[sig - 1] = *act;
    spin_unlock_irqrestore(&p->lock, s);
    return 0;
}

void signal_get_action(struct process *p, int sig, struct sigaction_k *out)
{
    memset(out, 0, sizeof(*out));
    if (sig < 1 || sig > SIG_MAX)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    *out = p->sigactions[sig - 1];
    spin_unlock_irqrestore(&p->lock, s);
}

uint64_t signal_blocked(void)
{
    return thread_current()->sig_blocked;
}

/* p->lock held: a default-terminate signal pending on the process that a
 * thread no longer blocks terminates the process now. */
static void recheck_defaults_locked(struct process *p, struct thread *t)
{
    uint64_t cand = p->sig_shared_pending & ~t->sig_blocked;
    while (cand) {
        int sig = __builtin_ctzll(cand) + 1;
        cand &= cand - 1;
        const struct sigaction_k *a = action_locked(p, sig);
        if (a->handler == SIG_DFL && !signal_default_is_ignore(sig)) {
            p->sig_shared_pending &= ~SIGMASK(sig);
            if (p->state == PROCESS_RUNNING && p->kill_sig == 0) {
                p->kill_sig = sig;
                p->exit_status = 128 + sig;
                struct thread *o;
                list_for_each_entry(o, &p->threads, proc_link)
                    sched_wake(o);
            }
        }
    }
}

void signal_set_blocked(uint64_t mask)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    t->sig_blocked = mask & ~UNBLOCKABLE;
    recheck_defaults_locked(p, t);
    spin_unlock_irqrestore(&p->lock, s);
}

void signal_set_blocked_saved(uint64_t saved)
{
    struct thread *t = thread_current();
    t->sig_saved_blocked = saved;
    t->sig_restore_blocked = true;
}

uint64_t signal_pending_set(void)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    uint64_t v = t->sig_pending | p->sig_shared_pending;
    spin_unlock_irqrestore(&p->lock, s);
    return v;
}

/* --- sending -------------------------------------------------------------------- */

static void fill_info(struct signal_info *slot, int sig, const struct signal_info *info)
{
    if (info) {
        *slot = *info;
        slot->sig = sig;
    } else {
        memset(slot, 0, sizeof(*slot));
        slot->sig = sig;
        slot->source = SIGSRC_KERNEL;
    }
}

/*
 * p->lock held. Decide what a signal does to `p` when sent to the process
 * or to `t`: returns true when it was consumed here (ignored, or turned
 * into a process termination), false when it was queued for delivery.
 */
/* p->lock held. Every stop signal off every set: a continue cancels
 * stops that have not been taken yet, and a stop cancels a continue. */
static void drop_pending_locked(struct process *p, uint64_t mask)
{
    p->sig_shared_pending &= ~mask;
    struct thread *o;
    list_for_each_entry(o, &p->threads, proc_link)
        o->sig_pending &= ~mask;
}

#define STOP_SIGNALS (SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) | SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU))

static bool route_locked(struct process *p, struct thread *t, int sig, const struct signal_info *info,
                         bool *woke_stopped)
{
    if (p->state != PROCESS_RUNNING)
        return true;
    if (sig == SIGKILL) {
        /* A stopped process is still killable, and needs nothing extra
         * here to be: the parked threads wait on `kill_sig` as well as
         * on `stopped` (process_stop_park), and the wake below is what
         * ends that wait. An explicit un-stop was written here first and
         * removed again, because no test could tell it from its absence
         * -- which is the honest sign that it did nothing. */
        if (p->kill_sig == 0) {
            p->kill_sig = sig;
            p->exit_status = 128 + sig;
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link)
                sched_wake(o);
        }
        return true;
    }
    /*
     * Continue first, and whatever the action is: SIGCONT resumes a
     * stopped process even when a handler is installed for it, and the
     * handler then runs on top. It also throws away stop signals that
     * were posted and not yet taken -- otherwise the process would
     * resume and immediately stop again on a stale one.
     */
    if (sig == SIGCONT) {
        drop_pending_locked(p, STOP_SIGNALS);
        if (p->stopped) {
            p->stopped = false;
            p->stop_sig = 0;
            p->stop_reportable = false;
            p->cont_reportable = true;
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link)
                o->sig_must_stop = false;
            *woke_stopped = true;
        }
    }
    const struct sigaction_k *a = action_locked(p, sig);
    if (a->handler == SIG_IGN || (a->handler == SIG_DFL && signal_default_is_ignore(sig)))
        return true;   /* discarded, blocked or not (a recorded deviation: Linux keeps a blocked one) */
    /*
     * The stop signals' default. The process stops as a unit: its own
     * flag is the authority, and every thread gets a reason to reach a
     * return to user mode and look at it. A stop signal with a handler
     * installed (SIGTSTP and friends may have one; SIGSTOP may not) is
     * queued like any other and runs the handler instead.
     */
    if (a->handler == SIG_DFL && signal_default_is_stop(sig)) {
        /* SIGSTOP cannot be blocked (UNBLOCKABLE), but SIGTSTP and the
         * terminal pair can: a process that blocks one has said it does
         * not want to be stopped by it, and the signal waits until it
         * unblocks rather than stopping it now. */
        bool blocked_everywhere = true;
        if (t) {
            blocked_everywhere = (t->sig_blocked & SIGMASK(sig)) != 0;
        } else {
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link)
                if (!(o->sig_blocked & SIGMASK(sig)))
                    blocked_everywhere = false;
        }
        if (blocked_everywhere)
            goto queue;   /* stays pending until one of them unblocks */
        drop_pending_locked(p, SIGMASK(SIGCONT));
        if (!p->stopped && p->kill_sig == 0) {
            p->stopped = true;
            p->stop_sig = sig;
            p->nr_stopped = 0;
            p->cont_reportable = false;
            /* Not `stop_reportable` here: the process is not stopped
             * until its last thread has parked, and the thread that
             * parks last is what says so (process_stop_park). Setting
             * it at post time lets a parent reclaim the terminal while
             * a thread of the job is still running, and lets the same
             * stop be reported twice -- once early, once again when
             * the last thread arrives and sets it for real. */
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link) {
                o->sig_must_stop = true;
                sched_wake(o);
            }
        }
        return true;
    }
    if (a->handler == SIG_DFL) {
        /* Default: terminate, as soon as some thread would take it. */
        bool blocked_everywhere = true;
        if (t) {
            blocked_everywhere = (t->sig_blocked & SIGMASK(sig)) != 0;
        } else {
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link)
                if (!(o->sig_blocked & SIGMASK(sig)))
                    blocked_everywhere = false;
        }
        if (!blocked_everywhere) {
            if (p->kill_sig == 0) {
                p->kill_sig = sig;
                p->exit_status = 128 + sig;
                struct thread *o;
                list_for_each_entry(o, &p->threads, proc_link)
                    sched_wake(o);
            }
            return true;
        }
        /* Blocked by every candidate: stays pending until one unblocks. */
    }
queue:
    if (t) {
        t->sig_pending |= SIGMASK(sig);
        fill_info(&t->sig_info[sig - 1], sig, info);
        sched_wake(t);
    } else {
        p->sig_shared_pending |= SIGMASK(sig);
        fill_info(&p->sig_shared_info[sig - 1], sig, info);
        /* Wake a thread that can take it (the main thread first). */
        struct thread *o;
        list_for_each_entry(o, &p->threads, proc_link) {
            if (!(o->sig_blocked & SIGMASK(sig))) {
                sched_wake(o);
                break;
            }
        }
    }
    return false;
}

/*
 * What a stop or a continue needs doing once p->lock is free: waking the
 * parked threads, and telling the parent an event happened. Neither can
 * be done under the lock -- the first takes a wait queue's lock and the
 * second sends a signal to another process.
 */
static void signal_after_route(struct process *p, bool woke_stopped, bool stopped_now)
{
    if (woke_stopped)
        waitqueue_wake_all(&p->stopped_wq);
    if (woke_stopped || stopped_now)
        process_notify_parent_event(p);
}

/*
 * Raise a stop signal on the calling process, and say whether anything
 * will come of it: false when the signal is ignored or blocked, in
 * which case the process will neither stop nor run a handler and the
 * caller must not pretend otherwise. The terminal turns a false into
 * `-EIO`, which is POSIX's answer for "this cannot be stopped".
 *
 * The test and the send are one critical section on purpose. Asking
 * first and sending afterwards is a race a sibling thread can win by
 * changing the action in between, and the loser is the reader: the
 * signal is discarded and the read returns `-EINTR` for a stop that
 * will never happen, which a retrying program retries for ever.
 */
bool signal_raise_stop_self(int sig, const struct signal_info *info)
{
    struct thread *t = thread_current();
    struct process *p = t ? t->proc : NULL;
    if (p == NULL || sig < 1 || sig > SIG_MAX)
        return false;
    bool woke_stopped = false;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    bool ignored = p->sigactions[sig - 1].handler == SIG_IGN || (t->sig_blocked & SIGMASK(sig)) != 0;
    if (!ignored)
        route_locked(p, NULL, sig, info, &woke_stopped);
    bool stopped_now = p->stopped;
    spin_unlock_irqrestore(&p->lock, s);
    if (!ignored)
        signal_after_route(p, woke_stopped, stopped_now);
    return !ignored;
}

int signal_send(struct process *p, int sig, const struct signal_info *info)
{
    if (sig < 1 || sig > SIG_MAX)
        return -EINVAL;
    bool woke_stopped = false;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    route_locked(p, NULL, sig, info, &woke_stopped);
    bool stopped_now = p->stopped;
    spin_unlock_irqrestore(&p->lock, s);
    signal_after_route(p, woke_stopped, stopped_now);
    return 0;
}

int signal_send_thread(struct thread *t, int sig, const struct signal_info *info)
{
    if (sig < 1 || sig > SIG_MAX)
        return -EINVAL;
    struct process *p = t->proc;
    if (p == NULL)
        return -ESRCH;
    bool woke_stopped = false;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    route_locked(p, t, sig, info, &woke_stopped);
    bool stopped_now = p->stopped;
    spin_unlock_irqrestore(&p->lock, s);
    signal_after_route(p, woke_stopped, stopped_now);
    return 0;
}

/* --- delivery ------------------------------------------------------------------- */

bool signal_pending(void)
{
    struct thread *t = thread_current();
    struct process *p = t ? t->proc : NULL;
    if (p == NULL)
        return false;
    if (__atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != PROCESS_RUNNING)
        return true;
    /* A stop this thread has not parked for yet. Without this a sibling
     * woken by process_stop re-evaluates its wait, finds the shared
     * pending bit already taken by whichever thread dequeued the stop,
     * and blocks again -- so the process would never fully stop. */
    if (__atomic_load_n(&t->sig_must_stop, __ATOMIC_ACQUIRE))
        return true;
    uint64_t pend = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE) | __atomic_load_n(&p->sig_shared_pending, __ATOMIC_ACQUIRE);
    return (pend & ~t->sig_blocked) != 0;
}

/* p->lock held. The lowest deliverable signal, taken off its set, or 0. */
static int dequeue_locked(struct process *p, struct thread *t, struct signal_info *info)
{
    uint64_t cand = (t->sig_pending | p->sig_shared_pending) & ~t->sig_blocked;
    if (cand == 0)
        return 0;
    /* Synchronous faults first: they name the interrupted instruction. */
    uint64_t sync = cand & (SIGMASK(SIGSEGV) | SIGMASK(SIGBUS) | SIGMASK(SIGILL) | SIGMASK(SIGFPE) | SIGMASK(SIGTRAP));
    int sig = __builtin_ctzll(sync ? sync : cand) + 1;
    if (t->sig_pending & SIGMASK(sig)) {
        t->sig_pending &= ~SIGMASK(sig);
        *info = t->sig_info[sig - 1];
    } else {
        p->sig_shared_pending &= ~SIGMASK(sig);
        *info = p->sig_shared_info[sig - 1];
    }
    return sig;
}

static void terminate(struct process *p, int status)
{
    arch_irq_enable();   /* the trap tail runs with interrupts off; exiting needs a preemptible context */
    (void)p;
    process_exit(status);
}

void signal_deliver(void *frame, bool is_syscall)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    if (p == NULL)
        return;
    for (;;) {
        if (__atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != PROCESS_RUNNING)
            terminate(p, p->exit_status);
        /*
         * Park here if the process is stopped. This is the only place a
         * thread stops, and it re-reads the process's own state rather
         * than trusting the flag that got it here -- a SIGCONT may have
         * ended the stop while this thread was inside a wait it could
         * not be interrupted from, and parking on that stale flag would
         * stop a process nothing is left to continue.
         *
         * A system call cut short by the stop is restarted, not failed:
         * `^Z` and then `fg` must not turn a blocked `read` into
         * -EINTR. That is unconditional here, because a stop has no
         * action to carry SA_RESTART.
         */
        if (__atomic_load_n(&t->sig_must_stop, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&p->stopped, __ATOMIC_ACQUIRE)) {
            bool irqs_were_on = arch_irq_enabled();
            arch_irq_enable();   /* parking blocks; the trap tail runs with interrupts off */
            bool parked = process_stop_park();
            if (!irqs_were_on)
                arch_irq_disable();
            if (parked && is_syscall && t->syscall_nr != SIGNAL_NO_RESTART) {
                struct arch_user_regs regs;
                arch_user_regs_from_syscall(frame, &regs);
                if (arch_user_regs_result(&regs) == -EINTR) {
                    arch_user_regs_restart_syscall(&regs, t->syscall_nr, t->syscall_arg0);
                    arch_user_regs_to_syscall(frame, &regs);
                }
            }
            continue;   /* everything may have changed: a kill, another stop, a signal */
        }
        struct signal_info info;
        struct sigaction_k act;
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        int sig = dequeue_locked(p, t, &info);
        if (sig)
            act = p->sigactions[sig - 1];
        spin_unlock_irqrestore(&p->lock, s);
        if (sig == 0) {
            if (t->sig_restore_blocked) {   /* a temporary mask (rt_sigsuspend, ppoll) with no handler to run */
                t->sig_restore_blocked = false;
                signal_set_blocked(t->sig_saved_blocked);
            }
            return;
        }
        if (act.handler == SIG_IGN || (act.handler == SIG_DFL && signal_default_is_ignore(sig)))
            continue;
        if (act.handler == SIG_DFL || p->pers->signal_frame == NULL)
            terminate(p, 128 + sig);   /* the default, or a personality without handlers */

        /* A handler: build the frame on a copy of the registers. */
        struct arch_user_regs regs;
        if (is_syscall)
            arch_user_regs_from_syscall(frame, &regs);
        else
            arch_user_regs_from_trap(frame, &regs);
        if (is_syscall && t->syscall_nr != SIGNAL_NO_RESTART && arch_user_regs_result(&regs) == -EINTR &&
            (act.flags & SA_RESTART))
            arch_user_regs_restart_syscall(&regs, t->syscall_nr, t->syscall_arg0);
        uint64_t blocked_before = t->sig_restore_blocked ? t->sig_saved_blocked : t->sig_blocked;
        t->sig_restore_blocked = false;
        bool irqs_were_on = arch_irq_enabled();
        arch_irq_enable();   /* the frame is written to user memory (demand faults, copies) */
        int rc = p->pers->signal_frame(&regs, &act, &info, blocked_before);
        if (!irqs_were_on)
            arch_irq_disable();
        if (rc) {
            kwarn("process: pid %u '%s': cannot deliver signal %d (%d); terminating", p->pid, p->name, sig, rc);
            terminate(p, 128 + SIGSEGV);
        }
        arch_user_regs_sanitize(&regs);   /* a handler address the return path could not load */
        s = spin_lock_irqsave(&p->lock);
        t->sig_blocked |= act.mask & ~UNBLOCKABLE;
        if (!(act.flags & SA_NODEFER))
            t->sig_blocked |= SIGMASK(sig) & ~UNBLOCKABLE;
        if (act.flags & SA_RESETHAND)
            memset(&p->sigactions[sig - 1], 0, sizeof(struct sigaction_k));
        spin_unlock_irqrestore(&p->lock, s);
        if (is_syscall)
            arch_user_regs_to_syscall(frame, &regs);
        else
            arch_user_regs_to_trap(frame, &regs);
        return;   /* one handler per return; the next runs after this one returns */
    }
}

void signal_fault(int sig, uint64_t addr, struct arch_trap_frame *frame)
{
    struct signal_info info = { .sig = sig, .source = SIGSRC_FAULT, .fault_addr = addr, .code = 1 };
    signal_fault_info(&info, frame);
}

void signal_fault_info(const struct signal_info *infop, struct arch_trap_frame *frame)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    KASSERT(p != NULL);
    struct signal_info info = *infop;
    int sig = info.sig;
    uint64_t addr = info.fault_addr;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    const struct sigaction_k *a = action_locked(p, sig);
    bool handled = a->handler != SIG_DFL && a->handler != SIG_IGN && !(t->sig_blocked & SIGMASK(sig)) &&
                   p->pers->signal_frame != NULL;
    if (handled) {
        t->sig_pending |= SIGMASK(sig);
        t->sig_info[sig - 1] = info;
    }
    spin_unlock_irqrestore(&p->lock, s);
    if (!handled) {
        kwarn("process: pid %u '%s' fault: signal %d at %p; terminating", p->pid, p->name, sig,
              (void *)(uintptr_t)addr);
        terminate(p, 128 + sig);
    }
    signal_deliver(frame, false);   /* sets the handler frame up; returns into it */
}

void signal_return(void *syscall_frame, const struct arch_user_regs *regs, uint64_t blocked)
{
    struct arch_user_regs r = *regs;
    arch_user_regs_sanitize(&r);
    signal_set_blocked(blocked);
    arch_user_regs_to_syscall(syscall_frame, &r);
}

int signal_wait(void)
{
    struct waitqueue wq;
    waitqueue_init(&wq, "sigsuspend");
    int rc = wait_event_killable(&wq, signal_pending());
    return rc ? rc : -EINTR;
}
