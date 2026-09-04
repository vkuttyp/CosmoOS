/*
 * modsig.h - Module signature trailer and its verification.
 *
 * A signed module is the ELF file followed by one struct modsig_trailer.
 * The magic is the last 8 bytes of the file so detection is a fixed
 * offset compare; the signature covers every byte before the trailer.
 * scripts/modsign.py writes it; modsig_check() verifies it.
 */

#ifndef KERNEL_MODSIG_H
#define KERNEL_MODSIG_H

#include <kernel/compiler.h>
#include <kernel/crypto.h>
#include <kernel/keyring.h>
#include <kernel/types.h>

#define MODSIG_MAGIC        "COSMOSIG"   /* 8 bytes, no NUL */
#define MODSIG_VERSION      1u
#define MODSIG_ALGO_ED25519 1u

struct modsig_trailer {
    uint8_t  sig[ED25519_SIGNATURE_SIZE];
    uint8_t  key_id[KEYRING_ID_SIZE];
    uint32_t version;
    uint32_t algo;
    uint8_t  magic[8];
};

STATIC_ASSERT(sizeof(struct modsig_trailer) == 88, "modsig trailer is 88 bytes");

/* Verify the trailer on file[0..size). On success returns 0 and sets
 * *payload_size to the ELF length (size minus the trailer). Failures:
 * -ENOKEY (no trailer, or the key id is not in the ring), -EKEYREJECTED
 * (wrong version or algorithm, or the signature does not verify). `why`
 * receives an immortal string. Pure apart from the CPU time. */
int modsig_check(const void *file, size_t size, size_t *payload_size, const char **why);

/* Build-time policy: CONFIG_MODULE_SIG_ENFORCE. */
bool modsig_enforced(void);

#endif /* KERNEL_MODSIG_H */
