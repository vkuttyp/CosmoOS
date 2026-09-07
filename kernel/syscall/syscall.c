/*
 * syscall.c - Generic system-call dispatcher.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <arch/user.h>

#include <arch/irq.h>

static uint64_t g_calls;
static uint64_t g_unknown;
static uint64_t g_filtered;

/*
 * Whether this process may make this call. A number past the mask is
 * denied rather than allowed: a program built against a smaller system
 * call count must deny what it has never heard of, which is the
 * direction that fails safe.
 *
 * The always-allowed set is the personality's, because it owns the
 * numbering: without it a filter that forgot `exit` would turn every
 * clean shutdown into a signal death, and one that forgot
 * `rt_sigreturn` would make the first signal fatal for a reason
 * unrelated to the filter.
 */
bool syscall_allowed(const struct process *p, uint64_t nr)
{
    if (nr >= COSMO_SYSCALL_MASK_WORDS * 64)
        return false;
    if (p->syscall_mask[nr / 64] & (1ull << (nr % 64)))
        return true;
    for (unsigned i = 0; i < p->pers->nr_always_allowed; i++)
        if (p->pers->always_allowed[i] == nr)
            return true;
    return false;
}

/* Narrowing only: the new mask is the intersection with what is in
 * force, so no sequence of calls widens what a process may do. */
void syscall_filter_install(struct process *p, const uint64_t *mask, unsigned words)
{
    for (unsigned i = 0; i < COSMO_SYSCALL_MASK_WORDS; i++)
        p->syscall_mask[i] &= i < words ? mask[i] : 0;
}

uint64_t syscall_filtered_count(void)
{
    return __atomic_load_n(&g_filtered, __ATOMIC_RELAXED);
}

int64_t syscall_dispatch(uint64_t nr, const uint64_t args[6], void *frame)
{
    struct percpu *pc = this_cpu();
    struct process *p = process_current();

    KASSERT(arch_irq_enabled());
    KASSERT(pc->irq_depth == 0 && pc->preempt_count == 0);
    if (p == NULL)
        panic("system call %llu from a thread without a process", (unsigned long long)nr);

    __atomic_fetch_add(&g_calls, 1u, __ATOMIC_RELAXED);
    p->syscalls++;

    const struct personality *pers = p->pers;
    if (nr >= pers->count || pers->table[nr] == NULL) {
        __atomic_fetch_add(&g_unknown, 1u, __ATOMIC_RELAXED);
        kdebug("syscall: pid %u unknown number %llu (%s)", p->pid, (unsigned long long)nr, pers->name);
        return -ENOSYS;
    }

    /*
     * The filter (docs/kernel/security/design.md §1f), asked only of
     * calls that exist. A filter takes away authority the process would
     * otherwise have; it does not invent a death for a call nobody can
     * make, and a number the kernel does not implement answers -ENOSYS
     * whether or not a filter is installed -- installing nothing must
     * change nothing.
     *
     * The mask is written only by this process through its own system
     * call, so reading it here needs no lock.
     */
    if (!syscall_allowed(p, nr)) {
        __atomic_fetch_add(&g_filtered, 1u, __ATOMIC_RELAXED);
        kwarn("syscall: pid %u killed: number %llu is outside its filter", p->pid, (unsigned long long)nr);
        process_kill(p, COSMO_SIGSYS);
        process_check_kill();
        return -EPERM;   /* not reached: process_check_kill does not return here */
    }

    struct syscall_args a = {
        .nr = nr,
        .a = { args[0], args[1], args[2], args[3], args[4], args[5] },
        .frame = frame,
    };
    struct thread *t = thread_current();
    t->syscall_nr = nr;
    t->syscall_arg0 = args[0];
    process_check_kill();
    int64_t rc = pers->table[nr](&a);
    /* A signal that arrived (or a kill, or an exiting process) is acted
     * on here, on the system-call frame: a handler frame, a restart of an
     * interrupted call, or the end of the process. The result register is
     * set first so the frame the handler returns into carries it. */
    if (signal_pending()) {
        arch_user_regs_set_result_in_frame(frame, rc);
        signal_deliver(frame, true);
        rc = arch_user_regs_result_in_frame(frame);
    }
    return rc;
}

uint64_t syscall_count(void)
{
    return __atomic_load_n(&g_calls, __ATOMIC_RELAXED);
}

uint64_t syscall_unknown_count(void)
{
    return __atomic_load_n(&g_unknown, __ATOMIC_RELAXED);
}
