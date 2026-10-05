/*
 * lockdep.c - Held-lock stacks, the dependency graph and the reports
 * (docs/kernel/lockdep/design.md). Enabled by CONFIG_LOCKDEP.
 *
 * The algorithm is lockdep_core.h. This file owns the per-CPU spinlock
 * stacks, the per-thread mutex stacks, the raw lock that serialises graph
 * updates, and the report path. Acquisition tracking takes no tracked
 * lock and allocates nothing. NMI/#MC writers remain unsupported; raw
 * lock re-entry on its owning CPU fails stop rather than waiting forever.
 */

#include <kernel/lockdep.h>

#if CONFIG_LOCKDEP

STATIC_ASSERT(LOCKDEP_MAX_TIMER_PROFILES >= CONFIG_MAX_CPUS,
              "lockdep timer profile table must cover every concurrent CPU callback");

#include <kernel/console.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/thread.h>

#include <arch/cpu.h>
#include <arch/irq.h>

struct lockdep_cpu {
    struct lockdep_held held[LOCKDEP_MAX_HELD];
    unsigned nr_held;
    uint64_t held_seq;
    const void *callback_timer;
    unsigned callback_profile;
};

struct lockdep_timer_lock {
    const void *lock;
    uint16_t node;
};

struct lockdep_timer_profile {
    const void *timer;
    unsigned nr_locks;
    bool overflow;
    struct lockdep_timer_lock locks[LOCKDEP_MAX_TIMER_LOCKS];
};

static struct lockdep_graph g_graph;
static struct lockdep_scratch g_scratch;   /* used under g_raw only */
static struct lockdep_cpu g_cpus[CONFIG_MAX_CPUS];
static struct lockdep_timer_profile g_timer_profiles[LOCKDEP_MAX_TIMER_PROFILES];
#define TIMER_PROFILE_TOMBSTONE ((const void *)(uintptr_t)1)
static struct lockdep_stats g_stats;
static bool g_off;                          /* fatal lockdep report in progress */

/* Self-test expectations. */
static int g_expect[CONFIG_MAX_CPUS];
static bool g_expect_armed[CONFIG_MAX_CPUS];
static unsigned g_expected_hits[CONFIG_MAX_CPUS];

/* The checker's own lock: zero when free, otherwise owning CPU + 1.
 * Publish ownership in the acquisition itself: a separate owner store
 * would leave a window in which an NMI could wait on its interrupted CPU. */
static uint32_t g_raw;

static arch_irq_state_t raw_lock(void)
{
    arch_irq_state_t s = arch_irq_save();
    unsigned cpu = arch_cpu_id();   /* IRQ masking prevents migration */
    uint32_t owner = cpu + 1u;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_raw, &expected, owner, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        if (expected == owner)
            panic("lockdep: graph raw lock re-entry on CPU %u", cpu);
        arch_cpu_relax();
    }
    return s;
}

static void raw_unlock(arch_irq_state_t s)
{
    __atomic_store_n(&g_raw, 0u, __ATOMIC_RELEASE);
    arch_irq_restore(s);
}

/* Share graph serialization so get_stats can capture all counters at one
 * instant. Called outside g_raw, including early-refused acquisitions. */
static void count_acquisition(void)
{
    arch_irq_state_t s = raw_lock();
    g_stats.acquisitions++;
    raw_unlock(s);
}

static const char *const g_kind_names[LOCKDEP_R_COUNT] = {
    [LOCKDEP_R_INVERSION] = "lock-order inversion",
    [LOCKDEP_R_RECURSION] = "recursive acquisition of one lock class",
    [LOCKDEP_R_IRQ] = "IRQ-unsafe use of an interrupt-context lock",
    [LOCKDEP_R_SLEEP] = "sleeping call in atomic context",
    [LOCKDEP_R_OVERFLOW] = "held-lock stack overflow",
    [LOCKDEP_R_UNHELD] = "release of a lock that is not held",
    [LOCKDEP_R_EXIT_HELD] = "thread exit with a mutex held",
    [LOCKDEP_R_IRQ_STATE] = "mismatched irqsave acquisition and restoration",
    [LOCKDEP_R_CALLBACK] = "timer cancellation waits while holding a callback lock",
};

const char *lockdep_report_name(enum lockdep_report_kind kind)
{
    return (unsigned)kind < LOCKDEP_R_COUNT ? g_kind_names[kind] : "?";
}

/* --- held stacks ---------------------------------------------------------- */

static struct lockdep_cpu *my_cpu(void)
{
    return &g_cpus[arch_cpu_id()];
}

/* The thread's mutex stack, or NULL before threads exist. */
static struct thread *me(void)
{
    return raw_this_cpu()->current;   /* identity */
}

static const char *kind_name(unsigned kind)
{
    return kind == LOCKDEP_KIND_MUTEX ? "mutex" : kind == LOCKDEP_KIND_CALLBACK ? "callback" : "spin";
}

static void print_held(const char *who, const struct lockdep_held *h, unsigned n)
{
    kprintf("  held by %s (%u):\n", who, n);
    for (unsigned i = 0; i < n; i++) {
        const struct lock_class *c = &g_graph.classes[lockdep_node_class(h[i].node)];
        kprintf("    [%u] %s '%s'#%u lock %p acquired at %p%s%s%s\n", i,
                kind_name(c->kind), c->name,
                lockdep_node_subclass(h[i].node), h[i].lock, (void *)h[i].ip,
                (h[i].flags & LOCKDEP_HF_IN_IRQ) ? " [irq]" : "",
                (h[i].flags & LOCKDEP_HF_IRQS_ON) ? " [irqs-on]" : "",
                (h[i].flags & LOCKDEP_HF_TRYLOCK) ? " [try]" : "");
    }
}

void lockdep_dump_held(void)
{
    STATIC_ASSERT(LOCKDEP_MAX_HELD_MUTEX <= LOCKDEP_MAX_HELD, "diagnostic buffer fits both stacks");
    struct thread *t = me();
    struct lockdep_held held[LOCKDEP_MAX_HELD];
    unsigned n;
    if (lockdep_snapshot_held_cpu(raw_cpu_id(), held, &n))
        print_held("this CPU", held, n);
    else
        kprintf("  held by this CPU: unavailable (stack busy, changed, or invalid)\n");
    if (t) {
        if (lockdep_snapshot_held_thread(t, held, &n))
            print_held(t->name, held, n);
        else
            kprintf("  held by %s: unavailable (stack busy, changed, or invalid)\n", t->name);
    }
}

/* Another CPU may be stuck or updating its stack. Make one bounded
 * atomic snapshot attempt; never wait on a CPU being diagnosed. */
bool lockdep_snapshot_held_cpu(unsigned cpu, struct lockdep_held *out, unsigned *count)
{
    *count = 0;
    if (cpu >= CONFIG_MAX_CPUS)
        return false;
    struct lockdep_cpu *lc = &g_cpus[cpu];
    return lockdep_core_held_snapshot(&lc->held_seq, lc->held, LOCKDEP_MAX_HELD, &lc->nr_held, out, count);
}

bool lockdep_snapshot_held_thread(const struct thread *t, struct lockdep_held *out, unsigned *count)
{
    *count = 0;
    if (t == NULL)
        return false;
    return lockdep_core_held_snapshot(&t->held_mutex_seq, t->held_mutex, LOCKDEP_MAX_HELD_MUTEX,
                                      &t->nr_held_mutex, out, count);
}

void lockdep_dump_held_cpu(unsigned cpu)
{
    if (cpu >= CONFIG_MAX_CPUS)
        return;
    struct lockdep_held held[LOCKDEP_MAX_HELD];
    unsigned n;
    if (!lockdep_snapshot_held_cpu(cpu, held, &n)) {
        kprintf("  held by cpu %u: unavailable (stack busy, changed, or invalid)\n", cpu);
        return;
    }
    char who[16];
    ksnprintf(who, sizeof(who), "cpu %u", cpu);
    print_held(who, held, n);
}

/* --- reports ---------------------------------------------------------------- */

/* Panics unless the report was expected by a self-test, in which case it
 * returns and the caller proceeds as if the operation were legal. */
static void report(enum lockdep_report_kind kind, const char *name, unsigned subclass, uintptr_t ip, const char *detail,
                   const uint16_t *path, unsigned path_len)
{
    arch_irq_state_t stats_irq = raw_lock();
    g_stats.reports++;
    raw_unlock(stats_irq);
    unsigned cpu = raw_cpu_id();
    int expected = (int)kind;
    bool armed = true;
    if (__atomic_load_n(&g_expect_armed[cpu], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&g_expect[cpu], __ATOMIC_RELAXED) == expected &&
        __atomic_compare_exchange_n(&g_expect_armed[cpu], &armed, false, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&g_expected_hits[cpu], 1u, __ATOMIC_RELAXED);
        kdebug("lockdep: expected report: %s ('%s'#%u at %p)", g_kind_names[kind], name ? name : "-", subclass,
               (void *)ip);
        return;
    }
    __atomic_store_n(&g_off, true, __ATOMIC_RELEASE);
    /* A failure can originate while console.lock is held by this CPU.
     * Enter the existing fatal-output mode before the first print, and
     * freeze CPU identity/held stacks before dumping them. panic() will
     * claim the panic and stop the other CPUs in its normal way. */
    arch_irq_disable();
    console_set_panic_mode();
    struct thread *t = me();
    kprintf("\nlockdep: %s\n", g_kind_names[kind]);
    kprintf("  lock '%s'#%u at %p, CPU %u, thread '%s', irq_depth %u, preempt_count %d\n", name ? name : "-",
            subclass, (void *)ip, raw_cpu_id(), t ? t->name : "(boot)", raw_this_cpu()->irq_depth,
            raw_this_cpu()->preempt_count);   /* a report */
    if (detail)
        kprintf("  %s\n", detail);
    lockdep_dump_held();
    if (path_len) {
        kprintf("  recorded chain that closes the cycle (last 8 nodes at most):");
        for (unsigned i = 0; i < path_len; i++) {
            const struct lock_class *c = &g_graph.classes[lockdep_node_class(path[i])];
            kprintf("%s '%s'#%u", i ? " ->" : "", c->name, lockdep_node_subclass(path[i]));
        }
        kprintf("\n");
    }
    panic("lockdep: %s", g_kind_names[kind]);
}

/* --- acquire / release ------------------------------------------------------ */

/* Classify (cached) and return the node; -1 when the class table is full. */
static int node_of(uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass, uintptr_t ip)
{
    uint16_t cached = __atomic_load_n(class_slot, __ATOMIC_ACQUIRE);
    if (cached == 0) {
        arch_irq_state_t s = raw_lock();
        int c = lockdep_core_class(&g_graph, name, kind);
        raw_unlock(s);
        if (c < 0) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip,
                   c == -2 ? "lock class name exceeds LOCKDEP_CLASS_NAME_MAX" :
                             "lock class table full (LOCKDEP_MAX_CLASSES)", NULL, 0);
            return -1;
        }
        cached = (uint16_t)(c + 1);
        __atomic_store_n(class_slot, cached, __ATOMIC_RELEASE);
    }
    if (subclass >= LOCKDEP_SUBCLASSES)
        panic("lockdep: subclass %u out of range for '%s'", subclass, name);
    return (int)lockdep_node(cached - 1u, subclass);
}

/* Usage and edge conflicts report outside the raw lock. Names are owned
 * and immutable; record both endpoints, not just the acquired class. */
static void report_irq(const char *name, unsigned subclass, uintptr_t ip,
                       uint16_t safe, uint16_t unsafe, const char *change)
{
    char detail[384];
    ksnprintf(detail, sizeof(detail),
              "%s connects IRQ-used '%s'#%u to IRQ-enabled '%s'#%u",
              change, g_graph.classes[lockdep_node_class(safe)].name, lockdep_node_subclass(safe),
              g_graph.classes[lockdep_node_class(unsafe)].name, lockdep_node_subclass(unsafe));
    report(LOCKDEP_R_IRQ, name, subclass, ip, detail, NULL, 0);
}

static bool check_usage(uint16_t node, bool in_irq, bool irqs_on, bool trylock, uintptr_t ip)
{
    unsigned cls = lockdep_node_class(node);
    struct lock_class *c = &g_graph.classes[cls];
    unsigned add = lockdep_core_usage(in_irq, irqs_on, trylock);
    if (c->kind != LOCKDEP_KIND_SPIN || !add ||
        (__atomic_load_n(&c->usage, __ATOMIC_ACQUIRE) & add) == add)
        return true;
    uint16_t safe, unsafe;
    arch_irq_state_t s = raw_lock();
    bool valid = lockdep_core_mark_usage(&g_graph, &g_scratch, cls, add, ip, &safe, &unsafe);
    raw_unlock(s);
    if (!valid)
        report_irq(c->name, lockdep_node_subclass(node), ip, safe, unsafe, "new class usage");
    return valid;
}

static unsigned timer_profile_start(const void *timer)
{
    uintptr_t key = (uintptr_t)timer >> 4;
    key ^= key >> 13;
    key *= (uintptr_t)0x9e3779b1u;
    return (unsigned)(key % LOCKDEP_MAX_TIMER_PROFILES);
}

static int timer_profile_find(const void *timer)
{
    unsigned first = timer_profile_start(timer);
    for (unsigned n = 0; n < LOCKDEP_MAX_TIMER_PROFILES; n++) {
        unsigned i = (first + n) % LOCKDEP_MAX_TIMER_PROFILES;
        if (g_timer_profiles[i].timer == timer)
            return (int)i;
        if (g_timer_profiles[i].timer == NULL)
            return -1;
    }
    return -1;
}

void lockdep_timer_cancel_done(const void *timer)
{
    arch_irq_state_t s = raw_lock();
    int slot = timer_profile_find(timer);
    if (slot >= 0) {
        g_timer_profiles[slot] = (struct lockdep_timer_profile){ .timer = TIMER_PROFILE_TOMBSTONE };
    }
    raw_unlock(s);
}

void lockdep_timer_enter(const void *timer)
{
    /* run_expired executes callbacks serially with IRQs masked, and
     * clears each profile before starting another callback on that CPU.
     * Thus at most CONFIG_MAX_CPUS profiles can be live; the static
     * assertion above makes capacity independent of timer queue depth. */
    struct lockdep_cpu *lc = my_cpu();
    arch_irq_state_t s = raw_lock();
    int slot = timer_profile_find(timer);
    if (slot >= 0) {
        g_timer_profiles[slot] = (struct lockdep_timer_profile){ .timer = timer };
    } else {
        unsigned first = timer_profile_start(timer);
        for (unsigned n = 0; n < LOCKDEP_MAX_TIMER_PROFILES; n++) {
            unsigned i = (first + n) % LOCKDEP_MAX_TIMER_PROFILES;
            if (g_timer_profiles[i].timer == NULL || g_timer_profiles[i].timer == TIMER_PROFILE_TOMBSTONE) {
                slot = (int)i;
                g_timer_profiles[slot] = (struct lockdep_timer_profile){ .timer = timer };
                break;
            }
        }
    }
    raw_unlock(s);
    if (slot < 0) {
        report(LOCKDEP_R_OVERFLOW, NULL, 0, (uintptr_t)__builtin_return_address(0),
               "timer callback has no registered lock profile", NULL, 0);
        return;
    }
    lc->callback_timer = timer;
    lc->callback_profile = (unsigned)slot;
}

void lockdep_timer_exit(const void *timer)
{
    struct lockdep_cpu *lc = my_cpu();
    if (lc->callback_timer != timer)
        report(LOCKDEP_R_OVERFLOW, NULL, 0, (uintptr_t)__builtin_return_address(0),
               "timer callback context nesting mismatch", NULL, 0);
    lc->callback_timer = NULL;
}

static void timer_profile_note(const void *timer, unsigned profile, const void *lock, uint16_t node)
{
    arch_irq_state_t s = raw_lock();
    if (profile < LOCKDEP_MAX_TIMER_PROFILES && g_timer_profiles[profile].timer == timer) {
        struct lockdep_timer_profile *p = &g_timer_profiles[profile];
        bool found = false;
        for (unsigned i = 0; i < p->nr_locks; i++)
            if (p->locks[i].lock == lock) { found = true; break; }
        if (!found) {
            if (p->nr_locks == LOCKDEP_MAX_TIMER_LOCKS)
                p->overflow = true;
            else
                p->locks[p->nr_locks++] = (struct lockdep_timer_lock){ lock, node };
        }
    }
    bool overflow = profile >= LOCKDEP_MAX_TIMER_PROFILES || g_timer_profiles[profile].overflow;
    raw_unlock(s);
    if (overflow)
        report(LOCKDEP_R_OVERFLOW, NULL, 0, (uintptr_t)__builtin_return_address(0),
               "timer callback lock profile exceeded its bounded storage", NULL, 0);
}

bool lockdep_timer_cancel_check(const void *timer, uintptr_t ip)
{
    preempt_disable();
    struct lockdep_cpu *lc = my_cpu();
    struct thread *t = me();
    arch_irq_state_t s = raw_lock();
    int slot = timer_profile_find(timer);
    bool conflict = false, overflow = false;
    const struct lockdep_timer_lock callback_lock = { 0 };
    struct lockdep_timer_lock found = callback_lock;
    if (slot >= 0) {
        struct lockdep_timer_profile *p = &g_timer_profiles[slot];
        overflow = p->overflow;
        for (unsigned i = 0; i < p->nr_locks && !conflict; i++) {
            for (unsigned j = 0; j < lc->nr_held; j++)
                if (lc->held[j].lock == p->locks[i].lock) {
                    conflict = true; found = p->locks[i]; break;
                }
            if (t)
                for (unsigned j = 0; j < t->nr_held_mutex && !conflict; j++)
                    if (t->held_mutex[j].lock == p->locks[i].lock) {
                        conflict = true; found = p->locks[i]; break;
                    }
        }
    }
    raw_unlock(s);
    if (overflow) {
        report(LOCKDEP_R_OVERFLOW, NULL, 0, ip, "timer callback lock profile overflowed", NULL, 0);
        preempt_enable();
        return false;
    }
    if (conflict) {
        unsigned cls = lockdep_node_class(found.node);
        report(LOCKDEP_R_CALLBACK, g_graph.classes[cls].name, lockdep_node_subclass(found.node), ip,
               "timer_cancel_sync holds an object lock acquired by this timer's callback", NULL, 0);
        preempt_enable();
        return false;
    }
    preempt_enable();
    return true;
}

/*
 * The check, before the acquisition waits. Nothing is pushed here: a lock
 * that is still contended is not held, and an interrupt arriving during
 * the wait (plain spin_lock with interrupts enabled) must not see it on
 * the stack. Edges record "attempted while held", which is the order
 * relation the checker wants whether or not the attempt has completed.
 */
static bool acquire_check(const void *lock, uint16_t *class_slot, const char *name, unsigned kind,
                          unsigned subclass, bool irqs_on, uintptr_t ip, bool wait)
{
    struct percpu *pc = raw_this_cpu();   /* identity: irq_depth is the same wherever the thread runs */
    bool in_irq = pc->irq_depth != 0;
    struct lockdep_cpu *lc = my_cpu();
    struct thread *t = in_irq ? NULL : me();
    /* A synchronous wait for a callback reports as a callback dependency,
     * whichever check finds it. A lock taken inside a callback that
     * closes the same cycle from the other side is an ordinary inversion
     * whose chain names the callback class. */
    enum lockdep_report_kind recursion = wait ? LOCKDEP_R_CALLBACK : LOCKDEP_R_RECURSION;
    enum lockdep_report_kind inversion = wait ? LOCKDEP_R_CALLBACK : LOCKDEP_R_INVERSION;

    int n = node_of(class_slot, name, kind, subclass, ip);
    if (n < 0)
        return false;
    uint16_t node = (uint16_t)n;
    count_acquisition();
    if (kind == LOCKDEP_KIND_SPIN && in_irq && lc->callback_timer)
        timer_profile_note(lc->callback_timer, lc->callback_profile, lock, node);

    /* 1. Interrupt safety, including paths observed before this usage. */
    if (!check_usage(node, in_irq, irqs_on, false, ip))
        return false;

    /* 2. The held set: this CPU's spinlocks, plus the thread's mutexes in thread context. */
    const struct lockdep_held *held[2] = { lc->held, t ? t->held_mutex : NULL };
    unsigned nheld[2] = { lc->nr_held, t ? t->nr_held_mutex : 0 };

    /* 2a. Same node already held: recursion. For a wait, the waiter is
     * inside a callback of the function it waits for. */
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned i = 0; i < nheld[k]; i++) {
            if (held[k][i].node == node) {
                report(recursion, name, subclass, ip,
                       wait ? "a callback waits synchronously for a callback of its own function" :
                              "the same lock class is already held; nest with a *_lock_nested subclass if this is intended",
                       NULL, 0);
                return false;   /* expected by a test: add no edges */
            }
        }
    }
    /* 2b. Order: would `node` reach any held node? Then record the edges. */
    bool need_edges = false;
    for (unsigned k = 0; k < 2 && !need_edges; k++)
        for (unsigned i = 0; i < nheld[k]; i++)
            if (!lockdep_core_has_edge(&g_graph, held[k][i].node, node)) {
                need_edges = true;
                break;
            }
    if (!need_edges)
        return true;
    uint16_t path[8];
    unsigned path_len = 0;
    bool cycle = false, irq_conflict = false;
    uint16_t safe = 0, unsafe = 0;
    uint16_t against = 0;
    arch_irq_state_t s = raw_lock();
    for (unsigned k = 0; k < 2 && !cycle && !irq_conflict; k++) {
        for (unsigned i = 0; i < nheld[k]; i++) {
            if (lockdep_core_has_edge(&g_graph, held[k][i].node, node))
                continue;
            g_stats.searches++;   /* g_raw held */
            if (lockdep_core_reaches(&g_graph, &g_scratch, node, held[k][i].node, path, 8, &path_len)) {
                cycle = true;
                against = held[k][i].node;
                break;
            }
            if (lockdep_core_irq_edge(&g_graph, &g_scratch, held[k][i].node, node, &safe, &unsafe)) {
                irq_conflict = true;
                against = held[k][i].node;
                break;
            }
            lockdep_core_add_edge(&g_graph, held[k][i].node, node);
        }
    }
    raw_unlock(s);
    if (irq_conflict) {
        char change[160];
        ksnprintf(change, sizeof(change), "new edge '%s'#%u -> '%s'#%u",
                  g_graph.classes[lockdep_node_class(against)].name, lockdep_node_subclass(against), name, subclass);
        report_irq(name, subclass, ip, safe, unsafe, change);
    }
    if (cycle) {
        char detail[160];
        const struct lock_class *ac = &g_graph.classes[lockdep_node_class(against)];
        if (wait)
            ksnprintf(detail, sizeof(detail), "'%s'#%u is held across a wait for '%s', which reaches it",
                      ac->name, lockdep_node_subclass(against), name);
        else
            ksnprintf(detail, sizeof(detail), "'%s'#%u is held, and '%s'#%u was recorded before it elsewhere", ac->name,
                      lockdep_node_subclass(against), name, subclass);
        report(inversion, name, subclass, ip, detail, path, path_len);
    }
    return !cycle && !irq_conflict;
}

/*
 * With interrupts off throughout: a mutex acquisition arrives with
 * preemption on, and this CPU's held-spinlock stack (L11) is this
 * CPU's only while the thread cannot move (S25). A spinlock's arrives
 * with preemption already off; the save costs it nothing it notices.
 */
void lockdep_acquire_check(const void *lock, uint16_t *class_slot, const char *name, unsigned kind,
                           unsigned subclass, bool irqs_on, uintptr_t ip)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return;
    if (kind == LOCKDEP_KIND_MUTEX) {
        arch_irq_state_t s = arch_irq_save();
        (void)acquire_check(lock, class_slot, name, kind, subclass, irqs_on, ip, false);
        arch_irq_restore(s);
    } else {
        (void)acquire_check(lock, class_slot, name, kind, subclass, irqs_on, ip, false);
    }
}

/* The lock is owned now: push it. */
void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                      bool trylock, bool irqs_on, uintptr_t ip)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return;
    struct percpu *pc = raw_this_cpu();   /* identity */
    bool in_irq = pc->irq_depth != 0;
    int n = node_of(class_slot, name, kind, subclass, ip);
    if (n < 0)
        return;
    if (trylock) {
        count_acquisition();
        (void)check_usage((uint16_t)n, in_irq, irqs_on, true, ip);
    }
    struct lockdep_held e = { .node = (uint16_t)n,
                              .flags = (uint8_t)((trylock ? LOCKDEP_HF_TRYLOCK : 0u) |
                                                 (in_irq ? LOCKDEP_HF_IN_IRQ : 0u) |
                                                 (irqs_on ? LOCKDEP_HF_IRQS_ON : 0u)),
                              .ip = ip,
                              .lock = lock };
    if (kind == LOCKDEP_KIND_MUTEX) {
        struct thread *t = in_irq ? NULL : me();
        if (t == NULL)
            return;   /* mutexes before threads exist are not tracked */
        if (t->nr_held_mutex == LOCKDEP_MAX_HELD_MUTEX) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip, "per-thread mutex stack full", NULL, 0);
            return;
        }
        unsigned count = t->nr_held_mutex;
        lockdep_core_held_begin(&t->held_mutex_seq);
        lockdep_core_held_store(&t->held_mutex[count], &e);
        __atomic_store_n(&t->nr_held_mutex, count + 1u, __ATOMIC_SEQ_CST);
        lockdep_core_held_end(&t->held_mutex_seq);
    } else {
        struct lockdep_cpu *lc = my_cpu();
        if (lc->nr_held == LOCKDEP_MAX_HELD) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip, "per-CPU spinlock stack full", NULL, 0);
            return;
        }
        lockdep_core_held_begin(&lc->held_seq);
        lockdep_core_held_store(&lc->held[lc->nr_held], &e);
        __atomic_store_n(&lc->nr_held, lc->nr_held + 1u, __ATOMIC_SEQ_CST);
        lockdep_core_held_end(&lc->held_seq);
    }
}

static bool remove_entry(struct lockdep_held *held, unsigned *n, uint64_t *seq, const void *lock)
{
    for (unsigned i = *n; i-- > 0;) {
        if (held[i].lock == lock) {
            lockdep_core_held_begin(seq);
            for (unsigned j = i; j + 1 < *n; j++)
                lockdep_core_held_store(&held[j], &held[j + 1]);
            __atomic_store_n(n, *n - 1u, __ATOMIC_SEQ_CST);
            lockdep_core_held_end(seq);
            return true;
        }
    }
    return false;
}

void lockdep_irqsave_acquired(const void *lock, bool irq_was_enabled)
{
    struct lockdep_cpu *lc = my_cpu();
    for (unsigned i = lc->nr_held; i-- > 0;) {
        if (lc->held[i].lock == lock) {
            struct lockdep_held e = lc->held[i];
            e.flags |= LOCKDEP_HF_IRQSAVE;
            if (irq_was_enabled)
                e.flags |= LOCKDEP_HF_IRQSAVE_ON;
            lockdep_core_held_begin(&lc->held_seq);
            lockdep_core_held_store(&lc->held[i], &e);
            lockdep_core_held_end(&lc->held_seq);
            return;
        }
    }
    report(LOCKDEP_R_UNHELD, NULL, 0, (uintptr_t)__builtin_return_address(0),
           "irqsave acquisition is missing from the per-CPU held stack", NULL, 0);
}

void lockdep_irqrestore_check(const void *lock, bool irq_will_enable, uintptr_t ip)
{
    struct lockdep_cpu *lc = my_cpu();
    for (unsigned i = lc->nr_held; i-- > 0;) {
        struct lockdep_held *h = &lc->held[i];
        if (h->lock != lock)
            continue;
        if (!(h->flags & LOCKDEP_HF_IRQSAVE) ||
            (((h->flags & LOCKDEP_HF_IRQSAVE_ON) != 0) != irq_will_enable)) {
            report(LOCKDEP_R_IRQ_STATE, NULL, 0, ip,
                   "restored interrupt state differs from this lock's irqsave state", NULL, 0);
        }
        if (irq_will_enable) {
            for (unsigned j = 0; j < lc->nr_held; j++) {
                if (j != i && !(lc->held[j].flags & LOCKDEP_HF_IRQS_ON)) {
                    report(LOCKDEP_R_IRQ_STATE, NULL, 0, ip,
                           "interrupts would be enabled while a lock acquired with IRQs off remains held", NULL, 0);
                    break;
                }
            }
        }
        return;
    }
    report(LOCKDEP_R_UNHELD, NULL, 0, ip,
           "irqrestore does not match a spinlock held by this CPU", NULL, 0);
}

void lockdep_release(const void *lock, unsigned kind, uintptr_t ip, bool irqrestore)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return;
    if (kind == LOCKDEP_KIND_MUTEX) {
        struct thread *t = me();
        if (t == NULL)
            return;
        if (!remove_entry(t->held_mutex, &t->nr_held_mutex, &t->held_mutex_seq, lock))
            report(LOCKDEP_R_UNHELD, NULL, 0, ip, "mutex_unlock of a mutex this thread does not hold", NULL, 0);
        return;
    }
    struct lockdep_cpu *lc = my_cpu();
    unsigned old_nr = lc->nr_held;
    unsigned found = old_nr;
    for (unsigned i = old_nr; i-- > 0;)
        if (lc->held[i].lock == lock) { found = i; break; }
    if (found == old_nr) {
        report(LOCKDEP_R_UNHELD, NULL, 0, ip, "spin_unlock of a spinlock this CPU does not hold", NULL, 0);
        return;
    }
    bool was_irqsave = (lc->held[found].flags & LOCKDEP_HF_IRQSAVE) != 0;
    if (irqrestore && !was_irqsave)
        report(LOCKDEP_R_IRQ_STATE, NULL, 0, ip,
               "irqrestore used for a lock acquired without irqsave", NULL, 0);
    lockdep_core_held_begin(&lc->held_seq);
    for (unsigned i = found; i + 1u < old_nr; i++)
        lockdep_core_held_store(&lc->held[i], &lc->held[i + 1u]);
    __atomic_store_n(&lc->nr_held, old_nr - 1u, __ATOMIC_SEQ_CST);
    lockdep_core_held_end(&lc->held_seq);
}

/* --- callback classes ------------------------------------------------------ */

/*
 * One class per callback function, named by its address: the graph copies
 * the name, and two timers with one function share the class -- which is
 * what makes a wait for one timer answerable from what another's callback
 * did (design.md, "Callback classes").
 */
/* The class's name: the graph's own copy once the slot is cached, so a
 * callback that runs every tick does not format a string every tick. The
 * class table only grows, so a cached index stays valid. */
static const char *callback_name(char *buf, size_t len, const void *fn, const uint16_t *class_slot)
{
    uint16_t cached = __atomic_load_n(class_slot, __ATOMIC_ACQUIRE);
    if (cached != 0)
        return g_graph.classes[cached - 1u].name;
    ksnprintf(buf, len, "callback %p", fn);
    return buf;
}

void lockdep_callback_enter(const void *fn, uint16_t *class_slot)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return;
    char buf[LOCKDEP_CLASS_NAME_MAX];
    const char *name = callback_name(buf, sizeof(buf), fn, class_slot);
    uintptr_t ip = (uintptr_t)__builtin_return_address(0);
    bool irqs_on = arch_irq_enabled();
    /* Held while the callback runs, like a lock the callback holds: every
     * lock taken inside is recorded under it. */
    (void)acquire_check(fn, class_slot, name, LOCKDEP_KIND_CALLBACK, 0, irqs_on, ip, false);
    lockdep_acquired(fn, class_slot, name, LOCKDEP_KIND_CALLBACK, 0, false, irqs_on, ip);
}

void lockdep_callback_exit(const void *fn)
{
    lockdep_release(fn, LOCKDEP_KIND_CALLBACK, (uintptr_t)__builtin_return_address(0), false);
}

bool lockdep_callback_wait(const void *fn, uint16_t *class_slot, uintptr_t ip)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return true;
    char buf[LOCKDEP_CLASS_NAME_MAX];
    const char *name = callback_name(buf, sizeof(buf), fn, class_slot);
    /* Acquired, never held: the edges say "this was waited for while the
     * held locks were held". Interrupts off for the CPU's held stack, as
     * for a mutex acquisition. */
    arch_irq_state_t s = arch_irq_save();
    bool ok = acquire_check(fn, class_slot, name, LOCKDEP_KIND_CALLBACK, 0, arch_irq_state_enabled(s), ip, true);
    arch_irq_restore(s);
    return ok;
}

/* --- the other checks ----------------------------------------------------- */

void lockdep_might_sleep(uintptr_t ip)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE))
        return;
    char detail[96];
    ksnprintf(detail, sizeof(detail), "preempt_count %d, irq_depth %u", raw_this_cpu()->preempt_count,
              raw_this_cpu()->irq_depth);   /* a report */
    report(LOCKDEP_R_SLEEP, NULL, 0, ip, detail, NULL, 0);
}

void lockdep_thread_exit(struct thread *t)
{
    if (__atomic_load_n(&g_off, __ATOMIC_ACQUIRE) || t->nr_held_mutex == 0)
        return;
    const struct lock_class *c = &g_graph.classes[lockdep_node_class(t->held_mutex[0].node)];
    report(LOCKDEP_R_EXIT_HELD, c->name, lockdep_node_subclass(t->held_mutex[0].node), t->held_mutex[0].ip, NULL,
           NULL, 0);
}

bool lockdep_is_held(const void *lock, unsigned kind)
{
    if (kind == LOCKDEP_KIND_MUTEX) {
        struct thread *t = me();
        if (t == NULL)
            return false;
        for (unsigned i = 0; i < t->nr_held_mutex; i++)
            if (t->held_mutex[i].lock == lock)
                return true;
        return false;
    }
    /* This CPU's stack, read with interrupts off when the asker could
     * move: a preemptible thread (lockdep_assert_not_held) that moved
     * between the read and the scan would be scanning another CPU's
     * stack (S25). A holder of any spinlock has preemption off already. */
    bool save = raw_this_cpu()->preempt_count == 0;
    arch_irq_state_t s = save ? arch_irq_save() : 0;
    struct lockdep_cpu *lc = my_cpu();
    bool held = false;
    for (unsigned i = 0; i < lc->nr_held && !held; i++)
        if (lc->held[i].lock == lock)
            held = true;
    if (save)
        arch_irq_restore(s);
    return held;
}

void lockdep_dump_graph(void)
{
    /* Normal diagnostics only: allocate before taking the raw lock, then
     * copy a bounded graph and print from private storage. Neither heap
     * operations nor logging may run under the validator's raw lock. */
    struct lockdep_graph *snapshot = kmalloc(sizeof(*snapshot), 0);
    if (!snapshot) {
        kwarn("lockdep: graph dump unavailable: snapshot allocation failed");
        return;
    }
    arch_irq_state_t s = raw_lock();
    lockdep_core_snapshot(&g_graph, snapshot);
    raw_unlock(s);
    unsigned nr_classes = snapshot->nr_classes;
    unsigned nr_edges = snapshot->nr_edges;
    unsigned nr_nodes = nr_classes * LOCKDEP_SUBCLASSES;
    kdebug("lockdep: %u classes, %u edges (a -> b: b was taken while a was held)", nr_classes, nr_edges);
    for (unsigned a = 0; a < nr_nodes; a++) {
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++) {
            uint64_t bits = snapshot->before[a][w];
            while (bits) {
                unsigned bit = (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                unsigned b = w * 64u + bit;
                const struct lock_class *ca = &snapshot->classes[lockdep_node_class((uint16_t)a)];
                const struct lock_class *cb = &snapshot->classes[lockdep_node_class((uint16_t)b)];
                kdebug("lockdep: edge %s '%s'#%u -> %s '%s'#%u", kind_name(ca->kind), ca->name,
                       lockdep_node_subclass((uint16_t)a), kind_name(cb->kind), cb->name,
                       lockdep_node_subclass((uint16_t)b));
            }
        }
    }
    kfree(snapshot);
}

void lockdep_get_stats(struct lockdep_stats *out)
{
    /* Every counter and graph mutation shares g_raw. The copy describes
     * one instant, which can include acquisitions still in progress;
     * it does not freeze the CPU/thread held stacks. Normal context only. */
    arch_irq_state_t s = raw_lock();
    *out = g_stats;
    out->classes = g_graph.nr_classes;
    out->edges = g_graph.nr_edges;
    raw_unlock(s);
}

void lockdep_expect(enum lockdep_report_kind kind)
{
    unsigned cpu = raw_cpu_id();
    __atomic_store_n(&g_expect[cpu], (int)kind, __ATOMIC_RELAXED);
    __atomic_store_n(&g_expect_armed[cpu], true, __ATOMIC_RELEASE);
}

unsigned lockdep_expected_hits(void)
{
    return __atomic_exchange_n(&g_expected_hits[raw_cpu_id()], 0u, __ATOMIC_ACQ_REL);
}

#if CONFIG_SELFTEST
void lockdep_test_snapshot_context(bool updating, void (*probe)(void *), void *arg)
{
    arch_irq_state_t s = raw_lock();
    struct lockdep_cpu *lc = my_cpu();
    if (updating)
        lockdep_core_held_begin(&lc->held_seq);
    probe(arg);
    if (updating)
        lockdep_core_held_end(&lc->held_seq);
    raw_unlock(s);
}
#endif

#endif /* CONFIG_LOCKDEP */
