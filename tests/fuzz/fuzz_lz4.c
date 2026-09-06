/*
 * fuzz_lz4.c - The LZ4 decompressor against bytes that are not a
 * compressed block (docs/verification/design.md).
 *
 * This is the one codec in the tree that parses attacker-controlled
 * bytes off a disk: a compressed record is whatever is in those blocks.
 * The decompressor must survive every input and never write outside the
 * buffer it was given, which is what ASan checks here. It must also be
 * honest in the other direction: anything the compressor produces must
 * decompress back to exactly what went in.
 */

#include "fuzz.h"

#include <kernel/lz4.h>

#include <stdlib.h>
#include <string.h>

#define OUT_MAX 65536u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    /* The first two bytes pick the output capacity, so the decompressor
     * is exercised against buffers both larger and much smaller than the
     * input claims to need. */
    size_t cap = (((size_t)data[0] << 8) | data[1]) % OUT_MAX + 1;
    const uint8_t *body = data + 2;
    size_t blen = size - 2;

    uint8_t *out = malloc(cap);
    FUZZ_ASSERT(out != NULL);
    size_t n = lz4_decompress(body, blen, out, cap);
    FUZZ_ASSERT(n <= cap);
    free(out);

    /* Round trip: whatever these bytes are, compressing them and
     * decompressing the result must give them back. */
    size_t bound = lz4_compress_bound(blen);
    uint8_t *packed = malloc(bound ? bound : 1);
    uint8_t *back = malloc(blen ? blen : 1);
    FUZZ_ASSERT(packed != NULL && back != NULL);
    size_t clen = lz4_compress(body, blen, packed, bound);
    if (clen > 0) {
        FUZZ_ASSERT(clen <= bound);
        size_t m = lz4_decompress(packed, clen, back, blen);
        FUZZ_ASSERT(m == blen);
        FUZZ_ASSERT(blen == 0 || memcmp(back, body, blen) == 0);
    }
    free(packed);
    free(back);
    return 0;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (cap < 64)
        return 0;
    switch (i) {
    case 0: {   /* a literal-only block: the shape of incompressible data */
        buf[0] = 0x00;
        buf[1] = 0x40;
        buf[2] = 0x50;
        memcpy(buf + 3, "hello", 5);
        return 8;
    }
    case 1: {   /* a match: four literals then a run copied from them */
        buf[0] = 0x00;
        buf[1] = 0x40;
        buf[2] = 0x4F;
        memcpy(buf + 3, "abcd", 4);
        buf[7] = 0x01;
        buf[8] = 0x00;
        buf[9] = 0x10;
        return 10;
    }
    case 2: {   /* a length chain that continues */
        buf[0] = 0x00;
        buf[1] = 0x40;
        buf[2] = 0xF0;
        buf[3] = 0xFF;
        buf[4] = 0x02;
        memset(buf + 5, 'z', 32);
        return 37;
    }
    default:
        return 0;
    }
}
