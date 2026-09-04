/*
 * modsig.c - Module signature trailer verification.
 */

#include <kernel/errno.h>
#include <kernel/modsig.h>
#include <kernel/string.h>

#ifndef CONFIG_MODULE_SIG_ENFORCE
#define CONFIG_MODULE_SIG_ENFORCE 1
#endif

bool modsig_enforced(void)
{
    return CONFIG_MODULE_SIG_ENFORCE != 0;
}

int modsig_check(const void *file, size_t size, size_t *payload_size, const char **why)
{
    const char *reason;
    int rc;

    if (size < sizeof(struct modsig_trailer)) {
        reason = "file shorter than a signature trailer";
        rc = -ENOKEY;
        goto out;
    }
    const uint8_t *bytes = file;
    struct modsig_trailer t;
    memcpy(&t, bytes + size - sizeof(t), sizeof(t));
    if (memcmp(t.magic, MODSIG_MAGIC, sizeof(t.magic)) != 0) {
        reason = "no signature trailer";
        rc = -ENOKEY;
        goto out;
    }
    if (t.version != MODSIG_VERSION) {
        reason = "unsupported signature version";
        rc = -EKEYREJECTED;
        goto out;
    }
    if (t.algo != MODSIG_ALGO_ED25519) {
        reason = "unsupported signature algorithm";
        rc = -EKEYREJECTED;
        goto out;
    }
    const struct trusted_key *key = keyring_find(t.key_id);
    if (key == NULL) {
        reason = "signing key not in the kernel key ring";
        rc = -ENOKEY;
        goto out;
    }
    size_t payload = size - sizeof(t);
    if (!ed25519_verify(t.sig, bytes, payload, key->pub)) {
        reason = "signature does not verify";
        rc = -EKEYREJECTED;
        goto out;
    }
    if (payload_size)
        *payload_size = payload;
    reason = "ok";
    rc = 0;
out:
    if (why)
        *why = reason;
    return rc;
}
