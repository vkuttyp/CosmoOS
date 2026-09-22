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
#include <kernel/lockup_core.h>

#define CONFIG_MAX_CPUS 64u

typedef uint64_t cpumask_t;
#define CPUMASK_ALL   (~(cpumask_t)0)
#define CPUMASK_OF(c) ((cpumask_t)1 << (c))

STATIC_ASSERT(CONFIG_MAX_CPUS <= 64, "cpumask_t is 64 bits");

struct thread;
struct runqueue;
struct timer_queue;

struct vm_space;

struct percpu {
    struct percpu *self;        /* offset 0: arch fast path (x86-64: mov %gs:0; AArch64 reads TPIDR_EL1) */
    uintptr_t kernel_stack_top; /* offset 8: syscall entry loads rsp from here */
    uintptr_t user_rsp_scratch; /* offset 16: syscall entry parks the user rsp here */
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
    uintptr_t boot_stack;       /* AP bootstrap stack; freed by its idle thread */
    struct vm_space *cur_space; /* the space whose root this CPU runs (vm_space_switch) */
    uint32_t hw_id;             /* local interrupt controller id (APIC id) */
    /* The lockup unit (kernel/core/lockup.c, docs/kernel/diagnostics/design.md, "Lockups"). */
    uintptr_t last_tick_pc;     /* the PC this CPU's last tick interrupted ... */
    uint64_t last_tick_ns;      /* ... and when; two stores per tick */
    uint64_t tick_cost_ns;      /* CONFIG_SELFTEST: tick entry to the scheduler hook, accumulated */
    struct cpu_sample sample;   /* this CPU's answer to the last request for its frame */
    uint64_t stall_ns;          /* soft: time this CPU has run one thread while others waited */
    uint64_t last_switches;
    bool soft_reported;
    /* Set by the straggler kick's handler, read and cleared by the trap
     * tail on the way out -- so it is true only inside the one trap the
     * kick caused, and a publish that sees it set happened in that
     * return (docs/kernel/quiesce/invariants.md, Q19). */
    bool quiesce_kicked;
    unsigned watch_target;      /* hard: the CPU this one watches (lockup_watch_target) */
    uint64_t watch_ticks;       /* ... its tick count when last seen to change */
    uint64_t watch_stall_ns;
    bool hard_reported;
};

/* Assembly (syscall entry) relies on these offsets. */
STATIC_ASSERT(offsetof(struct percpu, self) == 0, "percpu.self offset");
STATIC_ASSERT(offsetof(struct percpu, kernel_stack_top) == 8, "percpu.kernel_stack_top offset");
STATIC_ASSERT(offsetof(struct percpu, user_rsp_scratch) == 16, "percpu.user_rsp_scratch offset");

/* Set up and install the boot CPU's instance. First call in arch start. */
void percpu_init_boot(void);

/* Register a CPU's instance (SMP bring-up). Index must be < CONFIG_MAX_CPUS. */
void percpu_register(struct percpu *pc, unsigned cpu_id);

/*
 * Which CPU am I on? Two answers, one rule (docs/kernel/scheduler/
 * invariants.md, S25):
 *
 *   A per-CPU answer may be kept only while the thread cannot move:
 *   preemption disabled, interrupts off, interrupt context, or an
 *   affinity of one CPU. `preempt_disable()` is the migration barrier --
 *   a thread that holds it is never THREAD_READY and is never moved.
 *
 * `this_cpu()` and `arch_cpu_id()` are the checked forms: in debug
 * builds they panic, naming the call site, when the rule does not hold
 * where they are called (kernel/core/percpu.c). `raw_this_cpu()` and
 * `raw_cpu_id()` are the unchecked forms for the two kinds of read the
 * rule does not govern, and every use of them says which in a comment:
 *
 *   - an answer that is the same on any CPU that runs this thread: the
 *     thread itself (`->current`), and `preempt_count`/`irq_depth` read
 *     to assert they are zero, which in a preemptible context they are
 *     everywhere;
 *   - a diagnostic or a statistic, where a stale answer is a wrong
 *     number and never a wrong action.
 *
 * A thread that must keep a per-CPU answer across a sleep is created on
 * that CPU (`thread_create_on` with one bit): the check honours a
 * one-CPU affinity.
 */
static inline struct percpu *raw_this_cpu(void)
{
    return arch_percpu_get();
}

static inline unsigned raw_cpu_id(void)
{
    return raw_this_cpu()->cpu_id;
}

#if CONFIG_DEBUG
/* Out of line so that its return address is the call site it names. */
struct percpu *percpu_checked(void);
static inline struct percpu *this_cpu(void)
{
    return percpu_checked();
}

/* Self-tests: the next violation counts instead of panicking. One-shot. */
void percpu_claim_expect(void);
unsigned percpu_claim_expected_hits(void);
#else
static inline struct percpu *this_cpu(void)
{
    return arch_percpu_get();
}
#endif

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
    raw_this_cpu()->preempt_count++;   /* the barrier itself: raw by definition */
    barrier();
}

void preempt_enable(void);

/* The fourth preemption point (docs/kernel/scheduler/design.md,
 * "Preemption points"): reschedule now if a reschedule is pending and
 * this context may switch -- no spinlock held, not in an interrupt,
 * interrupts enabled. `preempt_enable` tests the same predicate when
 * the count reaches zero; this is for the moment the *other* term
 * becomes true, interrupts coming back on with the count already zero,
 * which is where every wake made under an irqsave lock leaves its
 * waker. Called by each architecture's `arch_irq_restore` after it has
 * enabled interrupts; safe to call anywhere, since every condition that
 * would make a switch wrong is in the predicate. */
void preempt_point(void);
/* How many times `preempt_point` switched on `cpu` (debug diagnostics; the scheduler dump prints it). */
uint64_t preempt_point_count(unsigned cpu);

static inline bool preemptible(void)
{
    /* Raw: both counts are zero on every CPU a preemptible thread can be
     * running on, and non-zero only where it cannot move. */
    struct percpu *pc = raw_this_cpu();
    return pc->preempt_count == 0 && pc->irq_depth == 0;
}

#endif /* KERNEL_PERCPU_H */
