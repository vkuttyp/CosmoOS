/* Shared graph protocol: real core helpers, a raw acquire/release writer
 * lock, and unlocked diagnostic readers. This models graph publication,
 * not interrupt entry, held stacks, callback lifetime, or NMI reentrancy. */
#include <kernel/lockdep_core.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRITERS 4u
#define READERS 2u
#define CLASSES 32u
#define ROUNDS 2048u

struct model {
    struct lockdep_graph graph;
    struct lockdep_scratch scratch;
    unsigned raw, ready, start;
    unsigned rejected_cycles, rejected_irq;
};
struct arg { struct model *m; unsigned id; unsigned observations; };

static void take(struct model *m)
{
    while (__atomic_exchange_n(&m->raw, 1u, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void drop(struct model *m)
{
    __atomic_store_n(&m->raw, 0u, __ATOMIC_RELEASE);
}
static void rendezvous(struct model *m)
{
    __atomic_fetch_add(&m->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&m->start, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void *writer(void *opaque)
{
    struct arg *a = opaque;
    struct model *m = a->m;
    unsigned rng = a->id + 1;
    rendezvous(m);
    for (unsigned i = 0; i < ROUNDS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "concurrent-%u", i % CLASSES);
        take(m);
        int cls = lockdep_core_class(&m->graph, name, LOCKDEP_KIND_SPIN);
        assert(cls >= 0);
        /* Publish usage while readers sample it atomically, as in kernel. */
        uint16_t safe, unsafe;
        unsigned usage = ((i + i / CLASSES) & 1u) ? LOCKDEP_USED_IN_IRQ : LOCKDEP_HELD_IRQS_ON;
        if (!lockdep_core_mark_usage(&m->graph, &m->scratch, (unsigned)cls,
                                     usage, i + 1u, &safe, &unsafe))
            m->rejected_irq++;
        rng = rng * 1664525u + 1013904223u;
        uint16_t from = lockdep_node((rng >> 16) % m->graph.nr_classes, 0);
        rng = rng * 1664525u + 1013904223u;
        uint16_t to = lockdep_node((rng >> 16) % m->graph.nr_classes, 0);
        unsigned path_len;
        if (lockdep_core_has_edge(&m->graph, from, to)) {
            /* Duplicate insertion must not increase the edge count. */
            assert(!lockdep_core_add_edge(&m->graph, from, to));
        } else if (lockdep_core_reaches(&m->graph, &m->scratch, to, from,
                                        NULL, 0, &path_len)) {
            m->rejected_cycles++;
        } else if (lockdep_core_irq_edge(&m->graph, &m->scratch, from, to, &safe, &unsafe)) {
            m->rejected_irq++;
        } else {
            assert(lockdep_core_add_edge(&m->graph, from, to));
        }
        drop(m);
        if (!(i % 16)) sched_yield();
    }
    return NULL;
}
/* Validate a private snapshot outside the writer lock while the source
 * may grow. Its edge total and metadata range must describe one graph. */
static void check_snapshot(const struct lockdep_graph *g)
{
    unsigned edges = 0;
    unsigned nodes = g->nr_classes * LOCKDEP_SUBCLASSES;
    for (unsigned c = 0; c < g->nr_classes; c++) {
        assert(g->classes[c].kind == LOCKDEP_KIND_SPIN);
        assert(strncmp(g->classes[c].name, "concurrent-", 11) == 0);
    }
    for (unsigned a = 0; a < LOCKDEP_MAX_NODES; a++)
        for (unsigned w = 0; w < LOCKDEP_NODE_WORDS; w++) {
            uint64_t bits = g->before[a][w];
            edges += (unsigned)__builtin_popcountll(bits);
            while (bits) {
                unsigned b = w * 64u + (unsigned)__builtin_ctzll(bits);
                assert(a < nodes && b < nodes);
                bits &= bits - 1;
            }
        }
    assert(edges == g->nr_edges);
}

static void *reader(void *opaque)
{
    struct arg *a = opaque;
    struct model *m = a->m;
    rendezvous(m);
    /* Do not let a reader exhaust its rounds before a writer is first
     * scheduled. There must be a published graph to sample. */
    for (;;) {
        take(m);
        unsigned edges = m->graph.nr_edges;
        drop(m);
        if (edges) break;
        sched_yield();
    }
    struct lockdep_graph *snapshot = malloc(sizeof(*snapshot));
    assert(snapshot);
    for (unsigned i = 0; i < ROUNDS; i++) {
        if (!(i % 128)) {
            take(m);
            lockdep_core_snapshot(&m->graph, snapshot);
            drop(m);
            check_snapshot(snapshot);
        }
        /* Also retain the unlocked-reader publication model: capture the
         * range, then inspect immutable metadata and atomic bitmaps. */
        take(m);
        unsigned count = m->graph.nr_classes;
        drop(m);
        for (unsigned c = 0; c < count; c++) {
            const struct lock_class *cls = &m->graph.classes[c];
            assert(cls->kind == LOCKDEP_KIND_SPIN);
            assert(strncmp(cls->name, "concurrent-", 11) == 0);
            unsigned usage = __atomic_load_n(&cls->usage, __ATOMIC_ACQUIRE);
            assert(usage != (LOCKDEP_USED_IN_IRQ | LOCKDEP_HELD_IRQS_ON));
            for (unsigned d = 0; d < count; d++)
                a->observations += lockdep_core_has_edge(&m->graph,
                                                         lockdep_node(c, 0), lockdep_node(d, 0));
        }
        sched_yield();
    }
    free(snapshot);
    return NULL;
}
struct held_model {
    struct lockdep_held held[LOCKDEP_MAX_HELD];
    uint64_t seq;
    unsigned count, done;
};

static void check_held_copy(const struct lockdep_held *held, unsigned count)
{
    assert(count <= LOCKDEP_MAX_HELD);
    if (!count) return;
    uintptr_t generation = held[0].ip;
    for (unsigned i = 0; i < count; i++) {
        assert(held[i].ip == generation);
        assert(held[i].node == (generation + i) % LOCKDEP_MAX_NODES);
        assert(held[i].flags == (uint8_t)(generation + i));
        assert(held[i].lock == (const void *)(generation + i + 1u));
    }
}

static void *held_writer(void *opaque)
{
    struct held_model *m = opaque;
    for (unsigned gen = 1; gen <= ROUNDS; gen++) {
        unsigned count = 1u + gen % LOCKDEP_MAX_HELD;
        lockdep_core_held_begin(&m->seq);
        for (unsigned i = 0; i < count; i++) {
            struct lockdep_held e = {
                .node = (uint16_t)((gen + i) % LOCKDEP_MAX_NODES),
                .flags = (uint8_t)(gen + i), .ip = gen,
                .lock = (const void *)(uintptr_t)(gen + i + 1u),
            };
            lockdep_core_held_store(&m->held[i], &e);
        }
        __atomic_store_n(&m->count, count, __ATOMIC_SEQ_CST);
        lockdep_core_held_end(&m->seq);
        sched_yield();
    }
    __atomic_store_n(&m->done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

static void *held_reader(void *opaque)
{
    struct held_model *m = opaque;
    struct lockdep_held copy[LOCKDEP_MAX_HELD];
    unsigned count;
    do {
        if (lockdep_core_held_snapshot(&m->seq, m->held, &m->count, copy, &count))
            check_held_copy(copy, count);
    } while (!__atomic_load_n(&m->done, __ATOMIC_ACQUIRE));
    return NULL;
}

static void test_held_snapshots(void)
{
    struct held_model m = {0};
    struct lockdep_held copy[LOCKDEP_MAX_HELD];
    unsigned count;
    assert(lockdep_core_held_snapshot(&m.seq, m.held, &m.count, copy, &count));
    assert(count == 0);
    /* A stopped CPU mid-update must return immediately, not spin. */
    lockdep_core_held_begin(&m.seq);
    assert(!lockdep_core_held_snapshot(&m.seq, m.held, &m.count, copy, &count));
    assert(count == 0);
    lockdep_core_held_end(&m.seq);
    m.count = LOCKDEP_MAX_HELD + 1u;
    assert(!lockdep_core_held_snapshot(&m.seq, m.held, &m.count, copy, &count));
    assert(count == 0);
    m.count = 0;
    pthread_t w, r[READERS];
    for (unsigned i = 0; i < READERS; i++)
        assert(pthread_create(&r[i], NULL, held_reader, &m) == 0);
    assert(pthread_create(&w, NULL, held_writer, &m) == 0);
    assert(pthread_join(w, NULL) == 0);
    for (unsigned i = 0; i < READERS; i++)
        assert(pthread_join(r[i], NULL) == 0);
    assert(lockdep_core_held_snapshot(&m.seq, m.held, &m.count, copy, &count));
    assert(count > 0);
    check_held_copy(copy, count);
    puts("lockdep-held-snapshot: PASS (busy, bounds, concurrent generations)");
}

int main(void)
{
    struct model *m = calloc(1, sizeof(*m));
    assert(m);
    pthread_t threads[WRITERS + READERS];
    struct arg args[WRITERS + READERS];
    for (unsigned i = 0; i < WRITERS + READERS; i++) {
        args[i] = (struct arg){ .m = m, .id = i };
        assert(pthread_create(&threads[i], NULL, i < WRITERS ? writer : reader, &args[i]) == 0);
    }
    while (__atomic_load_n(&m->ready, __ATOMIC_ACQUIRE) != WRITERS + READERS)
        sched_yield();
    __atomic_store_n(&m->start, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < WRITERS + READERS; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(m->graph.nr_classes == CLASSES);
    assert(m->rejected_cycles && m->rejected_irq);
    for (unsigned i = WRITERS; i < WRITERS + READERS; i++)
        assert(args[i].observations);

    /* Independent closure oracle: every accepted edge must leave a DAG,
     * accurate counts, and no IRQ-used -> IRQ-enabled path. */
    bool reach[CLASSES][CLASSES] = {0};
    unsigned edges = 0;
    for (unsigned a = 0; a < CLASSES; a++)
        for (unsigned b = 0; b < CLASSES; b++) {
            reach[a][b] = lockdep_core_has_edge(&m->graph, lockdep_node(a, 0), lockdep_node(b, 0));
            edges += reach[a][b];
        }
    assert(edges == m->graph.nr_edges && edges > 0);
    for (unsigned k = 0; k < CLASSES; k++)
        for (unsigned a = 0; a < CLASSES; a++)
            for (unsigned b = 0; b < CLASSES; b++)
                reach[a][b] |= reach[a][k] && reach[k][b];
    for (unsigned a = 0; a < CLASSES; a++) {
        assert(!reach[a][a]);
        for (unsigned b = 0; b < CLASSES; b++)
            assert(!reach[a][b] || !(m->graph.classes[a].usage & LOCKDEP_USED_IN_IRQ) ||
                                  !(m->graph.classes[b].usage & LOCKDEP_HELD_IRQS_ON));
    }
    free(m);
    test_held_snapshots();
    puts("lockdep-threads: PASS (publication, writers, readers, closure oracle)");
    return 0;
}
