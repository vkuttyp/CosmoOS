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

#define LOCKDEP_MAX_CLASSES    160u   /* the tree has ~95 (both kinds, tests included) */
#define LOCKDEP_SUBCLASSES     4u
#define LOCKDEP_MAX_NODES      (LOCKDEP_MAX_CLASSES * LOCKDEP_SUBCLASSES)
#define LOCKDEP_NODE_WORDS     (LOCKDEP_MAX_NODES / 64u)
#define LOCKDEP_MAX_HELD       24u   /* per CPU: spinlocks, interrupt context included */
#define LOCKDEP_MAX_HELD_MUTEX 8u    /* per thread */

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

struct lock_class {
    const char *name;
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

/* Class index for (name, kind), creating it; -1 when the table is full.
 * The name pointer is the key: one initialisation site, one class. */
static inline int lockdep_core_class(struct lockdep_graph *g, const char *name, unsigned kind)
{
    for (unsigned i = 0; i < g->nr_classes; i++) {
        if (g->classes[i].name == name && g->classes[i].kind == kind)
            return (int)i;
    }
    if (g->nr_classes == LOCKDEP_MAX_CLASSES)
        return -1;
    unsigned i = g->nr_classes++;
    g->classes[i].name = name;
    g->classes[i].kind = kind;
    g->classes[i].usage = 0;
    g->classes[i].irq_ip = g->classes[i].irqs_on_ip = 0;
    return (int)i;
}

static inline bool lockdep_core_has_edge(const struct lockdep_graph *g, uint16_t a, uint16_t b)
{
    return (g->before[a][b / 64u] >> (b % 64u)) & 1u;
}

/* Record "b was taken while a was held". True if the edge is new. */
static inline bool lockdep_core_add_edge(struct lockdep_graph *g, uint16_t a, uint16_t b)
{
    if (lockdep_core_has_edge(g, a, b))
        return false;
    g->before[a][b / 64u] |= (uint64_t)1 << (b % 64u);
    g->nr_edges++;
    return true;
}

/*
 * Is `to` reachable from `from` along recorded edges? Breadth-first over the
 * bitmaps. On success `path` receives the chain from `from` to `to`
 * inclusive (at most `path_max` entries, truncated from the start when
 * longer) and *path_len its length.
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
    bool found = false;
    while (head < tail && !found) {
        uint16_t n = s->queue[head++];
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS && !found; w++) {
            uint64_t bits = g->before[n][w] & ~s->visited[w];
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
    *path_len = len;
    unsigned keep = len < path_max ? len : path_max;
    unsigned skip = len - keep;   /* drop the oldest entries when truncating */
    uint16_t n = to;
    for (unsigned i = len; i-- > 0;) {
        if (i >= skip)
            path[i - skip] = n;
        n = s->parent[n];
    }
    return true;
}

#endif /* KERNEL_LOCKDEP_CORE_H */
