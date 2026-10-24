/*
 * blake2b.h - BLAKE2b (RFC 7693), plain and keyed
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 14").
 *
 * Keyed, it is the MAC an encrypted pool's metadata carries; plain, it is
 * the hash Argon2id is built on, in user space (userland/lib/argon2.c
 * compiles this file). Pure: no allocation, no locks, no globals.
 * Compiled on the host by tests/host/test_crypto.c, which checks it
 * against Python's hashlib.
 */

#ifndef KERNEL_BLAKE2B_H
#define KERNEL_BLAKE2B_H

#include <stddef.h>
#include <stdint.h>

#define BLAKE2B_OUT_MAX 64u
#define BLAKE2B_KEY_MAX 64u

struct blake2b {
    uint64_t h[8];
    uint64_t t[2];
    uint8_t buf[128];
    size_t buflen;
    size_t outlen;
};

/* An `outlen`-byte digest (1..64), keyed by `key` (0..64 bytes; NULL or
 * 0 for none). The output length is part of the parameter block, so a
 * truncated digest is a different function, not a prefix of a longer one. */
void blake2b_init_key(struct blake2b *s, size_t outlen, const void *key, size_t keylen);
void blake2b_init(struct blake2b *s, size_t outlen);
void blake2b_update(struct blake2b *s, const void *in, size_t n);
/* Writes outlen bytes and clears the state. */
void blake2b_final(struct blake2b *s, uint8_t *out);
void blake2b(uint8_t *out, size_t outlen, const void *in, size_t inlen);

#endif /* KERNEL_BLAKE2B_H */
