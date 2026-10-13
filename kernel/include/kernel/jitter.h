/*
 * jitter.h - CPU timing jitter as an entropy source of last resort
 * (docs/kernel/security/design.md §6, "Timing jitter").
 *
 * The estimator and the health test are pure functions of the samples, so
 * the host tests them (tests/host/test_jitter.c, built with
 * JITTER_HOST_TEST); the collector runs only in the kernel.
 */

#ifndef KERNEL_JITTER_H
#define KERNEL_JITTER_H

#include <stdbool.h>
#include <stdint.h>

#define JITTER_BATCH        1024u   /* samples per batch */
#define JITTER_RCT_CUTOFF   32u     /* the same value this many times in a row fails the batch */
#define JITTER_MIN_H_Q16    4096u   /* below 1/16 bit per sample the batch is not credited */
#define JITTER_SAFETY       8u      /* credit = samples * min(H, 1) / JITTER_SAFETY */

#define JITTER_LAG_MAX      256u    /* the lag predictor's longest lag: a cycle through every byte value */

/* The SP 800-90B most-common-value estimate of min-entropy per sample
 * (§6.3.1, the 99% upper bound on the mode's probability), in bits as
 * Q16 fixed point: 0 for n < 2, at most 8 << 16 for byte samples. It sees
 * only the spread of the values, not their order. */
uint32_t jitter_mcv_h_q16(const uint8_t *samples, unsigned n);

/* The SP 800-90B lag-prediction estimate (§6.3.8, the global predictor,
 * lags 1 to JITTER_LAG_MAX): each sample predicted to repeat the one at
 * the lag that has predicted best so far. The lower of two min-entropy
 * figures: the global one, from the 99% upper bound on the rate of
 * correct predictions (at least 1/256), and a local one from the longest
 * run of them, log2(predictions) / (run + 1). A cycle or a repeating
 * pattern scores near 0. Q16; 0 for n < 3. */
uint32_t jitter_lag_h_q16(const uint8_t *samples, unsigned n);

/* The repetition count test, its run carried across batches: false when
 * one value occurs `cutoff` or more times in a row, counting the end of
 * the previous batch. */
struct jitter_rct {
    unsigned run;   /* 0: nothing seen yet */
    uint8_t last;
};
void jitter_rct_init(struct jitter_rct *rct);
bool jitter_rct_ok(struct jitter_rct *rct, const uint8_t *samples, unsigned n, unsigned cutoff);

/* The credit a batch earns, in bits: 0 when it fails the repetition test
 * or the lower of the two estimates is below JITTER_MIN_H_Q16; else
 * n * min(H, 1) / JITTER_SAFETY. */
unsigned jitter_batch_credit(struct jitter_rct *rct, const uint8_t *samples, unsigned n);

/* Kernel: time JITTER_BATCH runs of a memory-walking workload on the
 * monotonic counter. deltas[] gets each run's raw count difference and
 * low[] its low byte, which is what is estimated. */
void jitter_sample(uint64_t deltas[JITTER_BATCH], uint8_t low[JITTER_BATCH]);

#endif /* KERNEL_JITTER_H */
