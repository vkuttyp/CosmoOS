/*
 * test_cputime.c - CPU-time accounting (kernel/process/cputime.c, generated
 * from Nova) on the host: the tick's sample, ITIMER_VIRTUAL and ITIMER_PROF,
 * the user/system split, and the exact scaling under it.
 */

#include "harness.h"

#include <kernel/cputime.h>

#include <string.h>

#define TICK 4000000ull   /* TICK_NS at 250 Hz */

static void test_tick_sample(void)
{
    struct cputime ct;
    memset(&ct, 0, sizeof(ct));
    EXPECT(cputime_tick(&ct, true, TICK) == 0);
    EXPECT(cputime_tick(&ct, false, TICK) == 0);
    EXPECT(cputime_tick(&ct, true, TICK) == 0);
    EXPECT(ct.sample.user_ns == 2 * TICK && ct.sample.system_ns == TICK);
    /* Saturates rather than wrapping (or panicking, in interrupt context). */
    ct.sample.user_ns = UINT64_MAX - 1;
    EXPECT(cputime_tick(&ct, true, TICK) == 0 && ct.sample.user_ns == UINT64_MAX);
}

static void test_itimer_set_get(void)
{
    struct cputime ct;
    memset(&ct, 0, sizeof(ct));
    uint64_t v = 1, i = 1;
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_VIRTUAL, TICK, &v, &i) && v == 0 && i == 0);
    EXPECT(!cputime_itimer_get(&ct, 0, TICK, &v, &i) && !cputime_itimer_get(&ct, 3, TICK, &v, &i));
    EXPECT(!cputime_itimer_set(&ct, 0, 1, 0, TICK, NULL, NULL));
    /* A tick is added to the value, as Linux adds one: the first sample
     * may charge a whole tick for a fraction of one. */
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_PROF, 10 * TICK, 3 * TICK, TICK, NULL, NULL));
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_PROF, TICK, &v, &i) && v == 11 * TICK && i == 3 * TICK);
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_VIRTUAL, TICK, &v, &i) && v == 0 && i == 0);   /* the other one */
    /* System time advances PROF only. */
    cputime_tick(&ct, false, TICK);
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_PROF, TICK, &v, &i) && v == 10 * TICK);
    /* Clearing reports the old setting and clears the interval too. */
    uint64_t ov = 0, oi = 0;
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_PROF, 0, 5 * TICK, TICK, &ov, &oi) && ov == 10 * TICK && oi == 3 * TICK);
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_PROF, TICK, &v, &i) && v == 0 && i == 0);
    /* "Never" saturates instead of wrapping into the past. */
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_VIRTUAL, UINT64_MAX, 0, TICK, NULL, NULL));
    EXPECT(ct.virt.expires_ns == UINT64_MAX);
    EXPECT(cputime_tick(&ct, true, TICK) == 0);
}

static void test_itimer_fire(void)
{
    struct cputime ct;
    memset(&ct, 0, sizeof(ct));
    /* VIRTUAL, one shot, 2 ticks (+1): fires on the third user tick and
     * disarms; system ticks do not count. */
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_VIRTUAL, 2 * TICK, 0, TICK, NULL, NULL));
    EXPECT(cputime_tick(&ct, true, TICK) == 0);
    EXPECT(cputime_tick(&ct, false, TICK) == 0);
    EXPECT(cputime_tick(&ct, true, TICK) == 0);
    EXPECT(cputime_tick(&ct, true, TICK) == CPUTIME_DUE_VIRT);
    uint64_t v, i;
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_VIRTUAL, TICK, &v, &i) && v == 0 && i == 0);
    EXPECT(cputime_tick(&ct, true, TICK) == 0);

    /* PROF, periodic: reloads from its expiry, so the period holds. */
    memset(&ct, 0, sizeof(ct));
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_PROF, TICK, 2 * TICK, TICK, NULL, NULL));
    unsigned fired = 0;
    for (int k = 0; k < 20; k++)
        fired += (cputime_tick(&ct, k & 1, TICK) & CPUTIME_DUE_PROF) != 0;
    EXPECT(fired == 10);   /* at ticks 2, 4, ..., 20 */
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_PROF, TICK, &v, &i) && v == 2 * TICK && i == 2 * TICK);

    /* Both at once. */
    memset(&ct, 0, sizeof(ct));
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_VIRTUAL, 1, 0, TICK, NULL, NULL));
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_PROF, 1, 0, TICK, NULL, NULL));
    EXPECT(cputime_tick(&ct, true, TICK) == 0);   /* 1 ns + a tick: not yet */
    EXPECT(cputime_tick(&ct, true, TICK) == (CPUTIME_DUE_VIRT | CPUTIME_DUE_PROF));

    /* Due but not yet fired reads one tick, never 0. */
    memset(&ct, 0, sizeof(ct));
    EXPECT(cputime_itimer_set(&ct, CPUTIME_ITIMER_PROF, TICK, 0, TICK, NULL, NULL));
    ct.sample.user_ns = 5 * TICK;   /* as if sampled without the check */
    EXPECT(cputime_itimer_get(&ct, CPUTIME_ITIMER_PROF, TICK, &v, &i) && v == TICK);
}

static uint64_t scale_ref(uint64_t a, uint64_t b, uint64_t c)
{
    return (uint64_t)((unsigned __int128)a * b / c);
}

static void test_scale(void)
{
    static const uint64_t vals[] = { 0, 1, 2, 3, 7, 1000, 4000000, 0xFFFFFFFFull, 0x100000000ull, 0x123456789ABCDEFull,
                                     0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, UINT64_MAX - 1, UINT64_MAX };
    size_t n = sizeof(vals) / sizeof(vals[0]);
    for (size_t x = 0; x < n; x++)
        for (size_t y = 0; y < n; y++)
            for (size_t z = 0; z < n; z++) {
                uint64_t a = vals[x], b = vals[y], c = vals[z];
                if (c == 0 || a > c)
                    continue;
                EXPECT(cputime_scale(a, b, c) == scale_ref(a, b, c));
            }
    /* A pseudo-random sweep. */
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 200000; k++) {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        uint64_t c = s | 1;
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        uint64_t a = s % c;
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        uint64_t b = s >> (k % 64);
        EXPECT(cputime_scale(a, b, c) == scale_ref(a, b, c));
    }
}

static void test_adjust(void)
{
    struct cputime_pair sample = { 3 * TICK, 1 * TICK }, rep = { 0, 0 };
    /* 3:1 of 100 ms. */
    cputime_adjust(100000000ull, &sample, &rep);
    EXPECT(rep.user_ns == 75000000ull && rep.system_ns == 25000000ull);
    /* Less than already reported: the report repeats. */
    cputime_adjust(90000000ull, &sample, &rep);
    EXPECT(rep.user_ns == 75000000ull && rep.system_ns == 25000000ull);
    /* Neither part goes down when the proportion moves. */
    sample = (struct cputime_pair){ 3 * TICK, 7 * TICK };
    cputime_adjust(110000000ull, &sample, &rep);
    EXPECT(rep.user_ns == 75000000ull && rep.system_ns == 35000000ull);
    sample = (struct cputime_pair){ 9 * TICK, 1 * TICK };
    cputime_adjust(120000000ull, &sample, &rep);
    EXPECT(rep.user_ns == 85000000ull && rep.system_ns == 35000000ull);
    EXPECT(rep.user_ns + rep.system_ns == 120000000ull);
    /* Never sampled: user time. All system: system time. */
    struct cputime_pair none = { 0, 0 }, r2 = { 0, 0 };
    cputime_adjust(5000ull, &none, &r2);
    EXPECT(r2.user_ns == 5000ull && r2.system_ns == 0);
    struct cputime_pair sys = { 0, TICK }, r3 = { 0, 0 };
    cputime_adjust(5000ull, &sys, &r3);
    EXPECT(r3.user_ns == 0 && r3.system_ns == 5000ull);
    /* Huge values scale exactly. */
    struct cputime_pair big = { UINT64_MAX / 3, UINT64_MAX / 3 }, r4 = { 0, 0 };
    cputime_adjust(UINT64_MAX - 1, &big, &r4);
    EXPECT(r4.user_ns + r4.system_ns == UINT64_MAX - 1 && r4.system_ns == (UINT64_MAX - 1) / 2);
}

static const struct host_test tests[] = {
    { "tick sample", test_tick_sample },
    { "itimer set and get", test_itimer_set_get },
    { "itimer expiry", test_itimer_fire },
    { "exact scaling", test_scale },
    { "user/system split", test_adjust },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
