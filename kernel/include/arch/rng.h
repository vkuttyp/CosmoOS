/*
 * arch/rng.h - The CPU's random instructions, as entropy sources for
 * kernel/core/random.c (docs/kernel/security/design.md §6).
 *
 * Each read is one attempt: it returns true and stores the value only
 * when the instruction itself reported success (x86-64 CF=1; AArch64
 * NZCV=0000). The caller retries and credits; a false return is never
 * credited or mixed. Any context: no locks, no sleeping.
 */

#ifndef ARCH_RNG_H
#define ARCH_RNG_H

#include <kernel/types.h>

/* Probe the CPU once, on the boot CPU, before the first read. */
void arch_rng_init(void);

/* The reseeding source (RDSEED, RNDRRS) and the DRBG (RDRAND, RNDR). */
bool arch_rng_has_seed(void);
bool arch_rng_has_random(void);
bool arch_rng_seed64(uint64_t *v);
bool arch_rng_random64(uint64_t *v);

/* The instruction names, for the boot line ("rdseed", "rndr"...). */
const char *arch_rng_seed_name(void);
const char *arch_rng_random_name(void);

#endif /* ARCH_RNG_H */
