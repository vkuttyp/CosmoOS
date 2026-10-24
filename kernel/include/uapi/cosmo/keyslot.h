/*
 * keyslot.h - A key slot of an encrypted cosmofs pool, as user space
 * seals it and the kernel stores it (roadmap 1.2 B5;
 * docs/kernel-services/filesystem/cosmofs/design.md, "The key hierarchy").
 *
 * The layout is the on-disk struct cfs_keyslot's, byte for byte: what
 * /dev/fsctl's KEY_INFO hands out and what KEY_SLOT and /dev/blkctl's
 * FORMAT_KEYED take in are the slots as they are written.
 */
#ifndef UAPI_COSMO_KEYSLOT_H
#define UAPI_COSMO_KEYSLOT_H

#include <stdint.h>

#define COSMO_KEY_SLOTS 8u
#define COSMO_KEY_SIZE  32u   /* the master key */

#define COSMO_KEYSLOT_EMPTY      0u
#define COSMO_KEYSLOT_PASSPHRASE 1u   /* Argon2id(passphrase, salt, t_cost, m_cost_kib, lanes) */
#define COSMO_KEYSLOT_KEYFILE    2u   /* slot 0 only: BLAKE2b keyed by the file's 32 bytes, over the salt */

struct cosmo_keyslot {        /* 96 bytes */
    uint32_t kind;
    uint32_t t_cost;
    uint32_t m_cost_kib;
    uint32_t lanes;
    uint8_t salt[16];
    uint8_t wrapped[32];      /* the master key under ChaCha20-Poly1305 */
    uint8_t tag[16];
    uint64_t created_ns;
    uint64_t reserved;
};

#endif /* UAPI_COSMO_KEYSLOT_H */
