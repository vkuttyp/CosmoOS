/*
 * keyring.h - Trusted public keys compiled into the kernel.
 *
 * scripts/gen-keyring.py turns the .pub files in tools/keys into the table behind
 * keyring_find(). The ring is immutable after build; a firmware-provided
 * ring is future work and would add keys, not change this interface.
 */

#ifndef KERNEL_KEYRING_H
#define KERNEL_KEYRING_H

#include <kernel/crypto.h>
#include <kernel/types.h>

#define KEYRING_ID_SIZE 8

struct trusted_key {
    uint8_t id[KEYRING_ID_SIZE];             /* first 8 bytes of SHA-512(pub) */
    uint8_t pub[ED25519_PUBLIC_KEY_SIZE];
    const char *name;
};

/* Lock-free, no allocation, any context. NULL if unknown. */
const struct trusted_key *keyring_find(const uint8_t id[KEYRING_ID_SIZE]);
unsigned keyring_count(void);
const struct trusted_key *keyring_entry(unsigned index);

/* The id derivation, shared with the signing tool. */
void keyring_key_id(const uint8_t pub[ED25519_PUBLIC_KEY_SIZE], uint8_t id[KEYRING_ID_SIZE]);

/* Generated table (kernel/security/keyring_builtin.c, built from
 * the .pub files in tools/keys). */
extern const struct trusted_key keyring_builtin[];
extern const unsigned keyring_builtin_count;

#endif /* KERNEL_KEYRING_H */
