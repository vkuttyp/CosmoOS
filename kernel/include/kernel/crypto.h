/*
 * crypto.h - Kernel cryptographic primitives (kernel/security/).
 *
 * SHA-512 (FIPS 180-4) and Ed25519 signature verification (RFC 8032).
 * Both are pure computations: no allocation, no locks, no sleeping, safe
 * in any context that can afford the CPU time. Ed25519 here is
 * verification only and variable-time; every input it sees is public.
 * Host tests in tests/host/test_crypto.c run the RFC vectors.
 */

#ifndef KERNEL_CRYPTO_H
#define KERNEL_CRYPTO_H

#include <kernel/types.h>

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

struct sha512_ctx {
    uint64_t state[8];
    uint64_t count;               /* bytes consumed */
    uint8_t  buf[SHA512_BLOCK_SIZE];
    size_t   buflen;
};

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len);
void sha512_final(struct sha512_ctx *ctx, uint8_t out[SHA512_DIGEST_SIZE]);
void sha512(const void *data, size_t len, uint8_t out[SHA512_DIGEST_SIZE]);

#define ED25519_PUBLIC_KEY_SIZE 32
#define ED25519_SIGNATURE_SIZE  64

/* true if `sig` is a valid Ed25519 signature of msg[0..len) under `pub`.
 * Rejects non-canonical S (>= L) and undecodable points. */
bool ed25519_verify(const uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                    const uint8_t pub[ED25519_PUBLIC_KEY_SIZE]);

#endif /* KERNEL_CRYPTO_H */
