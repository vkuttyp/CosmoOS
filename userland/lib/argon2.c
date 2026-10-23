/*
 * argon2.c - Argon2id (RFC 9106) on the kernel's BLAKE2b, for the
 * passphrase that unlocks an encrypted root (roadmap 1.2 B5;
 * docs/kernel-services/filesystem/cosmofs/design.md, "Key slots").
 *
 * Single-threaded: the lanes are filled one after another, which gives
 * the same output as filling them at once (the format fixes the order of
 * what each block may reference, not the order of the work). Version
 * 0x13 only. The memory is one allocation, zeroed before it is freed.
 */
#include "argon2.h"

#include <kernel/blake2b.h>

#include <stdlib.h>
#include <string.h>

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

static void store32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* --- Argon2id ------------------------------------------------------------------ */

#define QWORDS 128          /* a block is 1024 bytes */
#define SYNC_POINTS 4
#define ADDRESSES 128       /* pseudo-random values per address block */

struct block {
    uint64_t v[QWORDS];
};

/* H' (RFC 9106 section 3.3): a hash of any length from BLAKE2b's 64. */
static void hprime(uint8_t *out, uint32_t outlen, const void *in, size_t inlen)
{
    uint8_t le[4];
    store32(le, outlen);
    struct blake2b s;
    if (outlen <= 64) {
        blake2b_init(&s, outlen);
        blake2b_update(&s, le, 4);
        blake2b_update(&s, in, inlen);
        blake2b_final(&s, out);
        return;
    }
    uint8_t v[64];
    blake2b_init(&s, 64);
    blake2b_update(&s, le, 4);
    blake2b_update(&s, in, inlen);
    blake2b_final(&s, v);
    memcpy(out, v, 32);
    out += 32;
    uint32_t left = outlen - 32;
    while (left > 64) {
        blake2b(v, 64, v, 64);
        memcpy(out, v, 32);
        out += 32;
        left -= 32;
    }
    blake2b(v, left, v, 64);
    memcpy(out, v, left);
    memset(v, 0, sizeof(v));
}

static uint64_t fblamka(uint64_t x, uint64_t y)
{
    return x + y + 2 * (uint64_t)(uint32_t)x * (uint64_t)(uint32_t)y;
}

#define GB(a, b, c, d)                                                                                                 \
    do {                                                                                                               \
        a = fblamka(a, b);                                                                                             \
        d = rotr64(d ^ a, 32);                                                                                         \
        c = fblamka(c, d);                                                                                             \
        b = rotr64(b ^ c, 24);                                                                                         \
        a = fblamka(a, b);                                                                                             \
        d = rotr64(d ^ a, 16);                                                                                         \
        c = fblamka(c, d);                                                                                             \
        b = rotr64(b ^ c, 63);                                                                                         \
    } while (0)

#define PERM(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15)                                     \
    do {                                                                                                               \
        GB(v0, v4, v8, v12);                                                                                           \
        GB(v1, v5, v9, v13);                                                                                           \
        GB(v2, v6, v10, v14);                                                                                          \
        GB(v3, v7, v11, v15);                                                                                          \
        GB(v0, v5, v10, v15);                                                                                          \
        GB(v1, v6, v11, v12);                                                                                          \
        GB(v2, v7, v8, v13);                                                                                           \
        GB(v3, v4, v9, v14);                                                                                           \
    } while (0)

/* G (section 3.5): next = P(prev ^ ref) ^ (prev ^ ref), and ^ next's old
 * value when `xor` (every pass after the first). */
static void fill_block(const struct block *prev, const struct block *ref, struct block *next, int xor)
{
    struct block r, t;
    for (int i = 0; i < QWORDS; i++)
        r.v[i] = prev->v[i] ^ ref->v[i];
    t = r;
    if (xor)
        for (int i = 0; i < QWORDS; i++)
            t.v[i] ^= next->v[i];
    uint64_t *v = r.v;
    for (int i = 0; i < 8; i++)
        PERM(v[16 * i], v[16 * i + 1], v[16 * i + 2], v[16 * i + 3], v[16 * i + 4], v[16 * i + 5], v[16 * i + 6],
             v[16 * i + 7], v[16 * i + 8], v[16 * i + 9], v[16 * i + 10], v[16 * i + 11], v[16 * i + 12],
             v[16 * i + 13], v[16 * i + 14], v[16 * i + 15]);
    for (int i = 0; i < 8; i++)
        PERM(v[2 * i], v[2 * i + 1], v[2 * i + 16], v[2 * i + 17], v[2 * i + 32], v[2 * i + 33], v[2 * i + 48],
             v[2 * i + 49], v[2 * i + 64], v[2 * i + 65], v[2 * i + 80], v[2 * i + 81], v[2 * i + 96],
             v[2 * i + 97], v[2 * i + 112], v[2 * i + 113]);
    for (int i = 0; i < QWORDS; i++)
        next->v[i] = t.v[i] ^ v[i];
}

struct instance {
    struct block *mem;
    uint32_t passes, lanes, lane_length, segment_length, blocks;
};

static void next_addresses(struct block *address, struct block *input, const struct block *zero)
{
    input->v[6]++;
    fill_block(zero, input, address, 0);
    fill_block(zero, address, address, 0);
}

static uint32_t index_alpha(const struct instance *in, uint32_t pass, uint32_t slice, uint32_t index,
                            uint32_t pseudo, int same_lane)
{
    uint32_t area;
    if (pass == 0) {
        if (slice == 0)
            area = index - 1;
        else if (same_lane)
            area = slice * in->segment_length + index - 1;
        else
            area = slice * in->segment_length + (index == 0 ? (uint32_t)-1 : 0);
    } else {
        if (same_lane)
            area = in->lane_length - in->segment_length + index - 1;
        else
            area = in->lane_length - in->segment_length + (index == 0 ? (uint32_t)-1 : 0);
    }
    uint64_t rel = pseudo;
    rel = rel * rel >> 32;
    rel = area - 1 - ((uint64_t)area * rel >> 32);
    uint32_t start = 0;
    if (pass != 0)
        start = slice == SYNC_POINTS - 1 ? 0 : (slice + 1) * in->segment_length;
    return (uint32_t)((start + rel) % in->lane_length);
}

static void fill_segment(const struct instance *in, uint32_t pass, uint32_t lane, uint32_t slice)
{
    struct block address, input, zero;
    int independent = pass == 0 && slice < SYNC_POINTS / 2;   /* Argon2id */
    if (independent) {
        memset(&zero, 0, sizeof(zero));
        memset(&input, 0, sizeof(input));
        input.v[0] = pass;
        input.v[1] = lane;
        input.v[2] = slice;
        input.v[3] = in->blocks;
        input.v[4] = in->passes;
        input.v[5] = ARGON2_ID;
    }
    uint32_t start = 0;
    if (pass == 0 && slice == 0) {
        start = 2;   /* the first two blocks of a lane are made from H0 */
        if (independent)
            next_addresses(&address, &input, &zero);
    }
    uint32_t curr = lane * in->lane_length + slice * in->segment_length + start;
    uint32_t prev = curr % in->lane_length == 0 ? curr + in->lane_length - 1 : curr - 1;
    for (uint32_t i = start; i < in->segment_length; i++, curr++, prev++) {
        if (curr % in->lane_length == 1)
            prev = curr - 1;
        uint64_t pseudo;
        if (independent) {
            if (i % ADDRESSES == 0)
                next_addresses(&address, &input, &zero);
            pseudo = address.v[i % ADDRESSES];
        } else {
            pseudo = in->mem[prev].v[0];
        }
        uint32_t ref_lane = (uint32_t)((pseudo >> 32) % in->lanes);
        if (pass == 0 && slice == 0)
            ref_lane = lane;
        uint32_t ref = index_alpha(in, pass, slice, i, (uint32_t)pseudo, ref_lane == lane);
        fill_block(&in->mem[prev], &in->mem[in->lane_length * ref_lane + ref], &in->mem[curr], pass != 0);
    }
    if (independent) {
        memset(&address, 0, sizeof(address));
        memset(&input, 0, sizeof(input));
    }
}

int argon2id(const struct argon2_params *p, const void *pwd, size_t pwdlen, const void *salt, size_t saltlen,
             uint8_t *out, size_t outlen)
{
    if (p->t_cost < 1 || p->lanes < 1 || p->lanes > 0xffffff || outlen < 4 || outlen > 0xffffffffu ||
        saltlen < 8 || p->m_cost_kib < 8 * p->lanes)
        return -1;
    struct instance in;
    in.passes = p->t_cost;
    in.lanes = p->lanes;
    uint32_t m = p->m_cost_kib / (SYNC_POINTS * p->lanes) * (SYNC_POINTS * p->lanes);
    in.blocks = m;
    in.lane_length = m / p->lanes;
    in.segment_length = in.lane_length / SYNC_POINTS;
    in.mem = calloc(m, sizeof(struct block));
    if (in.mem == NULL)
        return -2;

    /* H0 (section 3.2). */
    uint8_t h0[72], le[4];
    struct blake2b s;
    blake2b_init(&s, 64);
    uint32_t head[6] = { p->lanes, (uint32_t)outlen, p->m_cost_kib, p->t_cost, 0x13, ARGON2_ID };
    for (int i = 0; i < 6; i++) {
        store32(le, head[i]);
        blake2b_update(&s, le, 4);
    }
    const void *parts[4] = { pwd, salt, p->secret, p->ad };
    size_t lens[4] = { pwdlen, saltlen, p->secret ? p->secretlen : 0, p->ad ? p->adlen : 0 };
    for (int i = 0; i < 4; i++) {
        store32(le, (uint32_t)lens[i]);
        blake2b_update(&s, le, 4);
        if (lens[i])
            blake2b_update(&s, parts[i], lens[i]);
    }
    blake2b_final(&s, h0);

    uint8_t bytes[1024];
    for (uint32_t l = 0; l < in.lanes; l++) {
        for (uint32_t j = 0; j < 2; j++) {
            store32(h0 + 64, j);
            store32(h0 + 68, l);
            hprime(bytes, sizeof(bytes), h0, sizeof(h0));
            for (int q = 0; q < QWORDS; q++)
                in.mem[l * in.lane_length + j].v[q] = load64(bytes + 8 * q);
        }
    }
    for (uint32_t pass = 0; pass < in.passes; pass++)
        for (uint32_t slice = 0; slice < SYNC_POINTS; slice++)
            for (uint32_t l = 0; l < in.lanes; l++)
                fill_segment(&in, pass, l, slice);

    struct block final = in.mem[in.lane_length - 1];
    for (uint32_t l = 1; l < in.lanes; l++)
        for (int q = 0; q < QWORDS; q++)
            final.v[q] ^= in.mem[l * in.lane_length + in.lane_length - 1].v[q];
    for (int q = 0; q < QWORDS; q++)
        store64(bytes + 8 * q, final.v[q]);
    hprime(out, (uint32_t)outlen, bytes, sizeof(bytes));

    memset(bytes, 0, sizeof(bytes));
    memset(h0, 0, sizeof(h0));
    memset(&final, 0, sizeof(final));
    /* Through a volatile pointer, so the clearing is not dropped as a
     * store to memory about to be freed. */
    volatile uint8_t *vm = (volatile uint8_t *)in.mem;
    for (size_t i = 0; i < (size_t)m * sizeof(struct block); i++)
        vm[i] = 0;
    free(in.mem);
    return 0;
}
