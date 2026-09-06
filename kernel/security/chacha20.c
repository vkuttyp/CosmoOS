/*
 * chacha20.c - ChaCha20 and Poly1305 (RFC 8439).
 *
 * Both are written the way the RFC states them, because a reader who
 * knows the document should be able to check this against it line by
 * line. Poly1305's arithmetic is the 26-bit limb form: five limbs hold a
 * 130-bit number in 32-bit words with room to accumulate, which is what
 * keeps the reduction free of a 128-bit type the kernel does not have on
 * every target.
 */

#include <kernel/chacha20.h>
#include <kernel/string.h>

/* --- ChaCha20 ------------------------------------------------------------ */

static inline uint32_t rotl32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d)                                                         \
    do {                                                                       \
        a += b; d ^= a; d = rotl32(d, 16);                                     \
        c += d; b ^= c; b = rotl32(b, 12);                                     \
        a += b; d ^= a; d = rotl32(d, 8);                                      \
        c += d; b ^= c; b = rotl32(b, 7);                                      \
    } while (0)

static inline uint32_t load32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* "expand 32-byte k" */
static const uint32_t chacha_sigma[4] = { 0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u };

static void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                           const uint8_t nonce[CHACHA20_NONCE_SIZE], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = chacha_sigma[0];
    s[1] = chacha_sigma[1];
    s[2] = chacha_sigma[2];
    s[3] = chacha_sigma[3];
    for (unsigned i = 0; i < 8; i++)
        s[4 + i] = load32le(key + 4 * i);
    s[12] = counter;
    for (unsigned i = 0; i < 3; i++)
        s[13 + i] = load32le(nonce + 4 * i);

    memcpy(x, s, sizeof(x));
    for (unsigned i = 0; i < 10; i++) {   /* 20 rounds: 10 double rounds */
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (unsigned i = 0; i < 16; i++)
        store32le(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter, const uint8_t nonce[CHACHA20_NONCE_SIZE],
                  const void *in, void *out, size_t len)
{
    const uint8_t *ip = in;
    uint8_t *op = out;
    uint8_t block[64];
    while (len > 0) {
        chacha20_block(key, counter, nonce, block);
        size_t n = len < sizeof(block) ? len : sizeof(block);
        for (size_t i = 0; i < n; i++)
            op[i] = ip[i] ^ block[i];
        ip += n;
        op += n;
        len -= n;
        counter++;
    }
    memset(block, 0, sizeof(block));
}

/* --- Poly1305 ------------------------------------------------------------- */

struct poly1305_state {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
};

static void poly1305_init(struct poly1305_state *st, const uint8_t key[32])
{
    /* r is clamped: the RFC clears the top four bits of the top three
     * bytes and the bottom two bits of the top three, which is what
     * keeps the products inside 64 bits. */
    uint32_t t0 = load32le(key + 0), t1 = load32le(key + 4), t2 = load32le(key + 8), t3 = load32le(key + 12);
    st->r[0] = t0 & 0x3ffffffu;
    st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    st->r[4] = (t3 >> 8) & 0x00fffffu;
    for (unsigned i = 0; i < 5; i++)
        st->h[i] = 0;
    for (unsigned i = 0; i < 4; i++)
        st->pad[i] = load32le(key + 16 + 4 * i);
}

/*
 * Whole 16-byte blocks. `hibit` is the 2^128 the RFC appends to every
 * complete block; the padded last block passes 0, which is the only
 * difference between the two cases and the reason this takes it as an
 * argument rather than existing twice.
 */
static void poly1305_blocks(struct poly1305_state *st, const uint8_t *m, size_t len, uint32_t hibit)
{
    while (len >= 16) {
        uint32_t t0 = load32le(m + 0), t1 = load32le(m + 4), t2 = load32le(m + 8), t3 = load32le(m + 12);
        st->h[0] += t0 & 0x3ffffffu;
        st->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x3ffffffu;
        st->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x3ffffffu;
        st->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x3ffffffu;
        st->h[4] += (t3 >> 8) | hibit;

        /* h *= r mod 2^130 - 5, in 26-bit limbs: the 5s fold the bits
         * above 2^130 back down, which is what that modulus buys. */
        uint64_t d0 = (uint64_t)st->h[0] * st->r[0] + (uint64_t)st->h[1] * (5 * st->r[4]) +
                      (uint64_t)st->h[2] * (5 * st->r[3]) + (uint64_t)st->h[3] * (5 * st->r[2]) +
                      (uint64_t)st->h[4] * (5 * st->r[1]);
        uint64_t d1 = (uint64_t)st->h[0] * st->r[1] + (uint64_t)st->h[1] * st->r[0] +
                      (uint64_t)st->h[2] * (5 * st->r[4]) + (uint64_t)st->h[3] * (5 * st->r[3]) +
                      (uint64_t)st->h[4] * (5 * st->r[2]);
        uint64_t d2 = (uint64_t)st->h[0] * st->r[2] + (uint64_t)st->h[1] * st->r[1] +
                      (uint64_t)st->h[2] * st->r[0] + (uint64_t)st->h[3] * (5 * st->r[4]) +
                      (uint64_t)st->h[4] * (5 * st->r[3]);
        uint64_t d3 = (uint64_t)st->h[0] * st->r[3] + (uint64_t)st->h[1] * st->r[2] +
                      (uint64_t)st->h[2] * st->r[1] + (uint64_t)st->h[3] * st->r[0] +
                      (uint64_t)st->h[4] * (5 * st->r[4]);
        uint64_t d4 = (uint64_t)st->h[0] * st->r[4] + (uint64_t)st->h[1] * st->r[3] +
                      (uint64_t)st->h[2] * st->r[2] + (uint64_t)st->h[3] * st->r[1] +
                      (uint64_t)st->h[4] * st->r[0];

        uint32_t c = (uint32_t)(d0 >> 26);
        st->h[0] = (uint32_t)d0 & 0x3ffffffu;
        d1 += c;
        c = (uint32_t)(d1 >> 26);
        st->h[1] = (uint32_t)d1 & 0x3ffffffu;
        d2 += c;
        c = (uint32_t)(d2 >> 26);
        st->h[2] = (uint32_t)d2 & 0x3ffffffu;
        d3 += c;
        c = (uint32_t)(d3 >> 26);
        st->h[3] = (uint32_t)d3 & 0x3ffffffu;
        d4 += c;
        c = (uint32_t)(d4 >> 26);
        st->h[4] = (uint32_t)d4 & 0x3ffffffu;
        st->h[0] += c * 5;
        c = st->h[0] >> 26;
        st->h[0] &= 0x3ffffffu;
        st->h[1] += c;

        m += 16;
        len -= 16;
    }
}

void poly1305(const uint8_t key[32], const void *data, size_t len, uint8_t tag[POLY1305_TAG_SIZE])
{
    struct poly1305_state st;
    poly1305_init(&st, key);
    const uint8_t *m = data;
    size_t whole = len & ~(size_t)15;
    poly1305_blocks(&st, m, whole, 1u << 24);

    if (len > whole) {
        /* The short last block: the RFC appends a single 1 byte after
         * the data and pads with zeros, and no 2^128. */
        uint8_t last[16];
        memset(last, 0, sizeof(last));
        memcpy(last, m + whole, len - whole);
        last[len - whole] = 1;
        poly1305_blocks(&st, last, sizeof(last), 0);
        memset(last, 0, sizeof(last));
    }

    /* Carry the limbs, then subtract p = 2^130 - 5 if h is at least p. */
    uint32_t c = st.h[1] >> 26;
    st.h[1] &= 0x3ffffffu;
    st.h[2] += c;
    c = st.h[2] >> 26;
    st.h[2] &= 0x3ffffffu;
    st.h[3] += c;
    c = st.h[3] >> 26;
    st.h[3] &= 0x3ffffffu;
    st.h[4] += c;
    c = st.h[4] >> 26;
    st.h[4] &= 0x3ffffffu;
    st.h[0] += c * 5;
    c = st.h[0] >> 26;
    st.h[0] &= 0x3ffffffu;
    st.h[1] += c;

    uint32_t g[5];
    uint32_t gc = 5;
    for (unsigned i = 0; i < 5; i++) {
        gc += st.h[i];
        g[i] = gc & 0x3ffffffu;
        gc >>= 26;
    }
    /* gc is 1 exactly when h >= p. Choosing without a branch keeps the
     * time independent of the value, which is the whole point of a MAC
     * that an attacker gets to guess at. */
    uint32_t mask = (uint32_t)0 - gc;   /* all ones when gc == 1 */
    for (unsigned i = 0; i < 5; i++)
        st.h[i] = (st.h[i] & ~mask) | (g[i] & mask);

    uint32_t f0 = (st.h[0] | (st.h[1] << 26));
    uint32_t f1 = ((st.h[1] >> 6) | (st.h[2] << 20));
    uint32_t f2 = ((st.h[2] >> 12) | (st.h[3] << 14));
    uint32_t f3 = ((st.h[3] >> 18) | (st.h[4] << 8));

    uint64_t t = (uint64_t)f0 + st.pad[0];
    store32le(tag + 0, (uint32_t)t);
    t = (uint64_t)f1 + st.pad[1] + (t >> 32);
    store32le(tag + 4, (uint32_t)t);
    t = (uint64_t)f2 + st.pad[2] + (t >> 32);
    store32le(tag + 8, (uint32_t)t);
    t = (uint64_t)f3 + st.pad[3] + (t >> 32);
    store32le(tag + 12, (uint32_t)t);

    memset(&st, 0, sizeof(st));
}

bool poly1305_verify(const uint8_t a[POLY1305_TAG_SIZE], const uint8_t b[POLY1305_TAG_SIZE])
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < POLY1305_TAG_SIZE; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* --- the shape the filesystem uses --------------------------------------- */

/* The Poly1305 key is the first keystream block, so the data starts at
 * counter 1 (RFC 8439 §2.6). */
static void one_time_key(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE],
                         uint8_t out[32])
{
    uint8_t block[64];
    chacha20_block(key, 0, nonce, block);
    memcpy(out, block, 32);
    memset(block, 0, sizeof(block));
}

void chacha20_tag_only(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE],
                       const void *data, size_t len, uint8_t tag[POLY1305_TAG_SIZE])
{
    uint8_t otk[32];
    one_time_key(key, nonce, otk);
    poly1305(otk, data, len, tag);
    memset(otk, 0, sizeof(otk));
}

void chacha20_seal(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE], void *data,
                   size_t len, uint8_t tag[POLY1305_TAG_SIZE])
{
    chacha20_xor(key, 1, nonce, data, data, len);
    chacha20_tag_only(key, nonce, data, len, tag);
}

bool chacha20_open(const uint8_t key[CHACHA20_KEY_SIZE], const uint8_t nonce[CHACHA20_NONCE_SIZE], void *data,
                   size_t len, const uint8_t tag[POLY1305_TAG_SIZE])
{
    uint8_t want[POLY1305_TAG_SIZE];
    chacha20_tag_only(key, nonce, data, len, want);
    if (!poly1305_verify(want, tag))
        return false;   /* a forged block never becomes plaintext */
    chacha20_xor(key, 1, nonce, data, data, len);
    return true;
}
