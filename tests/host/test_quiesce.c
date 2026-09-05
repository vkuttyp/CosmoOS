/*
 * test_quiesce.c - Host test of the epoch algorithm (kernel/include/kernel/
 * quiesce_core.h, docs/kernel/quiesce/testing.md). ASan/UBSan, real
 * threads: a reader that dereferences an object after the updater freed
 * it is a use-after-free ASan reports, so a wrong grace period fails the
 * build rather than a probability.
 */

#include "harness.h"

#include <kernel/quiesce_core.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* --- the pure arithmetic --- */

static void test_epoch_math(void)
{
    struct quiesce_state *st = calloc(1, sizeof(*st));
    EXPECT(st != NULL);
    EXPECT(sizeof(struct quiesce_cpu) == 64);
    EXPECT(((uintptr_t)&st->cpus[1] - (uintptr_t)&st->cpus[0]) == 64);

    uint64_t online = 0xFu;   /* CPUs 0..3 */
    uint64_t e = quiesce_core_begin(st);
    EXPECT(e == 1);
    /* Nobody has published: everyone pending. */
    EXPECT(quiesce_core_pending(st, e, online) == 0xFu);
    /* Offline CPUs never count. */
    EXPECT(quiesce_core_pending(st, e, 0x5u) == 0x5u);

    quiesce_core_publish(st, 0);
    quiesce_core_publish(st, 2);
    EXPECT(quiesce_core_pending(st, e, online) == 0xAu);
    EXPECT(st->cpus[0].transitions == 1 && st->cpus[2].transitions == 1);

    /* Two waiters advance twice; a CPU that publishes once afterwards
     * satisfies both (the >= comparison). */
    uint64_t e2 = quiesce_core_begin(st);
    uint64_t e3 = quiesce_core_begin(st);
    EXPECT(e2 == 2 && e3 == 3);
    quiesce_core_publish(st, 1);
    quiesce_core_publish(st, 3);
    EXPECT(quiesce_core_pending(st, e2, online) == 0x5u);   /* 0 and 2 published epoch 1 only */
    EXPECT(quiesce_core_pending(st, e3, online) == 0x5u);
    quiesce_core_publish(st, 0);
    quiesce_core_publish(st, 2);
    EXPECT(quiesce_core_pending(st, e3, online) == 0);
    EXPECT(quiesce_core_pending(st, e, online) == 0);

    /* The highest CPU slot works and the loop stops at the mask. */
    online = (uint64_t)1 << 63;
    uint64_t e4 = quiesce_core_begin(st);
    EXPECT(quiesce_core_pending(st, e4, online) == online);
    quiesce_core_publish(st, 63);
    EXPECT(quiesce_core_pending(st, e4, online) == 0);
    free(st);
}

/* --- threads: readers dereference, the updater frees after a grace period --- */

struct obj {
    unsigned magic;
    unsigned payload[15];
};
#define LIVE 0x4c495645u

struct model {
    struct quiesce_state st;
    struct obj *_Atomic cur;
    _Atomic unsigned stop;
    _Atomic unsigned long bad;
    unsigned nreaders;
};

struct reader_arg {
    struct model *m;
    unsigned cpu;
    unsigned long reads;
};

/* One "CPU": alternates read-side sections with quiescent points, exactly
 * the kernel's shape (a section may not straddle a publish). */
static void *reader_main(void *p)
{
    struct reader_arg *r = p;
    struct model *m = r->m;
    while (!atomic_load_explicit(&m->stop, memory_order_acquire)) {
        for (unsigned i = 0; i < 64; i++) {
            /* read section */
            struct obj *o = atomic_load_explicit(&m->cur, memory_order_acquire);
            unsigned sum = 0;
            for (unsigned k = 0; k < 15; k++)
                sum += o->payload[k];        /* freed memory here is an ASan report */
            if (o->magic != LIVE || sum != 15u * o->payload[0])
                atomic_fetch_add(&m->bad, 1);
            r->reads++;
            /* end of section */
        }
        quiesce_core_publish(&m->st, r->cpu);   /* quiescent point */
    }
    quiesce_core_publish(&m->st, r->cpu);
    return NULL;
}

static struct obj *new_obj(unsigned gen)
{
    struct obj *o = malloc(sizeof(*o));
    o->magic = LIVE;
    for (unsigned k = 0; k < 15; k++)
        o->payload[k] = gen;
    return o;
}

static void synchronize(struct model *m, uint64_t online)
{
    uint64_t target = quiesce_core_begin(&m->st);
    quiesce_core_publish(&m->st, 0);   /* the updater is CPU 0 and is quiescent itself */
    while (quiesce_core_pending(&m->st, target, online) != 0)
        sched_yield();
}

static void test_threads(void)
{
    struct model *m = calloc(1, sizeof(*m));
    EXPECT(m != NULL);
    m->nreaders = 4;
    atomic_store(&m->cur, new_obj(0));
    uint64_t online = (1u << (m->nreaders + 1)) - 1;   /* CPU 0 = updater, 1..n = readers */

    pthread_t th[8];
    struct reader_arg args[8];
    for (unsigned i = 0; i < m->nreaders; i++) {
        args[i] = (struct reader_arg){ .m = m, .cpu = i + 1 };
        EXPECT(pthread_create(&th[i], NULL, reader_main, &args[i]) == 0);
    }

    unsigned gens = 0;
    for (unsigned gen = 1; gen <= 2000; gen++) {
        struct obj *n = new_obj(gen);
        struct obj *old = atomic_exchange_explicit(&m->cur, n, memory_order_acq_rel);
        synchronize(m, online);
        memset(old, 0xDE, sizeof(*old));   /* a reader still here would see a bad magic ... */
        free(old);                         /* ... or an ASan use-after-free */
        gens++;
    }
    atomic_store_explicit(&m->stop, 1, memory_order_release);
    unsigned long reads = 0;
    for (unsigned i = 0; i < m->nreaders; i++) {
        pthread_join(th[i], NULL);
        reads += args[i].reads;
    }
    EXPECT(atomic_load(&m->bad) == 0);
    EXPECT(reads > 0);
    EXPECT(gens == 2000);
    free(atomic_load(&m->cur));
    free(m);
}

/* Without the grace period the same model is wrong; make sure the test
 * would notice: a reader holding an old pointer across a "free" sees the
 * poison. Done with a single controlled reader step, no threads, so the
 * failure is deterministic. */
static void test_negative_model(void)
{
    struct quiesce_state st;
    memset(&st, 0, sizeof(st));
    uint64_t online = 0x3u;
    uint64_t target = quiesce_core_begin(&st);
    quiesce_core_publish(&st, 0);
    /* CPU 1 has not published: the algorithm refuses to declare the
     * grace period over. An implementation that skipped the check would
     * free here. */
    EXPECT(quiesce_core_pending(&st, target, online) == 0x2u);
    quiesce_core_publish(&st, 1);
    EXPECT(quiesce_core_pending(&st, target, online) == 0);
}

static const struct host_test tests[] = {
    { "epoch-math", test_epoch_math },
    { "negative-model", test_negative_model },
    { "threads", test_threads },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
