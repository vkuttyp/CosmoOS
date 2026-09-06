/*
 * chacha20.h - ChaCha20 and Poly1305 (RFC 8439)
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 7").
 *
 * ChaCha20 rather than AES because there is no AES instruction to lean
 * on here -- a software AES is either slow or a table lookup whose
 * timing depends on the key, and this code runs on whatever the guest
 * gives it. ChaCha20's inner loop is add-rotate-xor on registers: the
 * same work for every input, no tables, nothing to leak through a cache.
 *
 * Pure: no allocation, no locks, no globals. Compiled on the host by
 * tests/host/test_chacha20.c, which checks it against the vectors in the
 * RFC, and fuzzed by tests/fuzz/fuzz_chacha20.c.
 */

#ifndef KERNEL_CHACHA20_H
#define KERNEL_CHACHA20_H

#include <kernel/types.h>

#define CHACHA20_KEY_SIZE   32u
#define CHACHA20_NONCE_SIZE 12u
#define POLY1305_TAG_SIZE   16u

/* XOR `len` bytes of `in` with the keystream for (key, counter, nonce)
 * into `out`; in and out may be the same pointer. */
void chacha20_xor(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter, const uint8_t nonce[CHACHA20_NONCE_SIZE],
                  const void *in, void *out, size_t len);

/* One-time authenticator over `len` bytes with a 32-byte key that must
 * never be reused for another message (RFC 8439 §2.5). */
void poly1305(const uint8_t key[32], const void *data, size_t len, uint8_t tag[POLY1305_TAG_SIZE]);

/* Constant time in the length of the tag: a comparison that stops early
 * tells an attacker how much of a forgery was right. */
bool poly1305_verify(const uint8_t a[POLY1305_TAG_SIZE], const uint8_t b[POLY1305_TAG_SIZE]);

/*
 * AEAD in the shape this filesystem needs: encrypt `len` bytes in place
 * and produce a tag over the *ciphertext* and the associated data, so a
 * holder of the disk and no key can still check a block (design.md,
 * "Integrity"). The Poly1305 key is the first block of the same
 * keystream, which is why the data starts at counter 1.
 *
 * `aad` is authenticated but not encrypted, and it is what says *where*
 * a block belongs: without it a valid block and its tag could be moved
 * to another offset of the same file and would open there, silently
 * displacing content. It is not stored -- the reader supplies what it
 * believes the position to be, and a lie makes the tag fail.
 */
void chacha20_seal(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE], const void *aad,
                   size_t aad_len, void *data, size_t len, uint8_t tag[POLY1305_TAG_SIZE]);
/* The reverse: check the tag first and decrypt only if it matches.
 * False leaves `data` untouched. */
bool chacha20_open(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE], const void *aad,
                   size_t aad_len, void *data, size_t len, const uint8_t tag[POLY1305_TAG_SIZE]);

#endif /* KERNEL_CHACHA20_H */
