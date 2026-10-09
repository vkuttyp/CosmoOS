/*
 * random.h - Kernel entropy pool (docs/kernel/security/design.md §6).
 *
 * Inputs (CPU random instructions, virtio-rng) are absorbed into an input
 * pool; output is SHA-512(key || counter) from a key that changes only by
 * a reseed of at least 256 credited bits and by a ratchet after every
 * request (forward secrecy). The pool is "seeded" after the first such
 * reseed, and stays seeded.
 *
 * random_get_bytes never blocks and never fails, seeded or not. A caller
 * whose value must not be predictable (a key, getrandom's default) checks
 * random_ready() or waits with random_wait_ready() first; design.md §6
 * classifies every caller. Any context, except the waits (thread context).
 */

#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <kernel/types.h>

/* How long kernel key generation waits for seeding before refusing. */
#define RANDOM_KEYGEN_WAIT_NS (5ull * 1000000000ull)
#define RANDOM_WAIT_FOREVER   UINT64_MAX

void random_init(void);

/* Mix `len` bytes in and credit `bits` of entropy (capped). Credit only
 * what a source delivered and reported valid (invariant S17). */
void random_add_entropy(const void *buf, size_t len, unsigned bits);

/* Fill buf. Never blocks or fails; unseeded output is predictable in
 * principle, so must-be-seeded callers ask random_ready() first. */
void random_get_bytes(void *buf, size_t len);

uint64_t random_u64(void);

/* Entropy credited so far, capped at 512 bits. */
unsigned random_entropy_bits(void);

/* Bytes mixed in by random_add_entropy (devices), for diagnostics. */
uint64_t random_source_bytes(void);

/* True once the pool is seeded; never false again. */
bool random_ready(void);

/* Wait until seeded: 0, -ETIMEDOUT after timeout_ns (RANDOM_WAIT_FOREVER:
 * no bound), or -EINTR when the calling process has a deliverable signal
 * or is being killed. Thread context. */
int random_wait_ready(uint64_t timeout_ns);

/* The boot's wait before the self-tests and init: up to 5 s, and one
 * WARN when it expires (invariant S19). */
void random_boot_wait(void);

#endif /* KERNEL_RANDOM_H */
