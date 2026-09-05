/*
 * verify.c - Signatures and checksums: the COSMOSIG trailer, the key ring
 * under /etc/pkg/keys, SHA-512 (docs/pkg/design.md). The SHA-512 and
 * Ed25519 code is the kernel's, compiled for user mode.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kernel/crypto.h>

#include "pkg.h"
#include "verify.h"

#define TRAILER_SIZE 88u
#define MAGIC        "COSMOSIG"

struct key {
    uint8_t id[8];
    uint8_t pub[32];
    char name[64];
};

static struct key g_keys[16];
static int g_nkeys;

int ring_load(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_nkeys < 16) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".pub") != 0)
            continue;
        char path[PKG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        char hex[80];
        ssize_t n = read(fd, hex, sizeof(hex) - 1);
        close(fd);
        if (n < 64)
            continue;
        hex[64] = '\0';
        struct key *k = &g_keys[g_nkeys];
        if (!hex_decode(hex, k->pub, 32))
            continue;
        uint8_t digest[SHA512_DIGEST_SIZE];
        sha512(k->pub, 32, digest);
        memcpy(k->id, digest, 8);
        strlcpy(k->name, e->d_name, sizeof(k->name));
        g_nkeys++;
    }
    closedir(d);
    return g_nkeys;
}

int trailer_split(const uint8_t *blob, size_t len, size_t *payload_len, const uint8_t **sig, const uint8_t **key_id,
                  char *err, size_t errlen)
{
    if (len < TRAILER_SIZE || memcmp(blob + len - 8, MAGIC, 8) != 0) {
        snprintf(err, errlen, "not signed (no COSMOSIG trailer)");
        return -1;
    }
    const uint8_t *t = blob + len - TRAILER_SIZE;
    uint32_t version, algo;
    memcpy(&version, t + 72, 4);
    memcpy(&algo, t + 76, 4);
    if (version != 1 || algo != 1) {
        snprintf(err, errlen, "unsupported signature version %u algorithm %u", version, algo);
        return -1;
    }
    *payload_len = len - TRAILER_SIZE;
    *sig = t;
    *key_id = t + 64;
    return 0;
}

bool ring_verify(const uint8_t *payload, size_t len, const uint8_t *sig, const uint8_t *key_id, char *err,
                 size_t errlen)
{
    for (int i = 0; i < g_nkeys; i++) {
        if (memcmp(g_keys[i].id, key_id, 8) != 0)
            continue;
        if (ed25519_verify(sig, payload, len, g_keys[i].pub))
            return true;
        snprintf(err, errlen, "bad signature (key %s)", g_keys[i].name);
        return false;
    }
    char hex[17];
    hex_encode(key_id, 8, hex);
    snprintf(err, errlen, "unknown signing key %s (not in %s)", hex, PKG_KEYS_DIR);
    return false;
}

int verify_signed(const uint8_t *blob, size_t len, size_t *payload_len, char *err, size_t errlen)
{
    const uint8_t *sig, *kid;
    if (trailer_split(blob, len, payload_len, &sig, &kid, err, errlen) < 0)
        return -1;
    return ring_verify(blob, *payload_len, sig, kid, err, errlen) ? 0 : -1;
}

void sha512_of(const void *data, size_t len, uint8_t out[SHA512_LEN])
{
    sha512(data, len, out);
}
