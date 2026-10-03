/* Actual interrupt.c under pthreads. IRQ masking is a host no-op, so
 * writer exclusion must come from the real per-slot lock. Joining all
 * dispatchers before record reuse substitutes for a grace period here;
 * this does not model the kernel's epoch or architecture entry protocol. */
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <arch/trap.h>

#define VECTOR 7u
#define WORKERS 4u
#define ROUNDS 64u
#define DISPATCHES 256u
static unsigned ready, go, stop, unhandled, syncs, sync_stats;
static unsigned vector_count = 16;
static unsigned sync_vector = UINT_MAX;
/* Panic interception is used only before any worker threads start. */
static jmp_buf panic_jmp;
static enum { PANIC_NONE, PANIC_PLAIN, PANIC_FRAME } expected_panic;
static const struct arch_trap_frame *expected_frame;
static char panic_message[256];
/* Generic dispatch treats frames as opaque; this host-only layout lets us
 * check that the original frame pointer reaches the handler/panic path. */
struct arch_trap_frame { unsigned marker; };
static const char *const names[] = { "handler-a", "handler-b" };
struct payload { unsigned kind, hits; };
struct worker { unsigned kind; struct payload *arg; int rc; };

unsigned arch_trap_vector_count(void) { return vector_count; }
void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame)
{
    (void)vector; (void)frame;
    __atomic_fetch_add(&unhandled, 1u, __ATOMIC_RELAXED);
}
void synchronize_quiesce(void)
{
    if (sync_vector != UINT_MAX)
        assert(!interrupt_handler_name(sync_vector));
    syncs++;
}
void quiesce_count_irq_sync(void)
{
    assert(syncs == sync_stats + 1);
    sync_stats++;
}
void klog(enum klog_level level, const char *fmt, ...) { (void)level; (void)fmt; }
void panic(const char *fmt, ...)
{
    assert(expected_panic == PANIC_PLAIN);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(panic_message, sizeof(panic_message), fmt, ap);
    va_end(ap);
    longjmp(panic_jmp, 1);
}
void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...)
{
    assert(expected_panic == PANIC_FRAME && frame == expected_frame);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(panic_message, sizeof(panic_message), fmt, ap);
    va_end(ap);
    longjmp(panic_jmp, 1);
}
static void handle(unsigned vector, void *arg, unsigned kind)
{
    struct payload *p = arg;
    assert(vector == VECTOR && p->kind == kind);
    __atomic_fetch_add(&p->hits, 1u, __ATOMIC_RELAXED);
}
static void handler_a(unsigned v, struct arch_trap_frame *f, void *arg)
{ (void)f; handle(v, arg, 0); }
static void handler_b(unsigned v, struct arch_trap_frame *f, void *arg)
{ (void)f; handle(v, arg, 1); }
static interrupt_handler_fn handlers[] = { handler_a, handler_b };

static void expect_dispatch_panic(unsigned vector, struct arch_trap_frame *frame)
{
    expected_panic = PANIC_FRAME;
    expected_frame = frame;
    if (setjmp(panic_jmp) == 0) {
        interrupt_dispatch(vector, frame);
        assert(!"out-of-range dispatch returned instead of panicking");
    }
    expected_panic = PANIC_NONE;
    char message[sizeof(panic_message)];
    snprintf(message, sizeof(message), "interrupt: vector %u out of range", vector);
    assert(!strcmp(panic_message, message));
}

static void expect_init_panic(unsigned count)
{
    vector_count = count;
    expected_panic = PANIC_PLAIN;
    if (setjmp(panic_jmp) == 0) {
        interrupt_init();
        assert(!"oversized vector table accepted instead of panicking");
    }
    expected_panic = PANIC_NONE;
    char message[sizeof(panic_message)];
    snprintf(message, sizeof(message),
             "interrupt: architecture reports %u vectors, table holds 1344", count);
    assert(!strcmp(panic_message, message));
    /* A production panic never returns. Reset the partial initialization
     * before using the table again in this single-threaded host test. */
    vector_count = 16;
    interrupt_init();
}

struct boundary_probe {
    unsigned vector, hits;
    struct arch_trap_frame *frame;
};
static void boundary_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    struct boundary_probe *p = arg;
    assert(vector == p->vector && frame == p->frame);
    p->hits++;
}

static void test_boundaries(unsigned count)
{
    vector_count = count;
    interrupt_init();
    struct arch_trap_frame frame = { .marker = 42 };
    struct boundary_probe p = { .vector = count - 1, .frame = &frame };
    sync_vector = p.vector;
    unsigned before_syncs = syncs, before_unhandled = unhandled;
    assert(interrupt_register(p.vector, boundary_handler, &p, NULL) == 0);
    assert(!strcmp(interrupt_handler_name(p.vector), "?"));
    assert(interrupt_register(p.vector, handler_a, NULL, "replacement") == -EBUSY);

    unsigned invalid[] = { count, UINT_MAX };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        unsigned v = invalid[i];
        assert(interrupt_register(v, handler_a, NULL, NULL) == -EINVAL);
        assert(interrupt_unregister(v, handler_a) == -EINVAL);
        assert(interrupt_unregister_vector(v) == -EINVAL);
        assert(interrupt_unregister_sync(v, handler_a) == -EINVAL);
        assert(interrupt_unregister_vector_sync(v) == -EINVAL);
        assert(interrupt_count(v) == 0 && !interrupt_handler_name(v));
        expect_dispatch_panic(v, &frame);
    }
    expect_dispatch_panic(count, NULL);
    assert(unhandled == before_unhandled && interrupt_count(p.vector) == 0);
    assert(interrupt_unregister_sync(p.vector, NULL) == -EINVAL);
    assert(interrupt_unregister_sync(p.vector, handler_a) == -ENOENT);
    assert(syncs == before_syncs && sync_stats == before_syncs);
    /* Rejected mutations must preserve the original registration. */
    interrupt_dispatch(p.vector, &frame);
    assert(p.hits == 1 && interrupt_count(p.vector) == 1);
    assert(!strcmp(interrupt_handler_name(p.vector), "?"));

    assert(interrupt_unregister_sync(p.vector, boundary_handler) == 0);
    assert(syncs == before_syncs + 1 && sync_stats == syncs);
    assert(!interrupt_handler_name(p.vector));
    assert(interrupt_unregister_sync(p.vector, boundary_handler) == -ENOENT);
    assert(interrupt_unregister_vector_sync(p.vector) == -ENOENT);
    assert(syncs == before_syncs + 1 && sync_stats == syncs);
    interrupt_dispatch(p.vector, &frame);
    assert(p.hits == 1 && unhandled == before_unhandled + 1);
    assert(interrupt_count(p.vector) == 2);

    assert(interrupt_register(p.vector, boundary_handler, &p, "boundary") == 0);
    interrupt_dispatch(p.vector, &frame);
    assert(p.hits == 2 && interrupt_count(p.vector) == 3);
    assert(interrupt_unregister_vector_sync(p.vector) == 0);
    assert(!interrupt_handler_name(p.vector));
    assert(syncs == before_syncs + 2 && sync_stats == syncs);
    sync_vector = UINT_MAX;
}
static void rendezvous(void)
{
    __atomic_fetch_add(&ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) sched_yield();
}
static void *registrant(void *arg)
{
    struct worker *w = arg;
    rendezvous();
    w->rc = interrupt_register(VECTOR, handlers[w->kind], w->arg, names[w->kind]);
    return NULL;
}
static void *remover(void *arg)
{
    struct worker *w = arg;
    rendezvous();
    w->rc = interrupt_unregister_vector(VECTOR);
    return NULL;
}
static void *dispatcher(void *arg)
{
    (void)arg;
    rendezvous();
    for (unsigned i = 0; i < DISPATCHES; i++) interrupt_dispatch(VECTOR, NULL);
    return NULL;
}
static void run_workers(void *(*fn)(void *), struct worker *w)
{
    pthread_t t[WORKERS * 2];
    unsigned n = fn == registrant ? WORKERS * 2 : WORKERS;
    ready = go = 0;   /* previous round's workers have all joined */
    for (unsigned i = 0; i < n; i++)
        assert(!pthread_create(&t[i], NULL, i < WORKERS ? fn : dispatcher, &w[i % WORKERS]));
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != n) sched_yield();
    __atomic_store_n(&go, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; i++) assert(!pthread_join(t[i], NULL));
}
static void *diagnostics(void *arg)
{
    (void)arg;
    uint64_t previous = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        uint64_t count = interrupt_count(VECTOR);
        assert(count >= previous);
        previous = count;
        const char *name = interrupt_handler_name(VECTOR);
        assert(!name || !strcmp(name, names[0]) || !strcmp(name, names[1]));
        sched_yield();
    }
    return NULL;
}
int main(void)
{
    /* 1344 is the documented table capacity (I-INT-10). Exercise both
     * a smaller architecture's logical limit and the physical table end. */
    expect_init_panic(1345);
    expect_init_panic(UINT_MAX);
    test_boundaries(16);
    test_boundaries(1344);
    vector_count = 16;
    interrupt_init();
    unhandled = syncs = sync_stats = 0;
    assert(interrupt_register(16, handler_a, NULL, NULL) == -EINVAL);
    assert(interrupt_register(VECTOR, NULL, NULL, NULL) == -EINVAL);
    assert(interrupt_unregister(VECTOR, NULL) == -EINVAL);
    assert(interrupt_unregister_vector(16) == -EINVAL);
    assert(interrupt_count(16) == 0 && !interrupt_handler_name(16));
    interrupt_dispatch(VECTOR, NULL);
    assert(unhandled == 1 && interrupt_count(VECTOR) == 1);
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, diagnostics, NULL));
    for (unsigned round = 0; round < ROUNDS; round++) {
        struct payload p[WORKERS];
        struct worker w[WORKERS];
        for (unsigned i = 0; i < WORKERS; i++) {
            p[i] = (struct payload){ .kind = i % 2 };
            w[i] = (struct worker){ .kind = p[i].kind, .arg = &p[i] };
        }
        unsigned before_unhandled = unhandled;
        run_workers(registrant, w); /* dispatch overlaps publication */
        unsigned successes = 0, winner = 0;
        for (unsigned i = 0; i < WORKERS; i++) {
            assert(w[i].rc == 0 || w[i].rc == -EBUSY);
            if (!w[i].rc) { successes++; winner = i; }
        }
        assert(successes == 1);
        assert(interrupt_unregister(VECTOR, handlers[1 - w[winner].kind]) == -ENOENT);
        interrupt_dispatch(VECTOR, NULL); /* at least one handled call */
        assert(p[winner].hits > 0);
        assert(p[winner].hits + unhandled - before_unhandled == WORKERS * DISPATCHES + 1);
        assert(interrupt_count(VECTOR) == 1 + (round + 1) * (WORKERS * DISPATCHES + 1));
        run_workers(remover, w);
        successes = 0;
        for (unsigned i = 0; i < WORKERS; i++) {
            assert(w[i].rc == 0 || w[i].rc == -ENOENT);
            successes += w[i].rc == 0;
        }
        assert(successes == 1);
        synchronize_irq(VECTOR); /* stub: dispatchers already joined */
    }
    __atomic_store_n(&stop, 1u, __ATOMIC_RELEASE);
    assert(!pthread_join(reader, NULL));
    assert(syncs == ROUNDS && sync_stats == ROUNDS && !interrupt_handler_name(VECTOR));
    assert(interrupt_unregister_vector(VECTOR) == -ENOENT);
    puts("interrupt: PASS (panic boundaries, removal errors, competing writers, coherent handler/argument, counts, diagnostic readers)");
    return 0;
}
