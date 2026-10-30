/*
 * sync_sched.c - A deterministic one-CPU scheduler for the host test of
 * wait queues, completions, semaphores and mutexes (tests/host/test_sync.c;
 * roadmap 2.0 SA3).
 *
 * Each fake thread is a pthread, but only the one holding the baton runs:
 * the kernel code under test sees a single CPU, and every switch happens
 * where the kernel would switch (a block, a yield, a thread's end). Which
 * ready thread runs next is drawn from a seeded generator, so a seed is one
 * interleaving and replays exactly. When nothing is ready the fake clock
 * jumps to the earliest pending timer, whose callback runs as an interrupt.
 * Every scheduling event goes into the trace, whose digest the test records.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <kernel/list.h>
#include <kernel/lockdep.h>
#include <kernel/percpu.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "sync_baton.h"
#include "sync_sched.h"

enum fstate { F_RUNNING, F_READY, F_BLOCKED, F_DONE };

struct fthread {
    struct thread t;
    enum fstate st;
    bool queued;
    int id;
    void (*body)(int id);
};

static struct fthread g_ft[SYNC_MAX_THREADS];
static int g_nft;
static bool g_deadlock;
static uint64_t g_rng;
static uint64_t g_now;
static struct percpu g_cpu;

/* --- the trace --- */

static uint64_t g_digest = 0xcbf29ce484222325ull;
static uint64_t g_lines;
static bool g_dump;

void sync_trace(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    g_lines++;
    if (g_dump)
        printf("%s\n", line);
}

uint64_t sync_trace_digest(void) { return g_digest; }
uint64_t sync_trace_lines(void) { return g_lines; }
void sync_trace_dump(bool on) { g_dump = on; }

/* --- the CPU --- */

struct percpu *arch_percpu_get(void) { return &g_cpu; }

static struct fthread *ft_of(struct thread *t) { return (struct fthread *)(void *)t; }   /* `t` is the first member */
static struct fthread *cur(void) { return ft_of(g_cpu.current); }

int sync_self(void) { return cur()->id; }

static uint64_t next_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

uint64_t sync_rand(unsigned n) { return n == 0 ? 0 : next_rand() % n; }

/* --- timers --- */

#define SYNC_MAX_TIMERS 16
static struct {
    struct timer *t;
    timer_fn fn;
    void *arg;
    uint64_t at;
    bool pending;
} g_timers[SYNC_MAX_TIMERS];

static int timer_slot(struct timer *t, bool make)
{
    for (int i = 0; i < SYNC_MAX_TIMERS; i++)
        if (g_timers[i].t == t)
            return i;
    if (!make)
        return -1;
    for (int i = 0; i < SYNC_MAX_TIMERS; i++)
        if (g_timers[i].t == NULL) {
            g_timers[i].t = t;
            return i;
        }
    panic("sync_sched: out of timers");
}

void timer_setup(struct timer *t, timer_fn fn, void *arg)
{
    int i = timer_slot(t, true);
    g_timers[i].fn = fn;
    g_timers[i].arg = arg;
    g_timers[i].pending = false;
}

void timer_start(struct timer *t, uint64_t delay_ns)
{
    int i = timer_slot(t, false);
    g_timers[i].at = g_now + delay_ns;
    g_timers[i].pending = true;
    sync_trace("T%d timer +%llu", sync_self(), (unsigned long long)delay_ns);
}

bool timer_cancel_sync(struct timer *t)
{
    int i = timer_slot(t, false);
    if (i < 0)
        return false;
    bool was = g_timers[i].pending;
    g_timers[i].t = NULL;   /* the timer is on the caller's stack: forget it */
    g_timers[i].pending = false;
    return was;
}

/* Fires the earliest pending timer, as an interrupt; false when none is. */
static bool fire_timer(void)
{
    int best = -1;
    for (int i = 0; i < SYNC_MAX_TIMERS; i++)
        if (g_timers[i].pending && (best < 0 || g_timers[i].at < g_timers[best].at))
            best = i;
    if (best < 0)
        return false;
    g_now = g_timers[best].at;
    g_timers[best].pending = false;
    sync_trace("clock %llu: timer fires", (unsigned long long)g_now);
    g_cpu.irq_depth++;
    g_timers[best].fn(g_timers[best].t, g_timers[best].arg);
    g_cpu.irq_depth--;
    return true;
}

uint64_t clock_now_ns(void) { return g_now; }
uint64_t clock_since_ns(uint64_t then) { return g_now > then ? g_now - then : 0; }

/* --- switching --- */

/* The next thread to run, chosen by the seed among the ready ones; -1 when
 * every thread has ended, or none can run again (a deadlock). */
static int pick(void)
{
    for (;;) {
        int ready[SYNC_MAX_THREADS], n = 0;
        for (int i = 0; i < g_nft; i++)
            if (g_ft[i].queued)
                ready[n++] = i;
        if (n > 0) {
            int k = ready[next_rand() % (unsigned)n];
            g_ft[k].queued = false;
            g_ft[k].st = F_RUNNING;
            return k;
        }
        if (fire_timer())
            continue;
        bool all_done = true;
        for (int i = 0; i < g_nft; i++)
            all_done = all_done && g_ft[i].st == F_DONE;
        if (!all_done) {
            g_deadlock = true;
            sync_trace("deadlock");
        }
        return -1;
    }
}

/* Gives the CPU to the next thread and, unless `self` has ended, waits
 * until it is chosen again. */
static void switch_from(int self)
{
    int next = pick();
    if (next == self) {   /* chosen again at once */
        g_cpu.current = &g_ft[self].t;
        return;
    }
    baton_give(next);
    if (g_ft[self].st == F_DONE)
        return;
    if (!baton_wait(self))
        baton_exit();   /* a deadlock ended the run with this thread blocked */
    g_cpu.current = &g_ft[self].t;
}

void sched_prepare_block_current(void)
{
    struct fthread *c = cur();
    if (c->st != F_READY)   /* a wake from a queue armed earlier in the same wait keeps it READY (S34) */
        c->st = F_BLOCKED;
}

void sched_set_running_current(void)
{
    struct fthread *c = cur();
    c->queued = false;
    c->st = F_RUNNING;
}

void sched_block_current(void)
{
    struct fthread *c = cur();
    if (c->st == F_READY) {   /* woken between its prepare and its block */
        c->queued = false;
        c->st = F_RUNNING;
        return;
    }
    sync_trace("T%d blocks", c->id);
    switch_from(c->id);
    sync_trace("T%d runs", c->id);
}

bool sched_wake(struct thread *t)
{
    struct fthread *f = ft_of(t);
    if (f->st != F_BLOCKED)
        return false;
    f->st = F_READY;
    f->queued = true;
    sync_trace("wake T%d", f->id);
    return true;
}

void sched_reprioritize(struct thread *t, int prio)
{
    sync_trace("T%d priority %d -> %d", ft_of(t)->id, t->priority, prio);
    t->priority = prio;
}

void sync_yield(void)
{
    struct fthread *c = cur();
    c->st = F_READY;
    c->queued = true;
    switch_from(c->id);
}

bool process_kill_pending(void) { return false; }

/* --- lockdep: counted, not checked (the kernel's own tests check it) --- */

static unsigned g_lockdep_events;

void lockdep_acquire_check(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                           bool irqs_on, uintptr_t ip)
{
    (void)lock, (void)class_slot, (void)name, (void)kind, (void)subclass, (void)irqs_on, (void)ip;
    g_lockdep_events++;
}

void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                      bool trylock, bool irqs_on, uintptr_t ip)
{
    (void)lock, (void)class_slot, (void)kind, (void)subclass, (void)irqs_on, (void)ip;
    sync_trace("T%d acquires %s%s", sync_self(), name, trylock ? " (try)" : "");
}

void lockdep_release(const void *lock, unsigned kind, uintptr_t ip, bool irqrestore)
{
    (void)lock, (void)kind, (void)ip, (void)irqrestore;
    sync_trace("T%d releases", sync_self());
}

void lockdep_completion_init(uint16_t *spin_slot, const char *name, uintptr_t ip)
{
    (void)spin_slot, (void)name, (void)ip;
    g_lockdep_events++;
}

void lockdep_completion_wait(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)
{
    (void)c, (void)spin_slot, (void)ip;
    sync_trace("T%d waits for %s", sync_self(), name);
}

void lockdep_completion_signal(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)
{
    (void)c, (void)spin_slot, (void)ip;
    sync_trace("T%d signals %s", sync_self(), name);
}

void lockdep_might_sleep(uintptr_t ip)
{
    (void)ip;
    panic("sync_sched: a sleep where it may not (preempt_count %d, irq_depth %u)", g_cpu.preempt_count,
          g_cpu.irq_depth);
}

/* --- a run --- */

static void *entry(void *arg)
{
    struct fthread *f = arg;
    if (!baton_wait(f->id))
        return NULL;
    g_cpu.current = &f->t;
    f->body(f->id);
    sync_trace("T%d ends", f->id);
    f->st = F_DONE;
    switch_from(f->id);
    return NULL;
}

bool sync_run(uint64_t seed, int n, const int prio[], void (*body)(int id))
{
    if (n > SYNC_MAX_THREADS)
        panic("sync_run: too many threads");
    memset(g_ft, 0, sizeof(g_ft));
    memset(g_timers, 0, sizeof(g_timers));
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_nft = n;
    g_rng = seed * 0x9e3779b97f4a7c15ull + 1;
    g_now = 0;
    g_deadlock = false;
    for (int i = 0; i < n; i++) {
        struct fthread *f = &g_ft[i];
        f->id = i;
        f->body = body;
        f->st = F_READY;
        f->queued = true;
        snprintf(f->t.name, sizeof(f->t.name), "T%d", i);
        f->t.priority = f->t.base_prio = prio[i];
        list_init(&f->t.pi_held);
    }
    baton_reset();
    void *args[SYNC_MAX_THREADS];
    for (int i = 0; i < n; i++)
        args[i] = &g_ft[i];
    baton_start(n, entry, args);
    baton_give(pick());
    baton_join(n);   /* the last thread to end gives the baton to -1 */
    for (int i = 0; i < n; i++)
        if (g_ft[i].t.priority != g_ft[i].t.base_prio)
            sync_trace("T%d ends boosted: %d, base %d", i, g_ft[i].t.priority, g_ft[i].t.base_prio);
    return !g_deadlock;
}

int sync_priority(int id) { return g_ft[id].t.priority; }
