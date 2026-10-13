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

/* The SP 800-90B most-common-value estimate of min-entropy per sample
 * (§6.3.1, the 99% upper bound on the mode's probability), in bits as
 * Q16 fixed point: 0 for n < 2, at most 8 << 16 for byte samples. */
uint32_t jitter_mcv_h_q16(const uint8_t *samples, unsigned n);

/* The repetition count test: false when one value occurs `cutoff` or more
 * times in a row. */
bool jitter_rct_ok(const uint8_t *samples, unsigned n, unsigned cutoff);

/* The credit a batch earns, in bits: 0 when it fails the repetition test
 * or its estimate is below JITTER_MIN_H_Q16; else n * min(H, 1) /
 * JITTER_SAFETY. */
unsigned jitter_batch_credit(const uint8_t *samples, unsigned n);

/* Kernel: time JITTER_BATCH runs of a memory-walking workload on the
 * monotonic counter. deltas[] gets each run's raw count difference and
 * low[] its low byte, which is what is estimated. */
void jitter_sample(uint64_t deltas[JITTER_BATCH], uint8_t low[JITTER_BATCH]);

#endif /* KERNEL_JITTER_H */
