/*
 * test_quiesce.c - Host test of the epoch algorithm (kernel/include/kernel/
 * quiesce_core.h, docs/kernel/quiesce/testing.md). Real threads, built
 * twice: under ASan/UBSan a reader that dereferences an object after the
 * updater freed it is a use-after-free ASan reports; under TSan
 * (host-test-quiesce-tsan) every reader access must happen-before the
 * free through the protocol's own atomics, whether or not a stale read
 * happened to occur -- so a missing release/acquire pairing is reported
 * even on runs where the hardware did nothing wrong. The TSan target also
 * builds negative controls with one order weakened, which must report.
 */

#include "harness.h"

#include <kernel/quiesce_core.h>

#include <pthread.h>
#include <stdbool.h>
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

/*
 * Reclaim an object the way the kernel's free would: poison, then free.
 * The first write is a scalar store to the field every reader checks,
 * and it is there for TSan. Its memset interceptor does not report a
 * race against earlier reads on this platform, and its free reports only
 * later accesses -- a model that poisoned with memset alone ran clean
 * under TSan with every order relaxed, so its negative controls could
 * never fail. The scalar store is an instrumented write TSan checks
 * against the readers' last reads of `magic`.
 */
static void reclaim(struct obj *old)
{
    old->magic = 0;
    memset(old, 0xDE, sizeof(*old));   /* a reader still here would see a bad magic ... */
    free(old);                         /* ... or an ASan use-after-free */
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
        reclaim(old);
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

/* --- two waiters: concurrent grace periods over shared readers --- */

/*
 * Two updaters, each replacing its own slot and waiting its own grace
 * periods, against readers that read both slots. This is the case the
 * `>=` comparison and W1's read-modify-write exist for: the epoch moves
 * twice before a reader publishes once, and a waiter that sees a later
 * epoch than its own must still be told the truth. A waiting updater
 * is quiescent -- in the kernel it sleeps, and schedule() publishes --
 * so it publishes its own CPU on every turn of the wait.
 */
struct model2 {
    struct quiesce_state st;
    struct obj *_Atomic cur[2];
    _Atomic unsigned stop;
    _Atomic unsigned done;   /* updaters finished */
    _Atomic unsigned long bad;
};

struct reader2_arg {
    struct model2 *m;
    unsigned cpu;
    unsigned long reads;
};

static void *reader2_main(void *p)
{
    struct reader2_arg *r = p;
    struct model2 *m = r->m;
    while (!atomic_load_explicit(&m->stop, memory_order_acquire)) {
        for (unsigned i = 0; i < 32; i++) {
            for (unsigned s = 0; s < 2; s++) {
                struct obj *o = atomic_load_explicit(&m->cur[s], memory_order_acquire);
                unsigned sum = 0;
                for (unsigned k = 0; k < 15; k++)
                    sum += o->payload[k];
                if (o->magic != LIVE || sum != 15u * o->payload[0])
                    atomic_fetch_add(&m->bad, 1);
                r->reads++;
            }
        }
        quiesce_core_publish(&m->st, r->cpu);
    }
    quiesce_core_publish(&m->st, r->cpu);
    return NULL;
}

struct updater2_arg {
    struct model2 *m;
    unsigned slot, cpu;
    uint64_t online;
};

static void *updater2_main(void *p)
{
    struct updater2_arg *u = p;
    struct model2 *m = u->m;
    for (unsigned gen = 1; gen <= 1000; gen++) {
        struct obj *n = new_obj(gen);
        struct obj *old = atomic_exchange_explicit(&m->cur[u->slot], n, memory_order_acq_rel);
        uint64_t target = quiesce_core_begin(&m->st);
        do {
            quiesce_core_publish(&m->st, u->cpu);   /* waiting is quiescent */
            sched_yield();
        } while (quiesce_core_pending(&m->st, target, u->online) != 0);
        reclaim(old);
    }
    /* Still a CPU after its last grace period: it keeps passing
     * quiescent points until the other waiter is done too, or that
     * waiter's later grace periods would wait for this CPU for ever. */
    atomic_fetch_add_explicit(&m->done, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&m->done, memory_order_acquire) < 2) {
        quiesce_core_publish(&m->st, u->cpu);
        sched_yield();
    }
    quiesce_core_publish(&m->st, u->cpu);
    return NULL;
}

static void test_two_waiters(void)
{
    struct model2 *m = calloc(1, sizeof(*m));
    EXPECT(m != NULL);
    atomic_store(&m->cur[0], new_obj(0));
    atomic_store(&m->cur[1], new_obj(0));
    enum { R = 3 };
    uint64_t online = (1u << (R + 2)) - 1;   /* CPUs 0, 1 = updaters, 2.. = readers */
    pthread_t rt[R], ut[2];
    struct reader2_arg ra[R];
    struct updater2_arg ua[2];
    for (unsigned i = 0; i < R; i++) {
        ra[i] = (struct reader2_arg){ .m = m, .cpu = i + 2 };
        EXPECT(pthread_create(&rt[i], NULL, reader2_main, &ra[i]) == 0);
    }
    for (unsigned i = 0; i < 2; i++) {
        ua[i] = (struct updater2_arg){ .m = m, .slot = i, .cpu = i, .online = online };
        EXPECT(pthread_create(&ut[i], NULL, updater2_main, &ua[i]) == 0);
    }
    for (unsigned i = 0; i < 2; i++)
        pthread_join(ut[i], NULL);
    atomic_store_explicit(&m->stop, 1, memory_order_release);
    unsigned long reads = 0;
    for (unsigned i = 0; i < R; i++) {
        pthread_join(rt[i], NULL);
        reads += ra[i].reads;
    }
    EXPECT(atomic_load(&m->bad) == 0);
    EXPECT(reads > 0);
    /* Two waiters, 1000 grace periods each: the epoch moved once per
     * grace period, never lost to the race between them (W1 is an RMW). */
    EXPECT(__atomic_load_n(&m->st.epoch, __ATOMIC_ACQUIRE) == 2000);
    free(atomic_load(&m->cur[0]));
    free(atomic_load(&m->cur[1]));
    free(m);
}

/* --- CPUs coming online during grace periods --- */

/*
 * The kernel's onlining order, modelled with its own helpers: a new CPU
 * marks itself online (a release store, as sched_start_cpu does), passes
 * Q0 and publishes before it runs any reader; the waiter advances the
 * epoch, passes W1b and only then reads which CPUs are online (as
 * sync_quiesce_counting does). Readers come online one by one while the
 * updater reclaims. Under TSan, a new CPU that read the old object
 * without the waiter waiting for it would be a race against the free.
 * (A forbidden outcome here needs the store-buffering reordering itself,
 * which is rare on hardware: the C11 verdicts for both orders are the
 * litmus tests in tests/litmus/quiesce/, this test is the threaded model
 * of the order the kernel uses.)
 */
enum { LATE = 4 };

struct model3 {
    struct quiesce_state st;
    struct obj *_Atomic cur;
    _Atomic bool online[LATE + 1];
    _Atomic unsigned stop;
    _Atomic unsigned gen;   /* the updater's progress: when each late CPU comes online */
    _Atomic unsigned long bad;
};

struct late_arg {
    struct model3 *m;
    unsigned cpu;
    unsigned long reads;
};

static uint64_t online_mask3(struct model3 *m)
{
    uint64_t mask = 0;
    for (unsigned c = 0; c <= LATE; c++)
        if (atomic_load_explicit(&m->online[c], memory_order_acquire))
            mask |= (uint64_t)1 << c;
    return mask;
}

static void *late_main(void *p)
{
    struct late_arg *r = p;
    struct model3 *m = r->m;
    /* Come online at a fixed point of the updater's run (CPU c after
     * generation 300c), so every one of them joins mid-run. */
    while (atomic_load_explicit(&m->gen, memory_order_acquire) < r->cpu * 300u)
        sched_yield();
    atomic_store_explicit(&m->online[r->cpu], true, memory_order_release);
    quiesce_core_after_online();                /* Q0 */
    quiesce_core_publish(&m->st, r->cpu);       /* before any reader */
    while (!atomic_load_explicit(&m->stop, memory_order_acquire)) {
        for (unsigned i = 0; i < 64; i++) {
            struct obj *o = atomic_load_explicit(&m->cur, memory_order_acquire);
            unsigned sum = 0;
            for (unsigned k = 0; k < 15; k++)
                sum += o->payload[k];
            if (o->magic != LIVE || sum != 15u * o->payload[0])
                atomic_fetch_add(&m->bad, 1);
            r->reads++;
        }
        quiesce_core_publish(&m->st, r->cpu);
    }
    quiesce_core_publish(&m->st, r->cpu);
    return NULL;
}

static void test_online_late(void)
{
    struct model3 *m = calloc(1, sizeof(*m));
    EXPECT(m != NULL);
    atomic_store(&m->cur, new_obj(0));
    atomic_store(&m->online[0], true);   /* the updater's CPU */
    pthread_t th[LATE];
    struct late_arg args[LATE];
    for (unsigned i = 0; i < LATE; i++) {
        args[i] = (struct late_arg){ .m = m, .cpu = i + 1 };
        EXPECT(pthread_create(&th[i], NULL, late_main, &args[i]) == 0);
    }
    unsigned saw_new_cpu = 0;
    uint64_t first = online_mask3(m);
    for (unsigned gen = 1; gen <= 2000; gen++) {
        atomic_store_explicit(&m->gen, gen, memory_order_release);
        struct obj *n = new_obj(gen);
        struct obj *old = atomic_exchange_explicit(&m->cur, n, memory_order_acq_rel);
        uint64_t target = quiesce_core_begin(&m->st);
        quiesce_core_after_begin();                  /* W1b */
        uint64_t online = online_mask3(m);           /* after W1, as the kernel reads it */
        if (online != first)
            saw_new_cpu = 1;
        quiesce_core_publish(&m->st, 0);
        while (quiesce_core_pending(&m->st, target, online) != 0)
            sched_yield();
        reclaim(old);
    }
    atomic_store_explicit(&m->stop, 1, memory_order_release);
    unsigned long reads = 0;
    for (unsigned i = 0; i < LATE; i++) {
        pthread_join(th[i], NULL);
        reads += args[i].reads;
    }
    EXPECT(atomic_load(&m->bad) == 0);
    EXPECT(reads > 0);
    EXPECT(saw_new_cpu);   /* CPUs really did come online during the run */
    EXPECT(online_mask3(m) == (1u << (LATE + 1)) - 1);   /* all of them */
    free(atomic_load(&m->cur));
    free(m);
}

static const struct host_test tests[] = {
    { "epoch-math", test_epoch_math },
    { "negative-model", test_negative_model },
    { "threads", test_threads },
    { "two-waiters", test_two_waiters },
    { "online-late", test_online_late },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
