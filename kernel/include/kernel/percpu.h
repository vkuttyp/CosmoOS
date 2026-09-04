/*
 * percpu.h - Per-CPU kernel state.
 *
 * One `struct percpu` per logical CPU, reached through the architecture's
 * fast per-CPU pointer (GS base on x86-64). The boot CPU's instance is
 * static and installed before anything else runs; additional CPUs get
 * theirs during SMP bring-up.
 *
 * Fields are written only by their own CPU except where noted
 * (need_resched is set by other CPUs under that CPU's run-queue lock).
 * Architecture-specific per-CPU data lives in the arch layer's own
 * arrays indexed by cpu_id, keeping this header architecture-neutral.
 */

#ifndef KERNEL_PERCPU_H
#define KERNEL_PERCPU_H

#include <kernel/compiler.h>

#include <arch/percpu.h>

#define CONFIG_MAX_CPUS 64u

typedef uint64_t cpumask_t;
#define CPUMASK_ALL   (~(cpumask_t)0)
#define CPUMASK_OF(c) ((cpumask_t)1 << (c))

STATIC_ASSERT(CONFIG_MAX_CPUS <= 64, "cpumask_t is 64 bits");

struct thread;
struct runqueue;
struct timer_queue;

struct percpu {
    struct percpu *self;        /* must stay first: arch fast path reads offset 0 */
    unsigned cpu_id;
    struct thread *current;
    struct thread *idle;
    int preempt_count;          /* > 0: preemption disabled on this CPU */
    bool need_resched;          /* set by tick/wake; consumed by schedule() */
    bool online;
    unsigned irq_depth;         /* > 0: executing an interrupt handler */
    struct runqueue *rq;
    struct timer_queue *timers;
    uint64_t ticks;             /* local timer ticks since this CPU started */
    uint64_t irq_count;         /* interrupts handled on this CPU */
};

/* Set up and install the boot CPU's instance. First call in arch start. */
void percpu_init_boot(void);

/* Register a CPU's instance (SMP bring-up). Index must be < CONFIG_MAX_CPUS. */
void percpu_register(struct percpu *pc, unsigned cpu_id);

static inline struct percpu *this_cpu(void)
{
    return arch_percpu_get();
}

/* Instance for CPU `cpu`, or NULL if never registered. */
struct percpu *percpu_get(unsigned cpu);

/* Number of registered CPUs (online or coming up). */
unsigned cpu_count(void);
bool cpu_online(unsigned cpu);
cpumask_t cpu_online_mask(void);

/* Preemption control. Nestable. preempt_enable may reschedule when the
 * count reaches zero, interrupts are enabled, and a reschedule is
 * pending; it never reschedules from interrupt context. */
static inline void preempt_disable(void)
{
    this_cpu()->preempt_count++;
    barrier();
}

void preempt_enable(void);

static inline bool preemptible(void)
{
    struct percpu *pc = this_cpu();
    return pc->preempt_count == 0 && pc->irq_depth == 0;
}

#endif /* KERNEL_PERCPU_H */
