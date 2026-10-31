/*
 * test_fair.c - The fair level's arithmetic (kernel/scheduler/fair.c,
 * generated from CeeSafe) on the host: Linux's weights, the exact weighted
 * charge against 128-bit arithmetic, placement, rebasing, the order and
 * the preemption rule; then one CPU simulated tick by tick with the
 * policy's rules (policy_rr.c's fair level) over these functions, where
 * each thread's share must follow its weight and a sleeper must not
 * monopolise the CPU when it wakes.
 */

#include "harness.h"

#include <kernel/fair.h>


#include <string.h>

/* Linux's kernel/sched/core.c sched_prio_to_weight, copied separately. */
static const uint32_t linux_weights[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static void test_weights(void)
{
    for (int n = -20; n <= 19; n++)
        EXPECT(fair_weight(n) == linux_weights[n + 20]);
    EXPECT(fair_weight(0) == FAIR_NICE_0_WEIGHT);
    EXPECT(fair_weight(-21) == 88761 && fair_weight(INT32_MIN) == 88761);
    EXPECT(fair_weight(20) == 15 && fair_weight(INT32_MAX) == 15);
}

static uint64_t ref_charge(uint64_t vr, uint64_t delta, uint32_t w)
{
    unsigned __int128 add = (unsigned __int128)delta * 1024u / (w == 0 ? 1u : w);
    unsigned __int128 sum = (unsigned __int128)vr + add;
    return sum > UINT64_MAX ? UINT64_MAX : (uint64_t)sum;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void test_charge(void)
{
    static const uint64_t deltas[] = { 0, 1, 14, 15, 1023, 1024, 4000000, 10000000, 1ull << 40, 1ull << 54,
                                       1ull << 58, 1ull << 60, UINT64_MAX / 1024, UINT64_MAX - 1, UINT64_MAX };
    static const uint64_t vrs[] = { 0, 1, 5000000, 1ull << 62, UINT64_MAX - 100, UINT64_MAX };
    for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); d++)
        for (unsigned v = 0; v < sizeof(vrs) / sizeof(vrs[0]); v++)
            for (unsigned w = 0; w < 40; w++)
                EXPECT(fair_charge(vrs[v], deltas[d], linux_weights[w]) ==
                       ref_charge(vrs[v], deltas[d], linux_weights[w]));
    EXPECT(fair_charge(7, 100, 0) == ref_charge(7, 100, 0));   /* a weight of 0 is taken as 1 */
    for (unsigned i = 0; i < 200000; i++) {
        uint64_t vr = rnd() >> (rnd() & 63), delta = rnd() >> (rnd() & 63);
        uint32_t w = (uint32_t)(rnd() % 100000) + 1;
        EXPECT(fair_charge(vr, delta, w) == ref_charge(vr, delta, w));
    }
    /* Nice 0 is one to one. */
    EXPECT(fair_charge(100, 4000000, 1024) == 4000100);
}

static void test_place_rebase_order(void)
{
    EXPECT(fair_place(0, 100, 10) == 90);                 /* a long sleeper starts just behind */
    EXPECT(fair_place(95, 100, 10) == 95);                /* a recent one keeps its own */
    EXPECT(fair_place(500, 100, 10) == 500);              /* a thread ahead stays ahead */
    EXPECT(fair_place(0, 5, 10) == 0);                    /* no floor below 0 */
    EXPECT(fair_rebase(130, 100, 1000) == 1030);          /* a lead is kept */
    EXPECT(fair_rebase(80, 100, 1000) == 980);            /* so is a lag */
    EXPECT(fair_rebase(80, 100, 10) == 0);                /* saturating below */
    EXPECT(fair_rebase(UINT64_MAX, 0, 5) == UINT64_MAX);  /* and above */
    EXPECT(fair_before(1, 2) && !fair_before(2, 1) && !fair_before(2, 2));
    EXPECT(!fair_should_preempt(110, 100, 10) && fair_should_preempt(111, 100, 10));
    EXPECT(!fair_should_preempt(UINT64_MAX, UINT64_MAX - 5, 10));
    EXPECT(fair_min(50, true, 70, true, 60) == 60);
    EXPECT(fair_min(50, true, 40, true, 60) == 50);       /* never backwards */
    EXPECT(fair_min(50, false, 0, true, 80) == 80 && fair_min(50, true, 90, false, 0) == 90);
    EXPECT(fair_min(50, false, 0, false, 0) == 50);
}

/*
 * One CPU, ticks of TICK: the running thread is charged its tick, then
 * gives way if a ready thread is more than the granularity behind; the
 * ready list is ordered by fair_before with ties in arrival order; a
 * thread joining is placed with fair_place against the queue's min.
 */
#define SIM_MAX 8
#define TICK 4000000ull

struct sim_thread {
    int nice;
    uint64_t vruntime;
    uint64_t ran;
    bool ready;      /* on the ready list (not running, not asleep) */
    bool asleep;
    uint64_t order;  /* arrival stamp, for ties */
};

struct sim {
    struct sim_thread t[SIM_MAX];
    unsigned n;
    int current;     /* -1: idle */
    uint64_t min;
    uint64_t stamp;
};

static int sim_leftmost(const struct sim *s)
{
    int best = -1;
    for (unsigned i = 0; i < s->n; i++) {
        if (!s->t[i].ready)
            continue;
        if (best < 0 || fair_before(s->t[i].vruntime, s->t[best].vruntime) ||
            (s->t[i].vruntime == s->t[best].vruntime && s->t[i].order < s->t[best].order))
            best = (int)i;
    }
    return best;
}

static void sim_update_min(struct sim *s)
{
    int l = sim_leftmost(s);
    s->min = fair_min(s->min, s->current >= 0, s->current >= 0 ? s->t[s->current].vruntime : 0, l >= 0,
                      l >= 0 ? s->t[l].vruntime : 0);
}

static void sim_enqueue(struct sim *s, unsigned i, bool joining)
{
    if (joining)
        s->t[i].vruntime = fair_place(s->t[i].vruntime, s->min, FAIR_WAKE_BONUS_NS);
    s->t[i].ready = true;
    s->t[i].order = s->stamp++;
}

static void sim_pick(struct sim *s)
{
    int l = sim_leftmost(s);
    s->current = l;
    if (l >= 0)
        s->t[l].ready = false;
    sim_update_min(s);
}

static void sim_tick(struct sim *s)
{
    if (s->current < 0) {
        sim_pick(s);
        return;
    }
    struct sim_thread *c = &s->t[s->current];
    c->ran += TICK;
    c->vruntime = fair_charge(c->vruntime, TICK, fair_weight(c->nice));
    sim_update_min(s);
    int l = sim_leftmost(s);
    if (l >= 0 && fair_should_preempt(c->vruntime, s->t[l].vruntime, FAIR_GRANULARITY_NS)) {
        sim_enqueue(s, (unsigned)s->current, false);
        sim_pick(s);
    }
}

static void sim_init(struct sim *s, const int *nices, unsigned n)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    s->current = -1;
    for (unsigned i = 0; i < n; i++) {
        s->t[i].nice = nices[i];
        sim_enqueue(s, i, true);
    }
}

/* Every thread's share against its weight's, within `pct` percent of the whole. */
static bool shares_follow_weights(const struct sim *s, unsigned pct)
{
    uint64_t total = 0, wsum = 0;
    for (unsigned i = 0; i < s->n; i++) {
        total += s->t[i].ran;
        wsum += fair_weight(s->t[i].nice);
    }
    for (unsigned i = 0; i < s->n; i++) {
        double want = (double)fair_weight(s->t[i].nice) / (double)wsum;
        double got = (double)s->t[i].ran / (double)total;
        if (got < want - pct / 100.0 || got > want + pct / 100.0)
            return false;
    }
    return true;
}

static void test_simulated_shares(void)
{
    static const int cases[][4] = { { 0, 0, 0, 0 }, { 0, 5, 0, 5 }, { -5, 0, 5, 10 }, { 0, 19, 19, 19 },
                                    { -20, 0, 0, 0 }, { 3, 3, 3, -3 } };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct sim s;
        sim_init(&s, cases[c], 4);
        for (unsigned k = 0; k < 100000; k++)   /* 400 s simulated */
            sim_tick(&s);
        EXPECT(shares_follow_weights(&s, 2));
        for (unsigned i = 0; i < 4; i++)
            EXPECT(s.t[i].ran > 0);              /* nobody starves */
    }
    /* Two equal threads take turns of twice the granularity and up to two
     * ticks (the tick charges in whole ticks): 12 to 16 ms, about the 10 ms
     * slice round-robin gave. */
    struct sim s;
    sim_init(&s, (const int[]){ 0, 0 }, 2);
    int last = -1;
    unsigned run = 0, longest = 0;
    for (unsigned k = 0; k < 10000; k++) {
        sim_tick(&s);
        run = s.current == last ? run + 1 : 1;
        last = s.current;
        if (run > longest)
            longest = run;
    }
    EXPECT(longest * TICK <= 2 * FAIR_GRANULARITY_NS + 2 * TICK);
}

static void test_simulated_sleeper(void)
{
    /* Thread 1 sleeps for 100 s while thread 0 runs alone, then wakes: it
     * is placed just behind, so it gets the CPU first but for no more
     * than the wake bonus and the granularity, then they alternate. */
    struct sim s;
    sim_init(&s, (const int[]){ 0, 0 }, 2);
    sim_tick(&s);
    s.t[1].ready = false;
    s.t[1].asleep = true;
    for (unsigned k = 0; k < 25000; k++)
        sim_tick(&s);
    EXPECT(s.current == 0);
    s.t[1].asleep = false;
    sim_enqueue(&s, 1, true);
    uint64_t ran0 = s.t[0].ran, ran1 = s.t[1].ran;
    unsigned straight = 0;
    for (unsigned k = 0; k < 1000; k++) {
        sim_tick(&s);
        if (s.current == 1 && s.t[0].ran == ran0)
            straight++;
    }
    EXPECT(straight * TICK <= FAIR_WAKE_BONUS_NS + 2 * FAIR_GRANULARITY_NS + 2 * TICK);
    uint64_t d0 = s.t[0].ran - ran0, d1 = s.t[1].ran - ran1;
    EXPECT(d0 * 10 >= d1 * 8 && d1 * 10 >= d0 * 8);   /* then an even split */
}

static const struct host_test tests[] = {
    { "Linux's weights", test_weights },
    { "exact weighted charge", test_charge },
    { "place, rebase, order, preempt, min", test_place_rebase_order },
    { "simulated shares follow weights", test_simulated_shares },
    { "simulated sleeper", test_simulated_sleeper },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
