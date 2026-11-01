/*
 * lz4_decode.c - The LZ4 block decoder (kernel/include/kernel/lz4.h): roadmap
 * 2.0 SA5's last family, a parser of bytes that came off a disk (cosmofs's
 * compressed blocks). The format is described in lz4.c, the compressor.
 * tests/host/test_lz4.c holds it to a recorded digest; fuzz_lz4 drives it.
 */

#include <kernel/lz4.h>
#include <kernel/string.h>

#define LZ4_MIN_MATCH 4u

size_t lz4_decompress(const void *src, size_t slen, void *dst, size_t dcap)
{
    const uint8_t *ip = src, *iend = ip + slen;
    uint8_t *op = dst, *oend = (uint8_t *)dst + dcap;

    while (op < oend && ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend)
                    return 0;
                b = *ip++;
                if (lit + b < lit)
                    return 0;   /* a length that wraps is not a length */
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op))
            return 0;
        memcpy(op, ip, lit);
        ip += lit;
        op += lit;
        if (op == oend)
            return (size_t)(op - (uint8_t *)dst);   /* the output is complete */
        if (ip == iend)
            return (size_t)(op - (uint8_t *)dst);   /* the last sequence has no match */
        if ((size_t)(iend - ip) < 2)
            return 0;

        unsigned off = (unsigned)ip[0] | ((unsigned)ip[1] << 8);
        ip += 2;
        if (off == 0 || off > (size_t)(op - (uint8_t *)dst))
            return 0;   /* a match before the start of the output */
        size_t mlen = token & 0x0F;
        if (mlen == 15) {
            unsigned b;
            do {
                if (ip >= iend)
                    return 0;
                b = *ip++;
                if (mlen + b < mlen)
                    return 0;
                mlen += b;
            } while (b == 255);
        }
        mlen += LZ4_MIN_MATCH;
        if (mlen > (size_t)(oend - op))
            return 0;
        /* Byte by byte, and deliberately: a match may overlap the
         * position being written, which is how the format codes a run. */
        const uint8_t *m = op - off;
        for (size_t i = 0; i < mlen; i++)
            op[i] = m[i];
        op += mlen;
    }
    return (size_t)(op - (uint8_t *)dst);
}
