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
    case SIGCONT:
    /* No job control: the stop signals cannot stop anything and must not
     * kill anything (audit finding #30); they are ignored, a recorded
     * deviation from Linux. */
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return 1;
    default:
        return 0;
    }
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
static bool route_locked(struct process *p, struct thread *t, int sig, const struct signal_info *info)
{
    if (p->state != PROCESS_RUNNING)
        return true;
    if (sig == SIGKILL) {
        if (p->kill_sig == 0) {
            p->kill_sig = sig;
            p->exit_status = 128 + sig;
            struct thread *o;
            list_for_each_entry(o, &p->threads, proc_link)
                sched_wake(o);
        }
        return true;
    }
    const struct sigaction_k *a = action_locked(p, sig);
    if (a->handler == SIG_IGN || (a->handler == SIG_DFL && signal_default_is_ignore(sig)))
        return true;   /* discarded, blocked or not (a recorded deviation: Linux keeps a blocked one) */
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

int signal_send(struct process *p, int sig, const struct signal_info *info)
{
    if (sig < 1 || sig > SIG_MAX)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    route_locked(p, NULL, sig, info);
    spin_unlock_irqrestore(&p->lock, s);
    return 0;
}

int signal_send_thread(struct thread *t, int sig, const struct signal_info *info)
{
    if (sig < 1 || sig > SIG_MAX)
        return -EINVAL;
    struct process *p = t->proc;
    if (p == NULL)
        return -ESRCH;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    route_locked(p, t, sig, info);
    spin_unlock_irqrestore(&p->lock, s);
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
