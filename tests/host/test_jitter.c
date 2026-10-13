/*
 * test_jitter.c - The jitter source's estimator and health test
 * (kernel/core/jitter.c), on the host.
 */

#include "harness.h"

#include <kernel/jitter.h>

#include <stdlib.h>

static uint8_t g_s[JITTER_BATCH];

/* A fixed-seed generator for "unpredictable" test bytes. */
static uint64_t g_x = 0x9E3779B97F4A7C15ull;
static uint8_t next_byte(void)
{
    g_x ^= g_x << 13;
    g_x ^= g_x >> 7;
    g_x ^= g_x << 17;
    return (uint8_t)(g_x >> 32);
}

static unsigned credit_fresh(const uint8_t *s, unsigned n)
{
    struct jitter_rct rct;
    jitter_rct_init(&rct);
    return jitter_batch_credit(&rct, s, n);
}

static bool rct_fresh(const uint8_t *s, unsigned n)
{
    struct jitter_rct rct;
    jitter_rct_init(&rct);
    return jitter_rct_ok(&rct, s, n, JITTER_RCT_CUTOFF);
}

/* One value only: the mode is every sample, so no entropy. */
static void test_constant_is_zero(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = 42;
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) == 0);
    EXPECT(jitter_lag_h_q16(g_s, JITTER_BATCH) == 0);
    EXPECT(!rct_fresh(g_s, JITTER_BATCH));
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == 0);
}

/* Unpredictable bytes: both estimates are several bits (the lag
 * predictor's local figure is log2(n) / (run + 1), about 5 bits for the
 * run of one or two correct guesses chance gives), and the credit is the
 * cap, one bit per sample over the safety factor. */
static void test_unpredictable_is_capped(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = next_byte();
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) > (5u << 16));
    EXPECT(jitter_lag_h_q16(g_s, JITTER_BATCH) > (3u << 16));
    EXPECT(rct_fresh(g_s, JITTER_BATCH));
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == JITTER_BATCH / JITTER_SAFETY);
}

/* Every byte value equally often, in a fixed cycle: the histogram calls
 * it nearly 8 bits; the lag predictor sees the cycle and calls it 0, and
 * the batch earns nothing. */
static void test_cycle_is_refused(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (uint8_t)(i * 37u);
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) > (6u << 16));
    EXPECT(jitter_lag_h_q16(g_s, JITTER_BATCH) < JITTER_MIN_H_Q16);
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == 0);
    /* Two values alternating: the same. */
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (uint8_t)(i & 1u);
    EXPECT(jitter_lag_h_q16(g_s, JITTER_BATCH) < JITTER_MIN_H_Q16);
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == 0);
}

/* Two values in an unpredictable order: about one bit by either
 * estimate's bound, credited under the cap. */
static void test_two_values(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = next_byte() & 1u;
    uint32_t h = jitter_mcv_h_q16(g_s, JITTER_BATCH);
    EXPECT(h > (1u << 16) * 7 / 10 && h <= (1u << 16));
    unsigned c = credit_fresh(g_s, JITTER_BATCH);
    EXPECT(c > 0 && c <= JITTER_BATCH / JITTER_SAFETY);
}

/* One value 99% of the time: below the floor, not credited. */
static void test_mostly_one_value(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (i % 100 == 0) ? 1 : 0;
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) < JITTER_MIN_H_Q16);
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == 0);
}

/* A good batch with one long run fails the repetition test whatever the
 * estimate says. */
static void test_repetition_fails(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = next_byte() | 1u;   /* never 0: the run below is the only one */
    for (unsigned i = 100; i < 100 + JITTER_RCT_CUTOFF; i++)
        g_s[i] = 0;
    EXPECT(!rct_fresh(g_s, JITTER_BATCH));
    EXPECT(credit_fresh(g_s, JITTER_BATCH) == 0);
    g_s[100 + JITTER_RCT_CUTOFF - 1] = 1;   /* one short of the cutoff */
    EXPECT(rct_fresh(g_s, JITTER_BATCH));
}

/* A run split across two batches is one run: each half passes alone, the
 * second fails when the state carries over. */
static void test_repetition_across_batches(void)
{
    static uint8_t a[JITTER_BATCH], b[JITTER_BATCH];
    for (unsigned i = 0; i < JITTER_BATCH; i++) {
        a[i] = next_byte() | 1u;
        b[i] = next_byte() | 1u;
    }
    unsigned half = JITTER_RCT_CUTOFF / 2 + 1;
    for (unsigned i = 0; i < half; i++) {
        a[JITTER_BATCH - 1 - i] = 0;
        b[i] = 0;
    }
    EXPECT(rct_fresh(a, JITTER_BATCH) && rct_fresh(b, JITTER_BATCH));
    struct jitter_rct rct;
    jitter_rct_init(&rct);
    EXPECT(jitter_rct_ok(&rct, a, JITTER_BATCH, JITTER_RCT_CUTOFF));
    EXPECT(!jitter_rct_ok(&rct, b, JITTER_BATCH, JITTER_RCT_CUTOFF));
}

static void test_too_few(void)
{
    g_s[0] = 1;
    EXPECT(jitter_mcv_h_q16(g_s, 1) == 0);
    EXPECT(jitter_lag_h_q16(g_s, 2) == 0);
    EXPECT(credit_fresh(g_s, 1) == 0);
}

static const struct host_test tests[] = {
    { "a constant has no entropy", test_constant_is_zero },
    { "unpredictable bytes are capped at one bit", test_unpredictable_is_capped },
    { "a cycle is refused", test_cycle_is_refused },
    { "two values in an unpredictable order", test_two_values },
    { "one value 99% of the time", test_mostly_one_value },
    { "a long run fails the repetition test", test_repetition_fails },
    { "a run across batches fails it", test_repetition_across_batches },
    { "fewer than two samples", test_too_few },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
