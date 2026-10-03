/* Actual interrupt.c under pthreads. IRQ masking is a host no-op, so
 * writer exclusion must come from the real per-slot lock. Joining all
 * dispatchers before record reuse substitutes for a grace period here;
 * this does not model the kernel's epoch or architecture entry protocol. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
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
static unsigned ready, go, stop, unhandled, syncs;
static const char *const names[] = { "handler-a", "handler-b" };
struct payload { unsigned kind, hits; };
struct worker { unsigned kind; struct payload *arg; int rc; };

unsigned arch_trap_vector_count(void) { return 16; }
void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame)
{
    (void)vector; (void)frame;
    __atomic_fetch_add(&unhandled, 1u, __ATOMIC_RELAXED);
}
void synchronize_quiesce(void) { syncs++; }
void quiesce_count_irq_sync(void) {}
void klog(enum klog_level level, const char *fmt, ...) { (void)level; (void)fmt; }
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...)
{
    (void)frame; (void)fmt; abort();
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
    interrupt_init();
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
    assert(syncs == ROUNDS && !interrupt_handler_name(VECTOR));
    assert(interrupt_unregister_vector(VECTOR) == -ENOENT);
    puts("interrupt: PASS (competing writers, coherent handler/argument, counts, diagnostic readers)");
    return 0;
}
