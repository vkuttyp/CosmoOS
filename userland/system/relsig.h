/*
 * relsig.h - A release's signed SHA256SUMS (docs/userland/design.md,
 * "Updating"): the key ring in /etc/pkg/keys, SHA-256, and the detached
 * signature SHA256SUMS.sig, which is the COSMOSIG trailer of
 * scripts/modsign.py on its own (signature, key id, version, algorithm,
 * magic) over SHA256SUMS's bytes. cosmo-upgrade and cosmo-verify check a
 * release with it.
 */
#ifndef USERLAND_RELSIG_H
#define USERLAND_RELSIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELSIG_KEYS_DIR "/etc/pkg/keys"
#define RELSIG_MAX_KEYS 16
#define RELSIG_SIG_SIZE 88
#define RELSIG_SHA256_SIZE 32

struct relsig_key {
    uint8_t id[8];   /* the first 8 bytes of the public key's SHA-512 */
    uint8_t pub[32];
    char name[64];   /* its file name in the ring */
};

extern struct relsig_key relsig_keys[RELSIG_MAX_KEYS];
extern int relsig_nkeys;

/* Every `*.pub` in `dir` (64 hex characters each); unreadable files are
 * skipped, as pkg skips them. */
void relsig_ring_load(const char *dir);
const struct relsig_key *relsig_key_find(const uint8_t id[8]);

void relsig_hex(const uint8_t *in, size_t n, char *out);
void relsig_sha256(const void *data, size_t len, uint8_t out[RELSIG_SHA256_SIZE]);

/* SHA256SUMS against its detached signature and the ring: 0, or -1 with
 * `err`. `keyid` (17 bytes) names the signing key once the signature is
 * well formed, "(none)" before. */
int relsig_sums_verify(const uint8_t *sums, size_t len, const uint8_t *sig, size_t siglen, char keyid[17],
                       char *err, size_t errlen);

/* The digest SHA256SUMS lists for `name`: 0, or -1 with `err` (not
 * listed, listed twice, or a line not of the `sha256sum` form). */
int relsig_sums_lookup(const uint8_t *sums, size_t len, const char *name, uint8_t digest[RELSIG_SHA256_SIZE],
                       char *err, size_t errlen);

#endif
