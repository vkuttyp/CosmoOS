/*
 * lockdep_core.h - The lock-order checker's tables and algorithm as pure
 * inline functions (docs/kernel/lockdep/design.md).
 *
 * No kernel dependency so tests/host/test_lockdep.c drives the same code
 * under the sanitizers. kernel/core/lockdep.c wraps it with the per-CPU
 * and per-thread held stacks, the raw lock and the reports.
 */

#ifndef KERNEL_LOCKDEP_CORE_H
#define KERNEL_LOCKDEP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCKDEP_MAX_CLASSES    320u   /* bounded pool, including one class per run queue (S24) */
#define LOCKDEP_SUBCLASSES     4u
#define LOCKDEP_MAX_NODES      (LOCKDEP_MAX_CLASSES * LOCKDEP_SUBCLASSES)
#define LOCKDEP_NODE_WORDS     (LOCKDEP_MAX_NODES / 64u)
#define LOCKDEP_MAX_HELD       24u   /* per CPU: spinlocks, interrupt context included */
#define LOCKDEP_MAX_HELD_MUTEX 8u    /* per thread */
#define LOCKDEP_CLASS_NAME_MAX 64u  /* including NUL; owned by the graph */
#define LOCKDEP_MAX_TIMER_PROFILES 64u  /* one active callback per possible CPU */
#define LOCKDEP_MAX_TIMER_LOCKS   16u

/* Lock kinds: a mutex and its internal spinlock share a name but are
 * different classes. */
#define LOCKDEP_KIND_SPIN  0u
#define LOCKDEP_KIND_MUTEX 1u

/* Class usage bits. Both at once is a report. */
#define LOCKDEP_USED_IN_IRQ   (1u << 0)   /* acquired with irq_depth > 0 */
#define LOCKDEP_HELD_IRQS_ON  (1u << 1)   /* acquired by spin_lock with interrupts enabled */

/* Held-entry flags. */
#define LOCKDEP_HF_TRYLOCK (1u << 0)
#define LOCKDEP_HF_IN_IRQ  (1u << 1)
#define LOCKDEP_HF_IRQS_ON (1u << 2)
#define LOCKDEP_HF_IRQSAVE (1u << 3)
#define LOCKDEP_HF_IRQSAVE_ON (1u << 4)

struct lock_class {
    char name[LOCKDEP_CLASS_NAME_MAX];
    unsigned kind;
    unsigned usage;
    uintptr_t irq_ip;      /* first acquisition in interrupt context */
    uintptr_t irqs_on_ip;  /* first acquisition with interrupts enabled */
};

struct lockdep_held {
    uint16_t node;         /* class * LOCKDEP_SUBCLASSES + subclass */
    uint8_t flags;
    uintptr_t ip;
    const void *lock;
};

struct lockdep_graph {
    struct lock_class classes[LOCKDEP_MAX_CLASSES];
    unsigned nr_classes;
    unsigned nr_edges;
    uint64_t before[LOCKDEP_MAX_NODES][LOCKDEP_NODE_WORDS];   /* bit b of before[a]: b was taken while a was held */
};

/* Caller holds the graph writer lock for the entire copy. Destination
 * is private, separately allocated storage; it owns metadata as well as
 * edges and may be traversed after the lock is released. */
static inline void lockdep_core_snapshot(const struct lockdep_graph *graph,
                                          struct lockdep_graph *snapshot)
{
    *snapshot = *graph;
}

/* Scratch for the reachability search; the caller serialises its use. */
struct lockdep_scratch {
    uint64_t visited[LOCKDEP_NODE_WORDS];
    uint16_t parent[LOCKDEP_MAX_NODES];
    uint16_t queue[LOCKDEP_MAX_NODES];
};

static inline uint16_t lockdep_node(unsigned class_index, unsigned subclass)
{
    return (uint16_t)(class_index * LOCKDEP_SUBCLASSES + subclass);
}

static inline unsigned lockdep_node_class(uint16_t node)
{
    return node / LOCKDEP_SUBCLASSES;
}

static inline unsigned lockdep_node_subclass(uint16_t node)
{
    return node % LOCKDEP_SUBCLASSES;
}

/* Class index for (name contents, kind), creating it; -1 when full,
 * -2 for an overlong name. Copy before publication: a module's rodata
 * can disappear after its last lock is released. Never truncate keys. */
static inline int lockdep_core_class(struct lockdep_graph *g, const char *name, unsigned kind)
{
    if (name == NULL)
        name = "?";
    unsigned len = 0;
    while (len < LOCKDEP_CLASS_NAME_MAX && name[len] != '\0')
        len++;
    if (len == LOCKDEP_CLASS_NAME_MAX)
        return -2;
    for (unsigned i = 0; i < g->nr_classes; i++) {
        unsigned j = 0;
        while (j < len && g->classes[i].name[j] == name[j])
            j++;
        if (j == len && g->classes[i].name[j] == '\0' && g->classes[i].kind == kind)
            return (int)i;
    }
    if (g->nr_classes == LOCKDEP_MAX_CLASSES)
        return -1;
    unsigned i = g->nr_classes++;
    for (unsigned j = 0; j <= len; j++)
        g->classes[i].name[j] = name[j];
    g->classes[i].kind = kind;
    g->classes[i].usage = 0;
    g->classes[i].irq_ip = g->classes[i].irqs_on_ip = 0;
    return (int)i;
}

static inline bool lockdep_core_has_edge(const struct lockdep_graph *g, uint16_t a, uint16_t b)
{
    return (__atomic_load_n(&g->before[a][b / 64u], __ATOMIC_RELAXED) >> (b % 64u)) & 1u;
}

/* Record "b was taken while a was held". True if the edge is new.
 * Caller must serialize graph writers (the kernel holds raw_lock across
 * cycle checking and insertion). The atomic bitmap update permits unlocked
 * readers; it does not make the check/increment or cycle decision lock-free. */
static inline bool lockdep_core_add_edge(struct lockdep_graph *g, uint16_t a, uint16_t b)
{
    if (lockdep_core_has_edge(g, a, b))
        return false;
    __atomic_fetch_or(&g->before[a][b / 64u], (uint64_t)1 << (b % 64u), __ATOMIC_RELAXED);
    g->nr_edges++;
    return true;
}

/*
 * Is `to` reachable from `from` along recorded edges? Breadth-first over the
 * bitmaps. On success `path` receives the chain from `from` to `to`
 * inclusive (at most `path_max` entries, truncated from the start when
 * longer) and *path_len the number of entries actually stored. The
 * returned length is always safe to use when iterating `path`; callers
 * wanting the whole chain supply LOCKDEP_MAX_NODES entries. With
 * path_max == 0, path may be NULL and reachability is still checked.
 */
static inline bool lockdep_core_reaches(const struct lockdep_graph *g, struct lockdep_scratch *s, uint16_t from,
                                        uint16_t to, uint16_t *path, unsigned path_max, unsigned *path_len)
{
    for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++)
        s->visited[w] = 0;
    unsigned head = 0, tail = 0;
    s->queue[tail++] = from;
    s->visited[from / 64u] |= (uint64_t)1 << (from % 64u);
    s->parent[from] = from;
    bool found = from == to;
    while (head < tail && !found) {
        uint16_t n = s->queue[head++];
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS && !found; w++) {
            uint64_t bits = __atomic_load_n(&g->before[n][w], __ATOMIC_RELAXED) & ~s->visited[w];
            while (bits) {
                unsigned bit = (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                uint16_t m = (uint16_t)(w * 64u + bit);
                s->visited[w] |= (uint64_t)1 << bit;
                s->parent[m] = n;
                s->queue[tail++] = m;
                if (m == to) {
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        *path_len = 0;
        return false;
    }
    /* Walk parents back from `to`, then reverse into `path`. */
    unsigned len = 1;
    for (uint16_t n = to; n != from; n = s->parent[n])
        len++;
    unsigned keep = len < path_max ? len : path_max;
    *path_len = keep;
    unsigned skip = len - keep;   /* drop the oldest entries when truncating */
    uint16_t n = to;
    for (unsigned i = len; i-- > 0;) {
        if (i >= skip)
            path[i - skip] = n;
        n = s->parent[n];
    }
    return true;
}

/* Find a usage-labelled descendant of node, or a labelled predecessor.
 * Predecessor search is a forward multi-source BFS, avoiding a second
 * 200 KiB reverse bitmap. Class usage labels every subclass, but traversal
 * never invents edges between subclasses. Caller holds the graph lock. */
static inline bool lockdep_core_find_usage(const struct lockdep_graph *g, struct lockdep_scratch *s,
                                           uint16_t node, unsigned usage, bool predecessors,
                                           uint16_t *endpoint)
{
    for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++)
        s->visited[w] = 0;
    unsigned head = 0, tail = 0;
    unsigned nr_nodes = g->nr_classes * LOCKDEP_SUBCLASSES;
    for (unsigned i = 0; i < nr_nodes; i++) {
        if (predecessors ? !(__atomic_load_n(&g->classes[lockdep_node_class((uint16_t)i)].usage, __ATOMIC_RELAXED) & usage) : i != node)
            continue;
        s->queue[tail++] = (uint16_t)i;
        s->visited[i / 64u] |= (uint64_t)1 << (i % 64u);
        s->parent[i] = (uint16_t)i;
    }
    while (head < tail) {
        uint16_t n = s->queue[head++];
        if (predecessors ? n == node : (__atomic_load_n(&g->classes[lockdep_node_class(n)].usage, __ATOMIC_RELAXED) & usage) != 0) {
            if (predecessors)
                while (s->parent[n] != n)
                    n = s->parent[n];
            *endpoint = n;
            return true;
        }
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++) {
            uint64_t bits = __atomic_load_n(&g->before[n][w], __ATOMIC_RELAXED) & ~s->visited[w];
            while (bits) {
                unsigned bit = (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                uint16_t m = (uint16_t)(w * 64u + bit);
                s->visited[w] |= (uint64_t)1 << bit;
                s->parent[m] = n;
                s->queue[tail++] = m;
            }
        }
    }
    return false;
}

/* Would adding from -> to connect an IRQ-used class to one acquired with
 * interrupts on? Existing graph is assumed valid. No mutation on failure. */
static inline bool lockdep_core_irq_edge(const struct lockdep_graph *g, struct lockdep_scratch *s,
                                         uint16_t from, uint16_t to, uint16_t *safe, uint16_t *unsafe)
{
    return lockdep_core_find_usage(g, s, from, LOCKDEP_USED_IN_IRQ, true, safe) &&
           lockdep_core_find_usage(g, s, to, LOCKDEP_HELD_IRQS_ON, false, unsafe);
}

/* A trylock in an IRQ cannot wait for the interrupted holder. Conversely,
 * a successful trylock with IRQs on can be interrupted while held. */
static inline unsigned lockdep_core_usage(bool in_irq, bool irqs_on, bool trylock)
{
    return (in_irq && !trylock ? LOCKDEP_USED_IN_IRQ : 0u) |
           (irqs_on ? LOCKDEP_HELD_IRQS_ON : 0u);
}

/* Validate newly observed usage against existing paths before publishing
 * it. Rejection leaves the graph valid even when a self-test consumes the
 * report and continues. Caller serializes both usage and graph writers. */
static inline bool lockdep_core_mark_usage(struct lockdep_graph *g, struct lockdep_scratch *s,
                                           unsigned cls, unsigned add, uintptr_t ip,
                                           uint16_t *safe, uint16_t *unsafe)
{
    struct lock_class *c = &g->classes[cls];
    unsigned before = __atomic_load_n(&c->usage, __ATOMIC_RELAXED);
    unsigned fresh = add & ~before;
    if (((before | add) & (LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON)) ==
        (LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON)) {
        *safe = *unsafe = lockdep_node(cls, 0);
        return false;
    }
    for (unsigned sub = 0; sub < LOCKDEP_SUBCLASSES; sub++) {
        uint16_t n = lockdep_node(cls, sub);
        if ((fresh & LOCKDEP_USED_IN_IRQ) &&
            lockdep_core_find_usage(g, s, n, LOCKDEP_HELD_IRQS_ON, false, unsafe)) {
            *safe = n;
            return false;
        }
        if ((fresh & LOCKDEP_HELD_IRQS_ON) &&
            lockdep_core_find_usage(g, s, n, LOCKDEP_USED_IN_IRQ, true, safe)) {
            *unsafe = n;
            return false;
        }
    }
    if (fresh & LOCKDEP_USED_IN_IRQ)
        c->irq_ip = ip;
    if (fresh & LOCKDEP_HELD_IRQS_ON)
        c->irqs_on_ip = ip;
    __atomic_store_n(&c->usage, before | add, __ATOMIC_RELEASE);
    return true;
}

#endif /* KERNEL_LOCKDEP_CORE_H */
