/*
 * mutex_floor.c - The C under kernel/scheduler/mutex.nova (roadmap 2.0 SA3).
 *
 * The mutex and its priority inheritance are Nova (mutex.nova, generated
 * into mutex.c and kernel/include/kernel/mutex_types.h). What stays here is
 * panic's and WARN's formatted messages, the interrupt and might-sleep
 * checks, which read the CPU's fields, and the module exports. It also
 * checks the constants mutex.nova copies.
 */

#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/thread.h>

_Static_assert(LOCKDEP_KIND_MUTEX == 1u && SCHED_PRIO_COUNT == 64, "mutex.nova's constants");

/* Declared for mutex.nova only; not part of mutex.h's interface. */
void mutex_floor_check_lock(const struct mutex *m, uintptr_t ip);
void mutex_floor_recursive(const struct mutex *m, const struct thread *cur);
void mutex_floor_bad_unlock(const struct mutex *m, const struct thread *cur);
void mutex_floor_pi_cycle(void);

/* Never in an interrupt; and, on every acquisition, not only a contended
 * one, never where the caller may not sleep (invariant S6), naming the
 * lock's call site. */
void mutex_floor_check_lock(const struct mutex *m, uintptr_t ip)
{
    struct percpu *pc = raw_this_cpu();   /* identity: zero on any CPU a sleeping caller runs on */
    if (pc->irq_depth != 0)
        panic("mutex_lock('%s') in interrupt context", m->name);
    if (pc->preempt_count != 0) {
        if (CONFIG_LOCKDEP)
            lockdep_might_sleep(ip);
        else
            panic("sleeping call with preemption disabled (count %d) or in interrupt (depth %u)", pc->preempt_count,
                  pc->irq_depth);
    }
}

void mutex_floor_recursive(const struct mutex *m, const struct thread *cur)
{
    panic("mutex_lock('%s'): recursive lock by '%s'", m->name, cur->name);
}

void mutex_floor_bad_unlock(const struct mutex *m, const struct thread *cur)
{
    panic("mutex_unlock('%s') by '%s' but owner is '%s'", m->name, cur->name, m->owner ? m->owner->name : "nobody");
}

void mutex_floor_pi_cycle(void)
{
    WARN(true, "mutex PI: donation chain cycle -- a lock-order violation");
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_lock_nested);
EXPORT_SYMBOL(mutex_trylock);
EXPORT_SYMBOL(mutex_unlock);
