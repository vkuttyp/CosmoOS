/*
 * test_jitter.c - The jitter source's estimator and health test
 * (kernel/core/jitter.c), on the host.
 */

#include "harness.h"

#include <kernel/jitter.h>

#include <stdlib.h>

static uint8_t g_s[JITTER_BATCH];

/* One value only: the mode is every sample, so no entropy. */
static void test_constant_is_zero(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = 42;
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) == 0);
    EXPECT(!jitter_rct_ok(g_s, JITTER_BATCH, JITTER_RCT_CUTOFF));
    EXPECT(jitter_batch_credit(g_s, JITTER_BATCH) == 0);
}

/* Every byte value equally often, never repeating: the estimate is the
 * 99% bound of p = 1/256, a little under 8 bits, and the credit is the cap,
 * one bit per sample over the safety factor. */
static void test_uniform_is_capped(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (uint8_t)(i * 37u);
    uint32_t h = jitter_mcv_h_q16(g_s, JITTER_BATCH);
    EXPECT(h > (6u << 16) && h < (8u << 16));
    EXPECT(jitter_rct_ok(g_s, JITTER_BATCH, JITTER_RCT_CUTOFF));
    EXPECT(jitter_batch_credit(g_s, JITTER_BATCH) == JITTER_BATCH / JITTER_SAFETY);
}

/* Two values alternating: p = 1/2 plus the bound, under one bit. */
static void test_two_values(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (uint8_t)(i & 1u);
    uint32_t h = jitter_mcv_h_q16(g_s, JITTER_BATCH);
    EXPECT(h > (1u << 16) * 8 / 10 && h < (1u << 16));
    unsigned c = jitter_batch_credit(g_s, JITTER_BATCH);
    EXPECT(c > 0 && c < JITTER_BATCH / JITTER_SAFETY);
}

/* One value 99% of the time: below the floor, not credited. */
static void test_mostly_one_value(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (i % 100 == 0) ? 1 : 0;
    EXPECT(jitter_mcv_h_q16(g_s, JITTER_BATCH) < JITTER_MIN_H_Q16);
    EXPECT(jitter_batch_credit(g_s, JITTER_BATCH) == 0);
}

/* A good batch with one long run fails the repetition test whatever the
 * estimate says. */
static void test_repetition_fails(void)
{
    for (unsigned i = 0; i < JITTER_BATCH; i++)
        g_s[i] = (uint8_t)(i * 37u);
    for (unsigned i = 100; i < 100 + JITTER_RCT_CUTOFF; i++)
        g_s[i] = 7;
    EXPECT(!jitter_rct_ok(g_s, JITTER_BATCH, JITTER_RCT_CUTOFF));
    EXPECT(jitter_batch_credit(g_s, JITTER_BATCH) == 0);
    g_s[100 + JITTER_RCT_CUTOFF - 1] = 8;   /* one short of the cutoff */
    EXPECT(jitter_rct_ok(g_s, JITTER_BATCH, JITTER_RCT_CUTOFF));
}

static void test_too_few(void)
{
    g_s[0] = 1;
    EXPECT(jitter_mcv_h_q16(g_s, 1) == 0);
    EXPECT(jitter_batch_credit(g_s, 1) == 0);
}

static const struct host_test tests[] = {
    { "a constant has no entropy", test_constant_is_zero },
    { "uniform bytes are capped at one bit", test_uniform_is_capped },
    { "two alternating values", test_two_values },
    { "one value 99% of the time", test_mostly_one_value },
    { "a long run fails the repetition test", test_repetition_fails },
    { "fewer than two samples", test_too_few },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
