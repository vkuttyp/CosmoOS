/*
 * relsig.c - A release's signed SHA256SUMS (relsig.h).
 */
#include "relsig.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <kernel/crypto.h>

struct relsig_key relsig_keys[RELSIG_MAX_KEYS];
int relsig_nkeys;

/* --- the key ring (the same trust root as pkg, docs/pkg/invariants.md PK9) ------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void relsig_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool unhex(const char *text, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(text[2 * i]), lo = hexval(text[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

void relsig_ring_load(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && relsig_nkeys < RELSIG_MAX_KEYS) {
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".pub") != 0)
            continue;
        char path[300], text[80];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (n < 64)
            continue;
        struct relsig_key *k = &relsig_keys[relsig_nkeys];
        if (!unhex(text, k->pub, 32))
            continue;
        uint8_t digest[SHA512_DIGEST_SIZE];
        sha512(k->pub, 32, digest);
        memcpy(k->id, digest, 8);
        snprintf(k->name, sizeof(k->name), "%s", e->d_name);
        relsig_nkeys++;
    }
    closedir(d);
}

const struct relsig_key *relsig_key_find(const uint8_t id[8])
{
    for (int i = 0; i < relsig_nkeys; i++)
        if (memcmp(relsig_keys[i].id, id, 8) == 0)
            return &relsig_keys[i];
    return NULL;
}

/* --- SHA-256 (FIPS 180-4) ----------------------------------------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return x >> n | x << (32 - n);
}

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void relsig_sha256(const void *data, size_t len, uint8_t out[RELSIG_SHA256_SIZE])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t left = len;
    for (; left >= 64; left -= 64, p += 64)
        sha256_block(h, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tlen = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tlen == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* --- SHA256SUMS and its signature -------------------------------------------------- */

int relsig_sums_verify(const uint8_t *sums, size_t len, const uint8_t *sig, size_t siglen, char keyid[17],
                       char *err, size_t errlen)
{
    snprintf(keyid, 17, "(none)");
    uint32_t version = 0, algo = 0;
    if (siglen == RELSIG_SIG_SIZE) {
        memcpy(&version, sig + 72, 4);
        memcpy(&algo, sig + 76, 4);
    }
    if (siglen != RELSIG_SIG_SIZE || memcmp(sig + 80, "COSMOSIG", 8) != 0 || version != 1 || algo != 1) {
        snprintf(err, errlen, "SHA256SUMS.sig: not a signature (%zu bytes; want the %d-byte COSMOSIG trailer, version 1, "
                              "Ed25519)", siglen, RELSIG_SIG_SIZE);
        return -1;
    }
    relsig_hex(sig + 64, 8, keyid);
    const struct relsig_key *k = relsig_key_find(sig + 64);
    if (k == NULL) {
        snprintf(err, errlen, "SHA256SUMS: unknown signing key %s (not in %s)", keyid, RELSIG_KEYS_DIR);
        return -1;
    }
    if (!ed25519_verify(sig, sums, len, k->pub)) {
        snprintf(err, errlen, "SHA256SUMS: bad signature (key %s)", k->name);
        return -1;
    }
    return 0;
}

int relsig_sums_lookup(const uint8_t *sums, size_t len, const char *name, uint8_t digest[RELSIG_SHA256_SIZE],
                       char *err, size_t errlen)
{
    size_t nl = strlen(name);
    int found = 0;
    for (size_t at = 0; at < len;) {
        const uint8_t *line = sums + at;
        const uint8_t *nlp = memchr(line, '\n', len - at);
        size_t ll = nlp ? (size_t)(nlp - line) : len - at;
        at += ll + 1;
        /* `sha256sum` text mode: 64 hex digits, two spaces, the name. */
        if (ll < 67 || line[64] != ' ' || line[65] != ' ') {
            snprintf(err, errlen, "SHA256SUMS: a line not of the form '<sha256>  <name>'");
            return -1;
        }
        uint8_t d[RELSIG_SHA256_SIZE];
        if (!unhex((const char *)line, d, sizeof(d))) {
            snprintf(err, errlen, "SHA256SUMS: a digest that is not hex");
            return -1;
        }
        if (ll - 66 == nl && memcmp(line + 66, name, nl) == 0) {
            if (found++) {
                snprintf(err, errlen, "SHA256SUMS lists %s twice", name);
                return -1;
            }
            memcpy(digest, d, sizeof(d));
        }
    }
    if (!found) {
        snprintf(err, errlen, "%s is not in SHA256SUMS", name);
        return -1;
    }
    return 0;
}
