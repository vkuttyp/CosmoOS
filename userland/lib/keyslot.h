/*
 * keyslot.h - An encrypted pool's key slots, sealed and opened in user
 * space (roadmap 1.2 B5; docs/kernel-services/filesystem/cosmofs/design.md,
 * "The key hierarchy").
 *
 * A slot seals the pool's 32-byte master key with ChaCha20-Poly1305 under
 * a slot key: Argon2id of a passphrase, or BLAKE2b keyed by a key file,
 * over the slot's own salt. The associated data is the pool's uuid and the
 * slot's index, kind, parameters and salt (cfs_keyslot_aad), so the tag
 * refuses a wrong passphrase before anything else is read. Pure: no I/O,
 * no randomness (the caller draws the salt), so the host can test it.
 */
#ifndef USERLAND_KEYSLOT_H
#define USERLAND_KEYSLOT_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/cosmo/keyslot.h>

/* RFC 9106's second recommended setting: 64 MiB, three passes, one lane
 * (design.md, "The key hierarchy", for the measurements). */
#define KEYSLOT_T_DEFAULT 3u
#define KEYSLOT_M_DEFAULT 65536u
#define KEYSLOT_LANES_DEFAULT 1u

#define KEYFILE_SIZE 32u
/* Where slot 0's key file is: the root of the ESP (cosmo-install --key-file). */
#define KEYFILE_NAME "cosmo.key"

/* Seal `master` into `out` as slot `index` of the pool `uuid`, with `salt`. */
int keyslot_seal_passphrase(const uint8_t uuid[16], unsigned index, uint32_t t_cost, uint32_t m_cost_kib,
                            uint32_t lanes, const void *pw, size_t pwlen, const uint8_t salt[16],
                            const uint8_t master[COSMO_KEY_SIZE], struct cosmo_keyslot *out);
int keyslot_seal_keyfile(const uint8_t uuid[16], const uint8_t file[KEYFILE_SIZE], const uint8_t salt[16],
                         const uint8_t master[COSMO_KEY_SIZE], struct cosmo_keyslot *out);

/* Open slot `index` with `secret` (a passphrase, or a key file's 32 bytes
 * for a key-file slot): 0 and the master key, -EKEYREJECTED when the tag
 * does not verify, -EINVAL for an empty slot or bad parameters, -ENOMEM
 * when Argon2id's memory cannot be had (the COSMO_E* values, which are
 * this libc's). */
int keyslot_open(const uint8_t uuid[16], unsigned index, const struct cosmo_keyslot *s, const void *secret,
                 size_t len, uint8_t master[COSMO_KEY_SIZE]);

#endif
