/*
 * random.h - Kernel entropy pool.
 *
 * A SHA-512 based pool: hardware sources (virtio-rng, later RDRAND,
 * interrupt timing) add entropy; consumers draw bytes. Outputs are
 * SHA-512(state || counter) with a reseed after each request so an
 * exposed state does not reveal earlier output. It is deterministic
 * given its inputs and is NOT a certified DRBG; it is what the kernel
 * has until the security phase revisits it. Any context (spinlock).
 */

#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <kernel/types.h>

void random_init(void);

/* Mix `len` bytes in and credit `bits` of entropy (capped). */
void random_add_entropy(const void *buf, size_t len, unsigned bits);

/* Fill buf. Never fails; quality reflects random_entropy_bits(). */
void random_get_bytes(void *buf, size_t len);

uint64_t random_u64(void);

/* Entropy credited so far, capped at 512 bits. */
unsigned random_entropy_bits(void);

/* Bytes mixed in from hardware sources, for diagnostics. */
uint64_t random_source_bytes(void);

#endif /* KERNEL_RANDOM_H */
