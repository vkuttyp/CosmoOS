/*
 * percpu.c - Per-CPU registry and preemption control.
 */

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>

#include <arch/cpu.h>
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

/* --- the per-CPU claim check (S25) ---------------------------------------- */

#if CONFIG_DEBUG
static int g_claim_expect;          /* one-shot: the next violation is counted, not fatal */
static unsigned g_claim_expected_hits;
static bool g_claim_off;            /* the report is in progress: no recursion into it */
#if CONFIG_PERCPU_WARN
/* The sweep's form: every violating site once, the boot goes on. */
static uintptr_t g_claim_sites[512];
static unsigned g_claim_nsites;
static bool g_claim_lock;
#endif

/*
 * Every read here is raw: the check must not check. The predicate is
 * the rule in percpu.h, plus "the scheduler has not started" and "one
 * CPU", where no answer can be wrong.
 */
static void claim_check(uintptr_t ip)
{
    struct percpu *pc = arch_percpu_get();
    if (pc->preempt_count != 0 || pc->irq_depth != 0 || !arch_irq_enabled())
        return;
    struct thread *cur = pc->current;
    if (cur == NULL || g_cpu_count < 2 || g_claim_off)
        return;
    if ((cur->flags & THREAD_FLAG_IDLE) != 0)
        return;
    if (__builtin_popcountll(cur->affinity) <= 1)
        return;
    if (g_claim_expect) {
        g_claim_expect = 0;
        g_claim_expected_hits++;
        return;
    }
#if CONFIG_PERCPU_WARN
    pc->preempt_count++;
    while (__atomic_test_and_set(&g_claim_lock, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    bool seen = false;
    for (unsigned i = 0; i < g_claim_nsites && i < 512; i++)
        if (g_claim_sites[i] == ip)
            seen = true;
    if (!seen && g_claim_nsites < 512)
        g_claim_sites[g_claim_nsites++] = ip;
    __atomic_clear(&g_claim_lock, __ATOMIC_RELEASE);
    pc->preempt_count--;
    if (!seen)
        kwarn("PERCPU-CLAIM ip=%p thread=%s", (void *)ip, cur->name);
#else
    g_claim_off = true;
    panic("percpu: a per-CPU answer read where the thread could move (ip %p, thread '%s'): preemption on, "
          "interrupts on, affinity 0x%llx -- docs/kernel/scheduler/invariants.md S25",
          (void *)ip, cur->name, (unsigned long long)cur->affinity);
#endif
}

struct percpu *percpu_checked(void)
{
    claim_check((uintptr_t)__builtin_return_address(0));
    return arch_percpu_get();
}

unsigned arch_cpu_id(void)
{
    claim_check((uintptr_t)__builtin_return_address(0));
    return arch_cpu_id_raw();
}

void percpu_claim_expect(void)
{
    g_claim_expect = 1;
}

unsigned percpu_claim_expected_hits(void)
{
    return g_claim_expected_hits;
}
#else
unsigned arch_cpu_id(void)
{
    return arch_cpu_id_raw();
}
#endif

void preempt_enable(void)
{
    struct percpu *pc = raw_this_cpu();   /* the barrier itself */
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
    struct percpu *pc = raw_this_cpu();   /* identity: the predicate below is false on any CPU a movable thread runs on */
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
EXPORT_SYMBOL(arch_cpu_id);   /* a multi-queue driver picks the queue of the CPU it runs on: checked like the kernel's own */
