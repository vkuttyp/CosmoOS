/*
 * keyring.c - Lookup over the built-in trusted key table.
 */

#include <kernel/keyring.h>
#include <kernel/string.h>

void keyring_key_id(const uint8_t pub[ED25519_PUBLIC_KEY_SIZE], uint8_t id[KEYRING_ID_SIZE])
{
    uint8_t digest[SHA512_DIGEST_SIZE];
    sha512(pub, ED25519_PUBLIC_KEY_SIZE, digest);
    memcpy(id, digest, KEYRING_ID_SIZE);
}

const struct trusted_key *keyring_find(const uint8_t id[KEYRING_ID_SIZE])
{
    for (unsigned i = 0; i < keyring_builtin_count; i++) {
        if (memcmp(keyring_builtin[i].id, id, KEYRING_ID_SIZE) == 0)
            return &keyring_builtin[i];
    }
    return NULL;
}

unsigned keyring_count(void)
{
    return keyring_builtin_count;
}

const struct trusted_key *keyring_entry(unsigned index)
{
    return index < keyring_builtin_count ? &keyring_builtin[index] : NULL;
}
