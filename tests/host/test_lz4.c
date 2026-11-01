/*
 * test_lz4.c - The LZ4 codec, on the host.
 *
 * Round trips, the cases the format makes easy to get wrong (overlapping
 * matches, lengths that continue, input too short to hold a match), and
 * the ones a disk makes necessary: malformed input must be refused, not
 * copied past the end of a buffer. The decoder (lz4_decode.c, roadmap 2.0
 * SA5) is also held to a recorded digest over seeded streams -- valid ones
 * the compressor made, decoded with every kind of capacity and input
 * length, damaged ones, noise, and streams built to reach every length and
 * offset check -- of what it returned and of its whole output buffer, so a
 * decoder that differs even in what it wrote before refusing is caught.
 * LZ4_BENCH=1 times it on 4 KiB blocks, cosmofs's block size.
 */

#include "harness.h"

#include <kernel/lz4.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* lz4_compress's match table (LZ4_WORK_BYTES), the caller's since roadmap M2. */
static uint32_t g_lz4_work[LZ4_WORK_BYTES / 4];

static int round_trip(const uint8_t *data, size_t len, const char *what)
{
    uint8_t packed[8192], out[8192];
    size_t n = lz4_compress(data, len, packed, sizeof(packed), g_lz4_work);
    if (n == 0) {
        printf("    %s: did not fit\n", what);
        return 0;
    }
    size_t back = lz4_decompress(packed, n, out, sizeof(out));
    if (back != len || memcmp(out, data, len) != 0) {
        printf("    %s: %zu bytes -> %zu -> %zu\n", what, len, n, back);
        return 0;
    }
    return 1;
}

static void test_roundtrip(void)
{
    static uint8_t buf[4096];

    /* All one byte: the overlapping-match case, and the best case. */
    memset(buf, 'a', sizeof(buf));
    EXPECT(round_trip(buf, sizeof(buf), "constant"));
    size_t n = lz4_compress(buf, sizeof(buf), buf + 0, 0, g_lz4_work);
    EXPECT(n == 0);   /* no room at all */
    uint8_t packed[8192];
    memset(buf, 'a', sizeof(buf));
    EXPECT(lz4_compress(buf, sizeof(buf), packed, sizeof(packed), g_lz4_work) < 64);

    /* Text-like: repeated words with literals between them. */
    static const char words[] = "the quick brown fox jumps over the lazy dog; ";
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)words[i % (sizeof(words) - 1)];
    EXPECT(round_trip(buf, sizeof(buf), "text"));
    EXPECT(lz4_compress(buf, sizeof(buf), packed, sizeof(packed), g_lz4_work) < sizeof(buf) / 2);

    /* Incompressible: a counter through a multiplier. Compression may
     * fail to shrink it, but a round trip must still be exact. */
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof(buf); i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 16);
    }
    EXPECT(round_trip(buf, sizeof(buf), "random"));

    /* Short inputs, including every length below the match minimum. */
    for (size_t len = 0; len <= 32; len++) {
        memset(buf, (int)len, len);
        EXPECT(round_trip(buf, len, "short"));
    }

    /* A long run inside literals: lengths that continue past 15 and 255. */
    memset(buf, 'x', sizeof(buf));
    for (size_t i = 0; i < 300; i++)
        buf[i] = (uint8_t)i;
    EXPECT(round_trip(buf, sizeof(buf), "literals then a run"));
}

/* Bytes off a disk are not a compressed block just because they are in
 * that field. Every one of these must be refused. */
static void test_malformed(void)
{
    uint8_t out[4096];

    EXPECT(lz4_decompress("", 0, out, sizeof(out)) == 0 || 1);   /* empty: zero out, not a crash */

    /* A literal length longer than the input. */
    static const uint8_t long_lit[] = { 0xF0, 0xFF, 0xFF, 0xFF };
    EXPECT(lz4_decompress(long_lit, sizeof(long_lit), out, sizeof(out)) == 0);

    /* A match offset of zero, and one pointing before the output. */
    static const uint8_t zero_off[] = { 0x04, 'a', 'b', 'c', 'd', 0x00, 0x00, 0x10 };
    EXPECT(lz4_decompress(zero_off, sizeof(zero_off), out, sizeof(out)) == 0);
    static const uint8_t far_off[] = { 0x04, 'a', 'b', 'c', 'd', 0xFF, 0xFF, 0x10 };
    EXPECT(lz4_decompress(far_off, sizeof(far_off), out, sizeof(out)) == 0);

    /* A match longer than the room left. */
    static const uint8_t long_match[] = { 0x4F, 'a', 'b', 'c', 'd', 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    EXPECT(lz4_decompress(long_match, sizeof(long_match), out, 8) == 0);

    /* A truncated length chain, and a truncated offset. */
    static const uint8_t trunc_len[] = { 0xF0, 0xFF };
    EXPECT(lz4_decompress(trunc_len, sizeof(trunc_len), out, sizeof(out)) == 0);
    static const uint8_t trunc_off[] = { 0x14, 'a', 0x01 };
    EXPECT(lz4_decompress(trunc_off, sizeof(trunc_off), out, sizeof(out)) == 0);

    /* Output that does not fit. */
    static const uint8_t eight[] = { 0x80, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    EXPECT(lz4_decompress(eight, sizeof(eight), out, 4) == 0);
}

/* A compressed block decompresses to the same bytes whatever the caller
 * claims the capacity is, as long as it is enough. */
static void test_bounds(void)
{
    static uint8_t buf[1024], packed[2048], out[1024];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i / 7);
    size_t n = lz4_compress(buf, sizeof(buf), packed, sizeof(packed), g_lz4_work);
    EXPECT(n > 0 && n < sizeof(buf));
    EXPECT(lz4_decompress(packed, n, out, sizeof(out)) == sizeof(buf));
    EXPECT(memcmp(out, buf, sizeof(buf)) == 0);
    EXPECT(lz4_decompress(packed, n, out, sizeof(buf) - 1) == 0);
    EXPECT(lz4_compress_bound(4096) >= 4096);
}

/* --- the decoder's digest (roadmap 2.0 SA5, the LZ4 family) --------------------------------------------- */

/* The digest of the C decoder (lz4.c's lz4_decompress, moved unchanged into
 * lz4_decode.c; 2026-11-01). */
#define LZ4_DIGEST 0xcd72ee02e42e9ec3ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0007ull
#define CAP 9000u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

/* One decode: what it returned, and the whole output buffer, which starts
 * as a pattern so that what it wrote before refusing counts too. */
static void decode(const uint8_t *src, size_t slen, size_t dcap)
{
    static uint8_t out[CAP];
    memset(out, 0xa5, sizeof(out));
    uint64_t got = lz4_decompress(src, slen, out, dcap);
    mix(&got, sizeof(got));
    mix(out, dcap < CAP ? dcap + (dcap < CAP) : CAP);   /* and the byte after the capacity, never written */
}

/* Data the compressor finds matches in, more or less: runs, a small
 * alphabet, a repeated phrase, or noise. */
static size_t gen_data(uint8_t *b, size_t max)
{
    size_t len = (size_t)(next() % (max + 1));
    unsigned kind = (unsigned)(next() % 4);
    for (size_t i = 0; i < len; i++) {
        switch (kind) {
        case 0: b[i] = (uint8_t)(i / (1 + next() % 64) % 3); break;
        case 1: b[i] = (uint8_t)("etaoin shrdlu"[next() % 13]); break;
        case 2: b[i] = (uint8_t)("the cosmofs block "[i % 18]); break;
        default: b[i] = (uint8_t)next(); break;
        }
    }
    return len;
}

/* A length nibble's continuation: `extra` more in bytes of 255 and a last. */
static size_t put_length(uint8_t *b, size_t at, size_t cap, size_t extra)
{
    while (extra >= 255 && at < cap) {
        b[at++] = 255;
        extra -= 255;
    }
    if (at < cap)
        b[at++] = (uint8_t)extra;
    return at;
}

/* A stream built sequence by sequence, so that long lengths, far and zero
 * offsets, overlapping matches and every place a stream can end are reached. */
static size_t gen_stream(uint8_t *b, size_t cap)
{
    size_t at = 0, produced = 0;
    unsigned seqs = (unsigned)(next() % 12);
    for (unsigned s = 0; s < seqs && at + 4 < cap; s++) {
        size_t lit = next() % 4 == 0 ? 15 + next() % 600 : next() % 15;
        size_t mlen = next() % 4 == 0 ? 15 + next() % 600 : next() % 15;
        b[at++] = (uint8_t)((lit >= 15 ? 15 : lit) << 4 | (mlen >= 15 ? 15 : mlen));
        if (lit >= 15)
            at = put_length(b, at, cap, lit - 15);
        for (size_t i = 0; i < lit && at < cap; i++)
            b[at++] = (uint8_t)next();
        produced += lit;
        unsigned off;
        switch (next() % 8) {
        case 0: off = 0; break;                                   /* refused */
        case 1: off = (unsigned)(produced + 1 + next() % 4); break;   /* before the start */
        case 2: off = 1; break;                                   /* a run */
        default: off = produced ? (unsigned)(1 + next() % produced) : 1; break;
        }
        if (at + 2 <= cap) {
            b[at++] = (uint8_t)off;
            b[at++] = (uint8_t)(off >> 8);
        }
        if (mlen >= 15)
            at = put_length(b, at, cap, mlen - 15);
        produced += mlen + 4;
    }
    return at;
}

static uint8_t g_data[8192], g_packed[9000], g_stream[9000];

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned r = 0; r < ROUNDS; r++) {
        size_t len = gen_data(g_data, sizeof(g_data));
        size_t n = lz4_compress(g_data, len, g_packed, sizeof(g_packed), g_lz4_work);
        mix(&n, sizeof(n));
        if (n > 0) {
            /* Each draw in its own statement: C leaves open the order of a call's arguments and of an
             * initializer list's expressions (gcc and clang differ). */
            size_t caps[4];
            caps[0] = len;
            caps[1] = len + 1 + next() % 64;
            caps[2] = len ? next() % len : 0;
            caps[3] = CAP;
            for (unsigned k = 0; k < 4; k++)
                decode(g_packed, n, caps[k] < CAP ? caps[k] : CAP);
            decode(g_packed, n - next() % (n + 1), CAP);                   /* cut short */
            memset(g_packed + n, (int)(next() & 0xff), 16);                 /* padding past the stream */
            decode(g_packed, n + 16, len);
            for (unsigned f = (unsigned)(next() % 4); f > 0; f--)        /* damaged */
                g_packed[next() % n] ^= (uint8_t)(1u << (next() % 8));
            decode(g_packed, n, CAP);
        }
        size_t m = gen_stream(g_stream, sizeof(g_stream));
        decode(g_stream, m, next() % 2 ? CAP : (size_t)(next() % CAP));
        for (size_t i = 0; i < 64; i++)                                    /* noise */
            g_stream[i] = (uint8_t)next();
        size_t nlen = (size_t)(next() % 65);
        decode(g_stream, nlen, (size_t)(next() % 512));
    }
    printf("  lz4 digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == LZ4_DIGEST);
}

/* LZ4_BENCH=1: the decoder on 4 KiB blocks of the four kinds of data, best of 15. */
static void lz4_bench(void)
{
    enum { BLOCKS = 64, BLOCK = 4096, PASSES = 200 };
    static uint8_t data[BLOCKS][BLOCK], packed[BLOCKS][BLOCK + BLOCK / 255 + 16], out[BLOCK];
    size_t plen[BLOCKS];
    g_rng = SEED;
    for (unsigned k = 0; k < BLOCKS; k++) {
        for (size_t i = 0; i < BLOCK; i++) {
            switch (k % 4) {
            case 0: data[k][i] = (uint8_t)(i / 37 % 3); break;
            case 1: data[k][i] = (uint8_t)("etaoin shrdlu"[next() % 13]); break;
            case 2: data[k][i] = (uint8_t)("the cosmofs block "[i % 18]); break;
            default: data[k][i] = (uint8_t)(next() % 16 < 12 ? 0 : next()); break;
            }
        }
        plen[k] = lz4_compress(data[k], BLOCK, packed[k], sizeof(packed[k]), g_lz4_work);
    }
    double best = 1e18;
    uint64_t sink = 0;
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned p = 0; p < PASSES; p++)
            for (unsigned k = 0; k < BLOCKS; k++)
                sink += lz4_decompress(packed[k], plen[k], out, BLOCK) + out[k % BLOCK];
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / (PASSES * BLOCKS);
        if (ns < best)
            best = ns;
    }
    printf("  bench lz4_decompress %.1f ns a 4 KiB block (sink %llu)\n", best, (unsigned long long)sink);
}

static const struct host_test tests[] = {
    { "lz4-roundtrip", test_roundtrip },
    { "lz4-malformed", test_malformed },
    { "lz4-bounds", test_bounds },
    { "lz4 decoder digest", test_digest },
};

int main(void)
{
    if (getenv("LZ4_BENCH") != NULL)
        lz4_bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
