/*
 * jitter.c - CPU timing jitter as an entropy source of last resort
 * (docs/kernel/security/design.md §6, "Timing jitter").
 *
 * A machine with no CPU random instruction, no firmware seed and no
 * entropy device -- Parallels Desktop on Apple Silicon -- otherwise never
 * seeds. The source times a short memory-walking workload on the
 * monotonic counter; caches, TLBs, interrupts and the host's scheduling
 * make the time vary. How much it varies is measured, not assumed: each
 * batch is estimated (the lower of SP 800-90B's most-common-value and
 * lag-prediction estimates over the low byte of each time) and
 * health-tested (the repetition count test, its run carried across
 * batches), and credited at most an eighth of a bit per sample.
 */

#include <kernel/jitter.h>

/* --- the estimator: pure, host-tested ------------------------------------ */

static uint64_t isqrt64(uint64_t x)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x)
        bit >>= 2;
    while (bit != 0) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* log2(x) in Q16 for 1 <= x < 2^63: the integer part from the top bit,
 * the fraction by repeated squaring of the normalised mantissa. */
static uint32_t log2_q16(uint64_t x)
{
    unsigned ip = 63u - (unsigned)__builtin_clzll(x);
    /* Mantissa in Q31: x / 2^ip, in [1, 2). */
    uint64_t m = ip >= 31 ? x >> (ip - 31) : x << (31 - ip);
    uint32_t frac = 0;
    for (unsigned i = 0; i < 16; i++) {
        m = (m * m) >> 31;   /* square: [1, 4) in Q31 */
        frac <<= 1;
        if (m >= (2ull << 31)) {
            m >>= 1;
            frac |= 1;
        }
    }
    return (ip << 16) | frac;
}

/* -log2 of the 99% upper bound on a probability estimated as hits / n
 * (SP 800-90B: p + 2.576 sqrt(p (1 - p) / (n - 1))), in bits as Q16;
 * floor is the smallest probability the samples allow (1/256 for bytes). */
static uint32_t h_from_rate(unsigned hits, unsigned n, uint64_t floor_q32)
{
    uint64_t one = 1ull << 32;
    uint64_t p = ((uint64_t)hits << 32) / n;
    uint64_t var = ((p * (one - p)) >> 32) / (n - 1);   /* Q32 */
    /* sqrt of a Q32 value, in Q32: isqrt(var << 32). var <= 2^30, so
     * the shift fits. */
    uint64_t sd = isqrt64(var << 32);
    uint64_t pu = p + (sd * 2576) / 1000;
    if (pu < floor_q32)
        pu = floor_q32;
    if (pu >= one)
        return 0;
    /* H = -log2(pu / 2^32) = 32 - log2(pu). */
    uint32_t l = log2_q16(pu);
    return l >= (32u << 16) ? 0 : (32u << 16) - l;
}

uint32_t jitter_mcv_h_q16(const uint8_t *samples, unsigned n)
{
    if (n < 2)
        return 0;
    unsigned count[256] = { 0 };
    unsigned mode = 0;
    for (unsigned i = 0; i < n; i++)
        if (++count[samples[i]] > mode)
            mode = count[samples[i]];
    return h_from_rate(mode, n, 0);
}

uint32_t jitter_lag_h_q16(const uint8_t *samples, unsigned n)
{
    if (n < 3)
        return 0;
    /* Each sample is predicted to equal the one `winner` lags back, the
     * lag that has predicted best so far; every lag's score is kept. */
    unsigned score[JITTER_LAG_MAX] = { 0 };
    unsigned winner = 0, predictions = 0, correct = 0, run = 0, longest = 0;
    for (unsigned i = 1; i < n; i++) {
        unsigned lag = winner + 1;
        if (i >= lag) {
            predictions++;
            if (samples[i] == samples[i - lag]) {
                correct++;
                if (++run > longest)
                    longest = run;
            } else {
                run = 0;
            }
        }
        unsigned dmax = i < JITTER_LAG_MAX ? i : JITTER_LAG_MAX;
        for (unsigned d = 1; d <= dmax; d++) {
            if (samples[i] == samples[i - d]) {
                score[d - 1]++;
                if (score[d - 1] >= score[winner])
                    winner = d - 1;
            }
        }
    }
    if (predictions < 2)
        return 0;
    /* Global: the rate of correct predictions over the batch. */
    uint32_t global = h_from_rate(correct, predictions, (1ull << 32) / 256);
    /* Local: the longest run of correct predictions. A run of r among n
     * predictions is about what a success probability p with
     * p^(r + 1) = 1/n would give, so H = log2(n) / (r + 1). It catches a
     * pattern the predictor locked onto late in the batch, which the
     * global rate dilutes (SP 800-90B §6.3.8's local estimate, solved
     * here in this closed form rather than its series). */
    uint32_t local = log2_q16(predictions) / (longest + 1);
    return local < global ? local : global;
}

void jitter_rct_init(struct jitter_rct *rct)
{
    rct->run = 0;
    rct->last = 0;
}

bool jitter_rct_ok(struct jitter_rct *rct, const uint8_t *samples, unsigned n, unsigned cutoff)
{
    bool ok = true;
    for (unsigned i = 0; i < n; i++) {
        rct->run = rct->run != 0 && samples[i] == rct->last ? rct->run + 1 : 1;
        rct->last = samples[i];
        if (rct->run >= cutoff)
            ok = false;
    }
    return ok;
}

unsigned jitter_batch_credit(struct jitter_rct *rct, const uint8_t *samples, unsigned n)
{
    if (!jitter_rct_ok(rct, samples, n, JITTER_RCT_CUTOFF))
        return 0;
    /* The lower of two estimates: the most common value's (the spread)
     * and the lag predictor's (catches a cycle or a repeating pattern,
     * which the histogram cannot see). */
    uint32_t h = jitter_mcv_h_q16(samples, n);
    uint32_t hl = jitter_lag_h_q16(samples, n);
    if (hl < h)
        h = hl;
    if (h < JITTER_MIN_H_Q16)
        return 0;
    if (h > (1u << 16))
        h = 1u << 16;   /* at most one bit per sample, whatever the estimate */
    return (unsigned)(((uint64_t)n * h) >> 16) / JITTER_SAFETY;
}

/* --- the collector: kernel only ------------------------------------------ */

#ifndef JITTER_HOST_TEST
#include <arch/timer.h>

#define WALK_BYTES 65536u

static uint8_t g_walk[WALK_BYTES];

void jitter_sample(uint64_t deltas[JITTER_BATCH], uint8_t low[JITTER_BATCH])
{
    uint64_t prev = 0;
    unsigned idx = 0;
    for (unsigned i = 0; i < JITTER_BATCH; i++) {
        uint64_t t0 = arch_clock_read();
        /* A walk whose stride depends on the previous time, so cache and
         * TLB state differ run to run; the stores keep it from being
         * folded away. 256 steps over 64 KiB: with 64 over 16 KiB, QEMU's
         * TCG made the times regular enough that the lag predictor refused
         * most batches on x86-64; with 256 a boot seeds in 8-17 batches
         * there. */
        for (unsigned k = 0; k < 256; k++) {
            idx = (idx + 64u * (1u + ((unsigned)prev & 7u)) + k) & (WALK_BYTES - 1);
            g_walk[idx] = (uint8_t)(g_walk[idx] + (uint8_t)k + (uint8_t)prev);
        }
        uint64_t d = arch_clock_read() - t0;
        deltas[i] = d;
        low[i] = (uint8_t)d;
        prev = d;
    }
}
#endif
