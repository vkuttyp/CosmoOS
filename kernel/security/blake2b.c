/*
 * blake2b.c - BLAKE2b (RFC 7693), plain and keyed (kernel/blake2b.h).
 *
 * The 64-bit word version: twelve rounds of add-rotate-xor over a
 * 128-byte block, with no tables -- the same work for every input.
 */

#include <kernel/blake2b.h>
#include <kernel/string.h>

static const uint64_t b2_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static const uint8_t b2_sigma[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 }, { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 }, { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 }, { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 }, { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
};

static uint64_t rotr64(uint64_t x, unsigned n)
{
    return (x >> n) | (x << (64 - n));
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void store64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}


static void b2_compress(struct blake2b *s, const uint8_t block[128], int last)
{
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; i++)
        m[i] = load64(block + 8 * i);
    for (int i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = b2_iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last)
        v[14] = ~v[14];
#define B2G(a, b, c, d, x, y)                                                                                          \
    do {                                                                                                               \
        v[a] = v[a] + v[b] + (x);                                                                                      \
        v[d] = rotr64(v[d] ^ v[a], 32);                                                                                \
        v[c] = v[c] + v[d];                                                                                            \
        v[b] = rotr64(v[b] ^ v[c], 24);                                                                                \
        v[a] = v[a] + v[b] + (y);                                                                                      \
        v[d] = rotr64(v[d] ^ v[a], 16);                                                                                \
        v[c] = v[c] + v[d];                                                                                            \
        v[b] = rotr64(v[b] ^ v[c], 63);                                                                                \
    } while (0)
    for (int r = 0; r < 12; r++) {
        const uint8_t *z = b2_sigma[r];
        B2G(0, 4, 8, 12, m[z[0]], m[z[1]]);
        B2G(1, 5, 9, 13, m[z[2]], m[z[3]]);
        B2G(2, 6, 10, 14, m[z[4]], m[z[5]]);
        B2G(3, 7, 11, 15, m[z[6]], m[z[7]]);
        B2G(0, 5, 10, 15, m[z[8]], m[z[9]]);
        B2G(1, 6, 11, 12, m[z[10]], m[z[11]]);
        B2G(2, 7, 8, 13, m[z[12]], m[z[13]]);
        B2G(3, 4, 9, 14, m[z[14]], m[z[15]]);
    }
#undef B2G
    for (int i = 0; i < 8; i++)
        s->h[i] ^= v[i] ^ v[i + 8];
}


void blake2b_init_key(struct blake2b *s, size_t outlen, const void *key, size_t keylen)
{
    if (key == NULL)
        keylen = 0;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++)
        s->h[i] = b2_iv[i];
    /* The parameter block's first word: digest length, key length,
     * fanout 1, depth 1 (RFC 7693 section 2.5). */
    s->h[0] ^= 0x01010000ULL ^ ((uint64_t)keylen << 8) ^ (uint64_t)outlen;
    s->outlen = outlen;
    /* A key is the first block of input, zero-padded to 128 bytes. */
    if (keylen) {
        uint8_t block[128];
        memset(block, 0, sizeof(block));
        memcpy(block, key, keylen);
        blake2b_update(s, block, sizeof(block));
        memset(block, 0, sizeof(block));
    }
}

void blake2b_init(struct blake2b *s, size_t outlen)
{
    blake2b_init_key(s, outlen, NULL, 0);
}

void blake2b_update(struct blake2b *s, const void *in, size_t n)
{
    const uint8_t *p = in;
    while (n > 0) {
        /* The last block is compressed by final, with the flag: keep a
         * full buffer until more input proves it is not the last. */
        if (s->buflen == 128) {
            s->t[0] += 128;
            if (s->t[0] < 128)
                s->t[1]++;
            b2_compress(s, s->buf, 0);
            s->buflen = 0;
        }
        size_t take = 128 - s->buflen < n ? 128 - s->buflen : n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        n -= take;
    }
}

void blake2b_final(struct blake2b *s, uint8_t *out)
{
    s->t[0] += s->buflen;
    if (s->t[0] < s->buflen)
        s->t[1]++;
    memset(s->buf + s->buflen, 0, 128 - s->buflen);
    b2_compress(s, s->buf, 1);
    uint8_t full[64];
    for (int i = 0; i < 8; i++)
        store64(full + 8 * i, s->h[i]);
    memcpy(out, full, s->outlen);
    memset(full, 0, sizeof(full));
    memset(s, 0, sizeof(*s));
}

void blake2b(uint8_t *out, size_t outlen, const void *in, size_t inlen)
{
    struct blake2b s;
    blake2b_init(&s, outlen);
    blake2b_update(&s, in, inlen);
    blake2b_final(&s, out);
}
