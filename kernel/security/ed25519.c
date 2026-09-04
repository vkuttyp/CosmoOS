/*
 * ed25519.c - Ed25519 signature verification (RFC 8032, section 5.1.7).
 *
 * Field elements are 5 limbs of 51 bits over 2^255-19 with 128-bit
 * intermediates; points are extended twisted Edwards coordinates
 * (X, Y, Z, T) and every group operation is the unified addition law,
 * so doubling is P + P. Scalar multiplication is plain double-and-add
 * scanning the scalar from the top. Everything is variable-time on
 * purpose: this file only verifies, and every input to verification
 * (message, signature, public key) is public. Constants below were
 * computed once with Python and are the limb form of d, 2d, sqrt(-1)
 * and the base point.
 */

#include <kernel/crypto.h>
#include <kernel/string.h>

typedef unsigned __int128 u128;

#define MASK51 ((1ULL << 51) - 1)

typedef struct {
    uint64_t v[5];
} fe;

typedef struct {
    fe x, y, z, t;
} ge;

static const fe FE_ZERO = { { 0, 0, 0, 0, 0 } };
static const fe FE_ONE = { { 1, 0, 0, 0, 0 } };
static const fe FE_D = { { 0x34dca135978a3ULL, 0x1a8283b156ebdULL, 0x5e7a26001c029ULL, 0x739c663a03cbbULL,
                           0x52036cee2b6ffULL } };
static const fe FE_D2 = { { 0x69b9426b2f159ULL, 0x35050762add7aULL, 0x3cf44c0038052ULL, 0x6738cc7407977ULL,
                            0x2406d9dc56dffULL } };
static const fe FE_SQRTM1 = { { 0x61b274a0ea0b0ULL, 0x0d5a5fc8f189dULL, 0x7ef5e9cbd0c60ULL, 0x78595a6804c9eULL,
                                0x2b8324804fc1dULL } };
static const ge GE_BASE = {
    { { 0x62d608f25d51aULL, 0x412a4b4f6592aULL, 0x75b7171a4b31dULL, 0x1ff60527118feULL, 0x216936d3cd6e5ULL } },
    { { 0x6666666666658ULL, 0x4ccccccccccccULL, 0x1999999999999ULL, 0x3333333333333ULL, 0x6666666666666ULL } },
    { { 1, 0, 0, 0, 0 } },
    { { 0x68ab3a5b7dda3ULL, 0x00eea2a5eadbbULL, 0x2af8df483c27eULL, 0x332b375274732ULL, 0x67875f0fd78b7ULL } },
};

/* Group order L, little endian. */
static const uint8_t L_BYTES[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};
static const uint64_t L_WORDS[4] = { 0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0ULL, 0x1000000000000000ULL };

/* --- field ------------------------------------------------------------ */

static inline uint64_t load64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void store64_le(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Bit 255 is ignored (it carries the sign of x in point encodings). */
static void fe_frombytes(fe *h, const uint8_t s[32])
{
    h->v[0] = load64_le(s) & MASK51;
    h->v[1] = (load64_le(s + 6) >> 3) & MASK51;
    h->v[2] = (load64_le(s + 12) >> 6) & MASK51;
    h->v[3] = (load64_le(s + 19) >> 1) & MASK51;
    h->v[4] = (load64_le(s + 24) >> 12) & MASK51;
}

static void fe_carry(fe *h)
{
    uint64_t c;
    c = h->v[0] >> 51; h->v[0] &= MASK51; h->v[1] += c;
    c = h->v[1] >> 51; h->v[1] &= MASK51; h->v[2] += c;
    c = h->v[2] >> 51; h->v[2] &= MASK51; h->v[3] += c;
    c = h->v[3] >> 51; h->v[3] &= MASK51; h->v[4] += c;
    c = h->v[4] >> 51; h->v[4] &= MASK51; h->v[0] += 19 * c;
    c = h->v[0] >> 51; h->v[0] &= MASK51; h->v[1] += c;
}

/* Canonical little-endian encoding of h mod p. */
static void fe_tobytes(uint8_t s[32], const fe *in)
{
    fe t = *in;
    fe_carry(&t);
    fe_carry(&t);
    /* t < 2^255. If t + 19 >= 2^255 then t >= p: the reduced value is
     * t + 19 with the top bit dropped. */
    fe u = t;
    u.v[0] += 19;
    fe_carry(&u);
    if (u.v[4] >> 51) {
        u.v[4] &= MASK51;
        t = u;
    }
    store64_le(s, t.v[0] | (t.v[1] << 51));
    store64_le(s + 8, (t.v[1] >> 13) | (t.v[2] << 38));
    store64_le(s + 16, (t.v[2] >> 26) | (t.v[3] << 25));
    store64_le(s + 24, (t.v[3] >> 39) | (t.v[4] << 12));
}

/* Every operation returns a carried element (limbs below 2^52), so any
 * output can be any input, including the subtrahend of fe_sub. */
static void fe_add(fe *h, const fe *a, const fe *b)
{
    for (unsigned i = 0; i < 5; i++)
        h->v[i] = a->v[i] + b->v[i];
    fe_carry(h);
}

/* a - b + 2p: with b carried (limbs < 2^52 - 38) no limb goes negative. */
static void fe_sub(fe *h, const fe *a, const fe *b)
{
    h->v[0] = a->v[0] + 0xFFFFFFFFFFFDAULL - b->v[0];
    h->v[1] = a->v[1] + 0xFFFFFFFFFFFFEULL - b->v[1];
    h->v[2] = a->v[2] + 0xFFFFFFFFFFFFEULL - b->v[2];
    h->v[3] = a->v[3] + 0xFFFFFFFFFFFFEULL - b->v[3];
    h->v[4] = a->v[4] + 0xFFFFFFFFFFFFEULL - b->v[4];
    fe_carry(h);
}

/* Inputs are carried (limbs < 2^52); the output is carried too. */
static void fe_mul(fe *h, const fe *f, const fe *g)
{
    uint64_t f0 = f->v[0], f1 = f->v[1], f2 = f->v[2], f3 = f->v[3], f4 = f->v[4];
    uint64_t g0 = g->v[0], g1 = g->v[1], g2 = g->v[2], g3 = g->v[3], g4 = g->v[4];
    uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;

    u128 r0 = (u128)f0 * g0 + (u128)f1 * g4_19 + (u128)f2 * g3_19 + (u128)f3 * g2_19 + (u128)f4 * g1_19;
    u128 r1 = (u128)f0 * g1 + (u128)f1 * g0 + (u128)f2 * g4_19 + (u128)f3 * g3_19 + (u128)f4 * g2_19;
    u128 r2 = (u128)f0 * g2 + (u128)f1 * g1 + (u128)f2 * g0 + (u128)f3 * g4_19 + (u128)f4 * g3_19;
    u128 r3 = (u128)f0 * g3 + (u128)f1 * g2 + (u128)f2 * g1 + (u128)f3 * g0 + (u128)f4 * g4_19;
    u128 r4 = (u128)f0 * g4 + (u128)f1 * g3 + (u128)f2 * g2 + (u128)f3 * g1 + (u128)f4 * g0;

    uint64_t c;
    uint64_t h0, h1, h2, h3, h4;
    c = (uint64_t)(r0 >> 51); h0 = (uint64_t)r0 & MASK51; r1 += c;
    c = (uint64_t)(r1 >> 51); h1 = (uint64_t)r1 & MASK51; r2 += c;
    c = (uint64_t)(r2 >> 51); h2 = (uint64_t)r2 & MASK51; r3 += c;
    c = (uint64_t)(r3 >> 51); h3 = (uint64_t)r3 & MASK51; r4 += c;
    c = (uint64_t)(r4 >> 51); h4 = (uint64_t)r4 & MASK51; h0 += 19 * c;
    c = h0 >> 51; h0 &= MASK51; h1 += c;

    h->v[0] = h0;
    h->v[1] = h1;
    h->v[2] = h2;
    h->v[3] = h3;
    h->v[4] = h4;
}

static void fe_sq(fe *h, const fe *f)
{
    fe_mul(h, f, f);
}

/* z^e for a little-endian exponent of `nbits` bits. */
static void fe_pow(fe *out, const fe *z, const uint8_t *exp_le, unsigned nbits)
{
    fe r = FE_ONE;
    for (int i = (int)nbits - 1; i >= 0; i--) {
        fe_sq(&r, &r);
        if ((exp_le[i / 8] >> (i % 8)) & 1)
            fe_mul(&r, &r, z);
    }
    *out = r;
}

/* (p - 5) / 8 = 2^252 - 3 */
static const uint8_t EXP_PM5D8[32] = {
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f,
};

static bool fe_iszero(const fe *f)
{
    uint8_t s[32];
    fe_tobytes(s, f);
    for (unsigned i = 0; i < 32; i++) {
        if (s[i] != 0)
            return false;
    }
    return true;
}

static bool fe_equal(const fe *a, const fe *b)
{
    uint8_t sa[32], sb[32];
    fe_tobytes(sa, a);
    fe_tobytes(sb, b);
    return memcmp(sa, sb, 32) == 0;
}

static bool fe_isodd(const fe *f)
{
    uint8_t s[32];
    fe_tobytes(s, f);
    return (s[0] & 1) != 0;
}

/* --- group ------------------------------------------------------------- */

/* Unified addition (add-2008-hwcd-3), valid for doubling too. */
static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe a, b, c, d, e, f, g, h, t;

    fe_sub(&t, &p->y, &p->x);
    fe_sub(&a, &q->y, &q->x);
    fe_mul(&a, &t, &a);          /* A = (Y1-X1)(Y2-X2) */
    fe_add(&t, &p->y, &p->x);
    fe_add(&b, &q->y, &q->x);
    fe_mul(&b, &t, &b);          /* B = (Y1+X1)(Y2+X2) */
    fe_mul(&c, &p->t, &q->t);
    fe_mul(&c, &c, &FE_D2);      /* C = 2d T1 T2 */
    fe_mul(&d, &p->z, &q->z);
    fe_add(&d, &d, &d);          /* D = 2 Z1 Z2 */
    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &c);
    fe_add(&g, &d, &c);
    fe_add(&h, &b, &a);
    fe_mul(&r->x, &e, &f);
    fe_mul(&r->y, &g, &h);
    fe_mul(&r->t, &e, &h);
    fe_mul(&r->z, &f, &g);
}

static void ge_neutral(ge *r)
{
    r->x = FE_ZERO;
    r->y = FE_ONE;
    r->z = FE_ONE;
    r->t = FE_ZERO;
}

/* r = [s]p, s little endian, scanned from bit 255 down. */
static void ge_scalarmult(ge *r, const uint8_t s[32], const ge *p)
{
    ge q;
    ge_neutral(&q);
    for (int i = 255; i >= 0; i--) {
        ge_add(&q, &q, &q);
        if ((s[i / 8] >> (i % 8)) & 1)
            ge_add(&q, &q, p);
    }
    *r = q;
}

/* RFC 8032 5.1.3 decoding. false for non-canonical y, no square root,
 * or x = 0 with the sign bit set. */
static bool ge_frombytes(ge *r, const uint8_t s[32])
{
    fe y;
    fe_frombytes(&y, s);
    uint8_t canon[32];
    fe_tobytes(canon, &y);
    if ((canon[31] & 0x7f) != (s[31] & 0x7f) || memcmp(canon, s, 31) != 0)
        return false;                       /* y >= p */
    unsigned sign = s[31] >> 7;

    fe y2, u, v, v3, v7, x, x2, t;
    fe_sq(&y2, &y);
    fe_sub(&u, &y2, &FE_ONE);               /* u = y^2 - 1 */
    fe_mul(&v, &y2, &FE_D);
    fe_add(&v, &v, &FE_ONE);                /* v = d y^2 + 1 */

    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);                   /* v^3 */
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, &v);                   /* v^7 */
    fe_mul(&t, &u, &v7);                    /* u v^7 */
    fe_pow(&t, &t, EXP_PM5D8, 252);         /* (u v^7)^((p-5)/8) */
    fe_mul(&x, &u, &v3);
    fe_mul(&x, &x, &t);                     /* x = u v^3 (u v^7)^((p-5)/8) */

    fe_sq(&x2, &x);
    fe_mul(&t, &v, &x2);                    /* v x^2 */
    if (!fe_equal(&t, &u)) {
        fe negu;
        fe_sub(&negu, &FE_ZERO, &u);
        if (!fe_equal(&t, &negu))
            return false;
        fe_mul(&x, &x, &FE_SQRTM1);
    }

    if (fe_iszero(&x)) {
        if (sign)
            return false;
    } else if (fe_isodd(&x) != (sign != 0)) {
        fe_sub(&x, &FE_ZERO, &x);
    }

    r->x = x;
    r->y = y;
    r->z = FE_ONE;
    fe_mul(&r->t, &x, &y);
    return true;
}

/* Projective equality without inversions: X1 Z2 == X2 Z1 and Y1 Z2 == Y2 Z1. */
static bool ge_equal(const ge *p, const ge *q)
{
    fe a, b;
    fe_mul(&a, &p->x, &q->z);
    fe_mul(&b, &q->x, &p->z);
    if (!fe_equal(&a, &b))
        return false;
    fe_mul(&a, &p->y, &q->z);
    fe_mul(&b, &q->y, &p->z);
    return fe_equal(&a, &b);
}

/* --- scalars ------------------------------------------------------------ */

/* out = in mod L for a 64-byte little-endian input, by shift-and-subtract
 * over 256-bit words (the remainder always stays below 2L). */
static void sc_reduce512(uint8_t out[32], const uint8_t in[64])
{
    uint64_t r[4] = { 0, 0, 0, 0 };
    for (int i = 511; i >= 0; i--) {
        uint64_t top = r[3] >> 63;
        r[3] = (r[3] << 1) | (r[2] >> 63);
        r[2] = (r[2] << 1) | (r[1] >> 63);
        r[1] = (r[1] << 1) | (r[0] >> 63);
        r[0] = (r[0] << 1) | ((in[i / 8] >> (i % 8)) & 1);
        (void)top;   /* r < 2L < 2^254 before the shift, so no bit is lost */

        bool ge_l = false;
        for (int j = 3; j >= 0; j--) {
            if (r[j] != L_WORDS[j]) {
                ge_l = r[j] > L_WORDS[j];
                break;
            }
            if (j == 0)
                ge_l = true;
        }
        if (ge_l) {
            uint64_t borrow = 0;
            for (unsigned j = 0; j < 4; j++) {
                u128 d = (u128)r[j] - L_WORDS[j] - borrow;
                r[j] = (uint64_t)d;
                borrow = (uint64_t)(d >> 64) & 1;
            }
        }
    }
    for (unsigned j = 0; j < 4; j++)
        store64_le(out + 8 * j, r[j]);
}

static bool sc_below_l(const uint8_t s[32])
{
    for (int i = 31; i >= 0; i--) {
        if (s[i] < L_BYTES[i])
            return true;
        if (s[i] > L_BYTES[i])
            return false;
    }
    return false;   /* equal to L */
}

/* --- verification --------------------------------------------------------- */

bool ed25519_verify(const uint8_t sig[ED25519_SIGNATURE_SIZE], const void *msg, size_t len,
                    const uint8_t pub[ED25519_PUBLIC_KEY_SIZE])
{
    ge a, r;
    if (!ge_frombytes(&a, pub))
        return false;
    if (!ge_frombytes(&r, sig))
        return false;
    if (!sc_below_l(sig + 32))
        return false;

    uint8_t hash[SHA512_DIGEST_SIZE];
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pub, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, hash);

    uint8_t k[32];
    sc_reduce512(k, hash);

    ge sb, ka, rhs;
    ge_scalarmult(&sb, sig + 32, &GE_BASE);   /* [S]B */
    ge_scalarmult(&ka, k, &a);                /* [k]A */
    ge_add(&rhs, &r, &ka);                    /* R + [k]A */
    return ge_equal(&sb, &rhs);
}
