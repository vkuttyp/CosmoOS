/*
 * lockdep.c - Held-lock stacks, the dependency graph and the reports
 * (docs/kernel/lockdep/design.md). Debug builds only.
 *
 * The algorithm is lockdep_core.h. This file owns the per-CPU spinlock
 * stacks, the per-thread mutex stacks, the raw lock that serialises graph
 * updates, and the report path. It takes no tracked lock and allocates
 * nothing, so it is safe from every context a lock is taken in.
 */

#include <kernel/lockdep.h>

#if CONFIG_LOCKDEP

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
};

static struct lockdep_graph g_graph;
static struct lockdep_scratch g_scratch;   /* used under g_raw only */
static struct lockdep_cpu g_cpus[CONFIG_MAX_CPUS];
static struct lockdep_stats g_stats;
static bool g_off;                          /* after a report that panicked, or during panic */

/* Self-test expectations. */
static int g_expect = -1;
static unsigned g_expected_hits;

/* The checker's own lock: a raw word, never tracked. */
static uint32_t g_raw;

static arch_irq_state_t raw_lock(void)
{
    arch_irq_state_t s = arch_irq_save();
    while (__atomic_exchange_n(&g_raw, 1u, __ATOMIC_ACQUIRE) != 0)
        arch_cpu_relax();
    return s;
}

static void raw_unlock(arch_irq_state_t s)
{
    __atomic_store_n(&g_raw, 0u, __ATOMIC_RELEASE);
    arch_irq_restore(s);
}

static const char *const g_kind_names[LOCKDEP_R_COUNT] = {
    [LOCKDEP_R_INVERSION] = "lock-order inversion",
    [LOCKDEP_R_RECURSION] = "recursive acquisition of one lock class",
    [LOCKDEP_R_IRQ] = "IRQ-unsafe use of an interrupt-context lock",
    [LOCKDEP_R_SLEEP] = "sleeping call in atomic context",
    [LOCKDEP_R_OVERFLOW] = "held-lock stack overflow",
    [LOCKDEP_R_UNHELD] = "release of a lock that is not held",
    [LOCKDEP_R_EXIT_HELD] = "thread exit with a mutex held",
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
    struct percpu *pc = this_cpu();
    return pc->current;
}

static void print_held(const char *who, const struct lockdep_held *h, unsigned n)
{
    kprintf("  held by %s (%u):\n", who, n);
    for (unsigned i = 0; i < n; i++) {
        const struct lock_class *c = &g_graph.classes[lockdep_node_class(h[i].node)];
        kprintf("    [%u] %s '%s'#%u at %p%s%s%s\n", i, c->kind == LOCKDEP_KIND_MUTEX ? "mutex" : "spin", c->name,
                lockdep_node_subclass(h[i].node), (void *)h[i].ip, (h[i].flags & LOCKDEP_HF_IN_IRQ) ? " [irq]" : "",
                (h[i].flags & LOCKDEP_HF_IRQS_ON) ? " [irqs-on]" : "",
                (h[i].flags & LOCKDEP_HF_TRYLOCK) ? " [try]" : "");
    }
}

void lockdep_dump_held(void)
{
    struct lockdep_cpu *lc = my_cpu();
    struct thread *t = me();
    print_held("this CPU", lc->held, lc->nr_held);
    if (t)
        print_held(t->name, t->held_mutex, t->nr_held_mutex);
}

/* --- reports ---------------------------------------------------------------- */

/* Panics unless the report was expected by a self-test, in which case it
 * returns and the caller proceeds as if the operation were legal. */
static void report(enum lockdep_report_kind kind, const char *name, unsigned subclass, uintptr_t ip, const char *detail,
                   const uint16_t *path, unsigned path_len)
{
    g_stats.reports++;
    if (g_expect == (int)kind) {
        g_expect = -1;
        g_expected_hits++;
        kdebug("lockdep: expected report: %s ('%s'#%u at %p)", g_kind_names[kind], name ? name : "-", subclass,
               (void *)ip);
        return;
    }
    g_off = true;
    struct thread *t = me();
    kprintf("\nlockdep: %s\n", g_kind_names[kind]);
    kprintf("  lock '%s'#%u at %p, CPU %u, thread '%s', irq_depth %u, preempt_count %d\n", name ? name : "-",
            subclass, (void *)ip, arch_cpu_id(), t ? t->name : "(boot)", this_cpu()->irq_depth,
            this_cpu()->preempt_count);
    if (detail)
        kprintf("  %s\n", detail);
    lockdep_dump_held();
    if (path_len) {
        kprintf("  recorded chain that closes the cycle:");
        for (unsigned i = 0; i < path_len; i++) {
            const struct lock_class *c = &g_graph.classes[lockdep_node_class(path[i])];
            kprintf("%s '%s'#%u", i ? " ->" : "", c->name, lockdep_node_subclass(path[i]));
        }
        kprintf("\n");
    }
    panic("lockdep: %s", g_kind_names[kind]);
}

/* --- acquire / release ------------------------------------------------------ */

void lockdep_acquire(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                     bool trylock, bool irqs_on, uintptr_t ip)
{
    if (g_off)
        return;
    struct percpu *pc = this_cpu();
    bool in_irq = pc->irq_depth != 0;
    struct lockdep_cpu *lc = my_cpu();
    struct thread *t = in_irq ? NULL : me();

    /* 1. The class, cached in the lock after the first lookup. */
    unsigned cls;
    if (*class_slot == 0) {
        arch_irq_state_t s = raw_lock();
        int c = lockdep_core_class(&g_graph, name, kind);
        raw_unlock(s);
        if (c < 0) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip, "lock class table full (LOCKDEP_MAX_CLASSES)", NULL, 0);
            return;
        }
        *class_slot = (uint16_t)(c + 1);
    }
    cls = *class_slot - 1u;
    if (subclass >= LOCKDEP_SUBCLASSES)
        panic("lockdep: subclass %u out of range for '%s'", subclass, name);
    uint16_t node = lockdep_node(cls, subclass);
    __atomic_fetch_add(&g_stats.acquisitions, 1u, __ATOMIC_RELAXED);

    /* 2. Interrupt safety. */
    struct lock_class *c = &g_graph.classes[cls];
    if (kind == LOCKDEP_KIND_SPIN) {
        const unsigned both = LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON;
        unsigned add = (in_irq ? LOCKDEP_USED_IN_IRQ : 0u) | (irqs_on ? LOCKDEP_HELD_IRQS_ON : 0u);
        if (add && (c->usage & add) != add) {
            arch_irq_state_t s = raw_lock();
            unsigned before = c->usage;
            if ((add & LOCKDEP_USED_IN_IRQ) && !(before & LOCKDEP_USED_IN_IRQ))
                c->irq_ip = ip;
            if ((add & LOCKDEP_HELD_IRQS_ON) && !(before & LOCKDEP_HELD_IRQS_ON))
                c->irqs_on_ip = ip;
            c->usage |= add;
            raw_unlock(s);
            /* Report the acquisition that completes the conflict, once
             * per class: the class keeps both bits afterwards. */
            if ((before & both) != both && (c->usage & both) == both) {
                char detail[128];
                ksnprintf(detail, sizeof(detail),
                          "taken in interrupt context at %p and with interrupts enabled at %p", (void *)c->irq_ip,
                          (void *)c->irqs_on_ip);
                report(LOCKDEP_R_IRQ, name, subclass, ip, detail, NULL, 0);
            }
        }
    }

    /* 3. The held set: this CPU's spinlocks, plus the thread's mutexes in thread context. */
    const struct lockdep_held *held[2] = { lc->held, t ? t->held_mutex : NULL };
    unsigned nheld[2] = { lc->nr_held, t ? t->nr_held_mutex : 0 };

    if (!trylock) {
        /* 3a. Same node already held: recursion. */
        for (unsigned k = 0; k < 2; k++) {
            for (unsigned i = 0; i < nheld[k]; i++) {
                if (held[k][i].node == node) {
                    report(LOCKDEP_R_RECURSION, name, subclass, ip,
                           "the same lock class is already held; nest with a *_lock_nested subclass if this is intended",
                           NULL, 0);
                    goto push;   /* expected by a test: record it, add no edges */
                }
            }
        }
        /* 3b. Order: would `node` reach any held node? Then record the edges. */
        bool need_edges = false;
        for (unsigned k = 0; k < 2 && !need_edges; k++)
            for (unsigned i = 0; i < nheld[k]; i++)
                if (!lockdep_core_has_edge(&g_graph, held[k][i].node, node)) {
                    need_edges = true;
                    break;
                }
        if (need_edges) {
            uint16_t path[8];
            unsigned path_len = 0;
            bool cycle = false;
            uint16_t against = 0;
            arch_irq_state_t s = raw_lock();
            for (unsigned k = 0; k < 2 && !cycle; k++) {
                for (unsigned i = 0; i < nheld[k]; i++) {
                    if (lockdep_core_has_edge(&g_graph, held[k][i].node, node))
                        continue;
                    g_stats.searches++;
                    if (lockdep_core_reaches(&g_graph, &g_scratch, node, held[k][i].node, path, 8, &path_len)) {
                        cycle = true;
                        against = held[k][i].node;
                        break;
                    }
                    lockdep_core_add_edge(&g_graph, held[k][i].node, node);
                }
            }
            raw_unlock(s);
            if (cycle) {
                char detail[128];
                const struct lock_class *ac = &g_graph.classes[lockdep_node_class(against)];
                ksnprintf(detail, sizeof(detail), "'%s'#%u is held, and '%s'#%u was recorded before it elsewhere",
                          ac->name, lockdep_node_subclass(against), name, subclass);
                report(LOCKDEP_R_INVERSION, name, subclass, ip, detail, path, path_len);
            }
        }
    }

push:;
    /* 4. Push. */
    struct lockdep_held e = { .node = node,
                              .flags = (uint8_t)((trylock ? LOCKDEP_HF_TRYLOCK : 0u) |
                                                 (in_irq ? LOCKDEP_HF_IN_IRQ : 0u) |
                                                 (irqs_on ? LOCKDEP_HF_IRQS_ON : 0u)),
                              .ip = ip,
                              .lock = lock };
    if (kind == LOCKDEP_KIND_MUTEX) {
        if (t == NULL)
            return;   /* mutexes before threads exist are not tracked */
        if (t->nr_held_mutex == LOCKDEP_MAX_HELD_MUTEX) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip, "per-thread mutex stack full", NULL, 0);
            return;
        }
        t->held_mutex[t->nr_held_mutex++] = e;
    } else {
        if (lc->nr_held == LOCKDEP_MAX_HELD) {
            report(LOCKDEP_R_OVERFLOW, name, subclass, ip, "per-CPU spinlock stack full", NULL, 0);
            return;
        }
        lc->held[lc->nr_held++] = e;
    }
}

static bool remove_entry(struct lockdep_held *held, unsigned *n, const void *lock)
{
    for (unsigned i = *n; i-- > 0;) {
        if (held[i].lock == lock) {
            for (unsigned j = i; j + 1 < *n; j++)
                held[j] = held[j + 1];
            (*n)--;
            return true;
        }
    }
    return false;
}

void lockdep_release(const void *lock, unsigned kind, uintptr_t ip)
{
    if (g_off)
        return;
    if (kind == LOCKDEP_KIND_MUTEX) {
        struct thread *t = me();
        if (t == NULL)
            return;
        if (!remove_entry(t->held_mutex, &t->nr_held_mutex, lock))
            report(LOCKDEP_R_UNHELD, NULL, 0, ip, "mutex_unlock of a mutex this thread does not hold", NULL, 0);
        return;
    }
    struct lockdep_cpu *lc = my_cpu();
    if (!remove_entry(lc->held, &lc->nr_held, lock))
        report(LOCKDEP_R_UNHELD, NULL, 0, ip, "spin_unlock of a spinlock this CPU does not hold", NULL, 0);
}

/* --- the other checks ----------------------------------------------------- */

void lockdep_might_sleep(uintptr_t ip)
{
    if (g_off)
        return;
    char detail[96];
    ksnprintf(detail, sizeof(detail), "preempt_count %d, irq_depth %u", this_cpu()->preempt_count,
              this_cpu()->irq_depth);
    report(LOCKDEP_R_SLEEP, NULL, 0, ip, detail, NULL, 0);
}

void lockdep_thread_exit(struct thread *t)
{
    if (g_off || t->nr_held_mutex == 0)
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
    struct lockdep_cpu *lc = my_cpu();
    for (unsigned i = 0; i < lc->nr_held; i++)
        if (lc->held[i].lock == lock)
            return true;
    return false;
}

void lockdep_dump_graph(void)
{
    kdebug("lockdep: %u classes, %u edges (a -> b: b was taken while a was held)", g_graph.nr_classes,
           g_graph.nr_edges);
    for (unsigned a = 0; a < g_graph.nr_classes * LOCKDEP_SUBCLASSES; a++) {
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++) {
            uint64_t bits = g_graph.before[a][w];
            while (bits) {
                unsigned bit = (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                unsigned b = w * 64u + bit;
                const struct lock_class *ca = &g_graph.classes[lockdep_node_class((uint16_t)a)];
                const struct lock_class *cb = &g_graph.classes[lockdep_node_class((uint16_t)b)];
                kdebug("lockdep: edge %s '%s'#%u -> %s '%s'#%u", ca->kind == LOCKDEP_KIND_MUTEX ? "mutex" : "spin",
                       ca->name, lockdep_node_subclass((uint16_t)a), cb->kind == LOCKDEP_KIND_MUTEX ? "mutex" : "spin",
                       cb->name, lockdep_node_subclass((uint16_t)b));
            }
        }
    }
}

void lockdep_get_stats(struct lockdep_stats *out)
{
    *out = g_stats;
    out->classes = g_graph.nr_classes;
    out->edges = g_graph.nr_edges;
}

void lockdep_expect(enum lockdep_report_kind kind)
{
    g_expect = (int)kind;
}

unsigned lockdep_expected_hits(void)
{
    unsigned n = g_expected_hits;
    g_expected_hits = 0;
    return n;
}

#endif /* CONFIG_LOCKDEP */
