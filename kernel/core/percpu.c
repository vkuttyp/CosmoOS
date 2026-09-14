/*
 * percpu.c - Per-CPU registry and preemption control.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <arch/irq.h>
#include <arch/percpu.h>

static struct percpu g_boot_percpu;
static struct percpu *g_cpus[CONFIG_MAX_CPUS];
static unsigned g_cpu_count;

void percpu_init_boot(void)
{
    memset(&g_boot_percpu, 0, sizeof(g_boot_percpu));
    g_boot_percpu.cpu_id = 0;
    g_boot_percpu.online = true;
    g_cpus[0] = &g_boot_percpu;
    g_cpu_count = 1;
    arch_percpu_install(&g_boot_percpu);
}

void percpu_register(struct percpu *pc, unsigned cpu_id)
{
    KASSERT(cpu_id < CONFIG_MAX_CPUS);
    KASSERT(g_cpus[cpu_id] == NULL);
    pc->cpu_id = cpu_id;
    g_cpus[cpu_id] = pc;
    if (cpu_id >= g_cpu_count)
        g_cpu_count = cpu_id + 1;
}

struct percpu *percpu_get(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? g_cpus[cpu] : NULL;
}

unsigned cpu_count(void)
{
    return g_cpu_count;
}

bool cpu_online(unsigned cpu)
{
    struct percpu *pc = percpu_get(cpu);
    return pc != NULL && __atomic_load_n(&pc->online, __ATOMIC_ACQUIRE);
}

cpumask_t cpu_online_mask(void)
{
    cpumask_t m = 0;
    for (unsigned i = 0; i < g_cpu_count; i++) {
        if (cpu_online(i))
            m |= CPUMASK_OF(i);
    }
    return m;
}

void preempt_enable(void)
{
    struct percpu *pc = this_cpu();
    KASSERT(pc->preempt_count > 0);
    barrier();
    pc->preempt_count--;
    if (pc->preempt_count == 0 && pc->need_resched && pc->irq_depth == 0 && arch_irq_enabled())
        sched_preempt();
}

/*
 * Every wake in this kernel happens under an interrupt-disabling
 * spinlock, and `spin_unlock_irqrestore` runs `preempt_enable` while
 * interrupts are still off -- so the test above never fires for a
 * same-CPU wake, and the woken thread used to run at the next tick.
 * The restore of interrupts is the moment the last condition becomes
 * true, and `arch_irq_restore` calls here after enabling them
 * (docs/audit/next-subsystem-wake-preempt.md).
 *
 * The two internal callers of `arch_irq_restore` pass through unharmed
 * by the predicate alone: the lockdep bracket inside `spin_unlock`
 * restores while the lock's `preempt_disable` still holds (count > 0),
 * and the tail of `schedule()` restores its caller's state with the
 * count at zero, where a pending reschedule means one more trip through
 * `schedule()` -- the same thing a tick landing there would do.
 */
static uint64_t g_restore_preempts[CONFIG_MAX_CPUS];

void preempt_point(void)
{
    struct percpu *pc = this_cpu();
    if (pc->preempt_count == 0 && pc->need_resched && pc->irq_depth == 0 && arch_irq_enabled()) {
        /* This CPU's word, written only here; atomic because the scheduler
         * dump reads every CPU's from wherever it runs. */
        __atomic_fetch_add(&g_restore_preempts[pc->cpu_id], 1, __ATOMIC_RELAXED);
        sched_preempt();
    }
}

uint64_t preempt_point_count(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? __atomic_load_n(&g_restore_preempts[cpu], __ATOMIC_RELAXED) : 0;
}

/* Module ABI exports (docs/kernel/module/api.md): a multi-queue driver
 * sizes its queues by the CPU count. */
#include <kernel/module.h>
EXPORT_SYMBOL(cpu_count);
