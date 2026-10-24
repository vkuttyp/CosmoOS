/*
 * argon2.h - Argon2id (RFC 9106), for passphrases (roadmap 1.2 B5;
 * userland/lib/argon2.c). BLAKE2b is the kernel's (kernel/blake2b.h).
 */
#ifndef USERLAND_ARGON2_H
#define USERLAND_ARGON2_H

#include <stddef.h>
#include <stdint.h>

#define ARGON2_ID 2u   /* the type field of H0: Argon2id */

struct argon2_params {
    uint32_t t_cost;       /* passes over the memory */
    uint32_t m_cost_kib;   /* memory, in KiB (1 KiB blocks) */
    uint32_t lanes;        /* parallelism; filled one lane after another here */
    const void *secret;    /* K, optional */
    size_t secretlen;
    const void *ad;        /* X, optional */
    size_t adlen;
};

/* The Argon2id tag of `pwd` and `salt` (at least 8 bytes) into `out`.
 * 0, -1 for parameters RFC 9106 does not allow, -2 when the memory
 * cannot be had. */
int argon2id(const struct argon2_params *p, const void *pwd, size_t pwdlen, const void *salt, size_t saltlen,
             uint8_t *out, size_t outlen);

#endif
