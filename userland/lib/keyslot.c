/*
 * keyslot.c - Sealing and opening an encrypted pool's key slots
 * (keyslot.h; roadmap 1.2 B5).
 */
#include "keyslot.h"

#include <string.h>

#include <uapi/cosmo/syscall.h>   /* COSMO_E*: the host test builds this with a libc that has no EKEYREJECTED */

#include <kernel/blake2b.h>
#include <kernel/chacha20.h>

#include "../../kernel-services/filesystem/cosmofs/cosmofs_format.h"
#include "argon2.h"

_Static_assert(sizeof(struct cosmo_keyslot) == sizeof(struct cfs_keyslot), "a slot is the on-disk slot");

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

/* The slot key: Argon2id of the passphrase, or BLAKE2b keyed by the file. */
static int slot_key(const struct cosmo_keyslot *s, const void *secret, size_t len, uint8_t key[32])
{
    if (s->kind == COSMO_KEYSLOT_KEYFILE) {
        if (len != KEYFILE_SIZE)
            return -COSMO_EKEYREJECTED;
        struct blake2b b;
        blake2b_init_key(&b, 32, secret, len);
        blake2b_update(&b, s->salt, sizeof(s->salt));
        blake2b_final(&b, key);
        return 0;
    }
    if (s->kind != COSMO_KEYSLOT_PASSPHRASE)
        return -COSMO_EINVAL;
    struct argon2_params p = { .t_cost = s->t_cost, .m_cost_kib = s->m_cost_kib, .lanes = s->lanes };
    int rc = argon2id(&p, secret, len, s->salt, sizeof(s->salt), key, 32);
    return rc == 0 ? 0 : rc == -2 ? -COSMO_ENOMEM : -COSMO_EINVAL;
}

static int seal(const uint8_t uuid[16], unsigned index, struct cosmo_keyslot *s, const void *secret, size_t len,
                const uint8_t master[COSMO_KEY_SIZE])
{
    uint8_t key[32], nonce[12], aad[CFS_KEYSLOT_AAD];
    int rc = slot_key(s, secret, len, key);
    if (rc)
        return rc;
    cfs_keyslot_nonce(index, nonce);
    cfs_keyslot_aad(uuid, index, (const struct cfs_keyslot *)s, aad);
    memcpy(s->wrapped, master, COSMO_KEY_SIZE);
    chacha20_seal(key, nonce, aad, sizeof(aad), s->wrapped, sizeof(s->wrapped), s->tag);
    wipe(key, sizeof(key));
    return 0;
}

int keyslot_seal_passphrase(const uint8_t uuid[16], unsigned index, uint32_t t_cost, uint32_t m_cost_kib,
                            uint32_t lanes, const void *pw, size_t pwlen, const uint8_t salt[16],
                            const uint8_t master[COSMO_KEY_SIZE], struct cosmo_keyslot *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = COSMO_KEYSLOT_PASSPHRASE;
    out->t_cost = t_cost;
    out->m_cost_kib = m_cost_kib;
    out->lanes = lanes;
    memcpy(out->salt, salt, sizeof(out->salt));
    return seal(uuid, index, out, pw, pwlen, master);
}

int keyslot_seal_keyfile(const uint8_t uuid[16], const uint8_t file[KEYFILE_SIZE], const uint8_t salt[16],
                         const uint8_t master[COSMO_KEY_SIZE], struct cosmo_keyslot *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = COSMO_KEYSLOT_KEYFILE;
    memcpy(out->salt, salt, sizeof(out->salt));
    return seal(uuid, 0, out, file, KEYFILE_SIZE, master);
}

int keyslot_open(const uint8_t uuid[16], unsigned index, const struct cosmo_keyslot *s, const void *secret,
                 size_t len, uint8_t master[COSMO_KEY_SIZE])
{
    if (index >= COSMO_KEY_SLOTS || s->kind == COSMO_KEYSLOT_EMPTY)
        return -COSMO_EINVAL;
    uint8_t key[32], nonce[12], aad[CFS_KEYSLOT_AAD], buf[COSMO_KEY_SIZE];
    int rc = slot_key(s, secret, len, key);
    if (rc)
        return rc;
    cfs_keyslot_nonce(index, nonce);
    cfs_keyslot_aad(uuid, index, (const struct cfs_keyslot *)s, aad);
    memcpy(buf, s->wrapped, sizeof(buf));
    bool ok = chacha20_open(key, nonce, aad, sizeof(aad), buf, sizeof(buf), s->tag);
    wipe(key, sizeof(key));
    if (ok)
        memcpy(master, buf, COSMO_KEY_SIZE);
    wipe(buf, sizeof(buf));
    return ok ? 0 : -COSMO_EKEYREJECTED;
}
