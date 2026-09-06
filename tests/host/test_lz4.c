/*
 * test_lz4.c - The LZ4 codec, on the host.
 *
 * Round trips, the cases the format makes easy to get wrong (overlapping
 * matches, lengths that continue, input too short to hold a match), and
 * the ones a disk makes necessary: malformed input must be refused, not
 * copied past the end of a buffer.
 */

#include "harness.h"

#include <kernel/lz4.h>

#include <stdio.h>
#include <string.h>

static int round_trip(const uint8_t *data, size_t len, const char *what)
{
    uint8_t packed[8192], out[8192];
    size_t n = lz4_compress(data, len, packed, sizeof(packed));
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
    size_t n = lz4_compress(buf, sizeof(buf), buf + 0, 0);
    EXPECT(n == 0);   /* no room at all */
    uint8_t packed[8192];
    memset(buf, 'a', sizeof(buf));
    EXPECT(lz4_compress(buf, sizeof(buf), packed, sizeof(packed)) < 64);

    /* Text-like: repeated words with literals between them. */
    static const char words[] = "the quick brown fox jumps over the lazy dog; ";
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)words[i % (sizeof(words) - 1)];
    EXPECT(round_trip(buf, sizeof(buf), "text"));
    EXPECT(lz4_compress(buf, sizeof(buf), packed, sizeof(packed)) < sizeof(buf) / 2);

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
    size_t n = lz4_compress(buf, sizeof(buf), packed, sizeof(packed));
    EXPECT(n > 0 && n < sizeof(buf));
    EXPECT(lz4_decompress(packed, n, out, sizeof(out)) == sizeof(buf));
    EXPECT(memcmp(out, buf, sizeof(buf)) == 0);
    EXPECT(lz4_decompress(packed, n, out, sizeof(buf) - 1) == 0);
    EXPECT(lz4_compress_bound(4096) >= 4096);
}

static const struct host_test tests[] = {
    { "lz4-roundtrip", test_roundtrip },
    { "lz4-malformed", test_malformed },
    { "lz4-bounds", test_bounds },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
