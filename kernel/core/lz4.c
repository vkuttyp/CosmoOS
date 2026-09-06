/*
 * lz4.c - LZ4 block compression (kernel/include/kernel/lz4.h).
 *
 * The format, so the code below can be read against it:
 *
 *   token   one byte: high nibble = literal length, low nibble = match
 *           length - 4. A nibble of 15 means "more", as a chain of bytes
 *           that continue while each is 255.
 *   literals  that many bytes, copied out as they are.
 *   offset  two bytes, little-endian, distance back into the output;
 *           1..65535, never 0.
 *   match   at least 4 bytes copied from that distance, which may
 *           overlap the current position -- that is how a run is coded,
 *           so the copy is byte by byte and must stay so.
 *
 * The last sequence has literals and no match, and the format requires
 * the last five bytes of a block to be literals, which is what lets a
 * decoder copy without checking on the fast path. This decoder checks
 * anyway: its input came off a disk.
 */

#include <kernel/lz4.h>
#include <kernel/string.h>

#define LZ4_MIN_MATCH 4u
#define LZ4_LAST_LITERALS 5u
#define LZ4_MF_LIMIT 12u          /* no match may start this near the end */
#define LZ4_HASH_BITS 12u
#define LZ4_HASH_SIZE (1u << LZ4_HASH_BITS)

static inline uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Knuth's multiplicative hash over four bytes: cheap, and good enough to
 * find the matches that matter in 4 KiB of filesystem data. */
static inline unsigned hash4(uint32_t v)
{
    return (unsigned)((v * 2654435761u) >> (32 - LZ4_HASH_BITS));
}

static uint8_t *emit_length(uint8_t *op, const uint8_t *oend, size_t len)
{
    while (len >= 255) {
        if (op >= oend)
            return NULL;
        *op++ = 255;
        len -= 255;
    }
    if (op >= oend)
        return NULL;
    *op++ = (uint8_t)len;
    return op;
}

size_t lz4_compress(const void *src, size_t slen, void *dst, size_t dcap)
{
    const uint8_t *ip = src, *anchor = ip;
    const uint8_t *iend = ip + slen;
    uint8_t *op = dst, *oend = (uint8_t *)dst + dcap;
    /* One entry per hash: the most recent position with those four
     * bytes. A chain would find longer matches; this is a filesystem's
     * inner loop, and one probe is the trade taken. */
    static const size_t table_bytes = LZ4_HASH_SIZE * sizeof(uint32_t);
    (void)table_bytes;
    uint32_t table[LZ4_HASH_SIZE];
    memset(table, 0, sizeof(table));

    if (slen < LZ4_MF_LIMIT + LZ4_MIN_MATCH)
        goto last_literals;   /* too short to hold a match at all */

    const uint8_t *mflimit = iend - LZ4_MF_LIMIT;
    ip++;   /* position 0 is never a match target: 0 means "empty" below */
    while (ip < mflimit) {
        unsigned h = hash4(load32(ip));
        uint32_t cand = table[h];
        table[h] = (uint32_t)(ip - (const uint8_t *)src);
        const uint8_t *match = (const uint8_t *)src + cand;
        if (cand == 0 || (size_t)(ip - match) > 65535 || load32(match) != load32(ip)) {
            ip++;
            continue;
        }

        /* How far the match runs, never past where literals must be. */
        const uint8_t *limit = iend - LZ4_LAST_LITERALS;
        size_t mlen = LZ4_MIN_MATCH;
        while (ip + mlen < limit && ip[mlen] == match[mlen])
            mlen++;

        size_t lit = (size_t)(ip - anchor);
        uint8_t *token = op;
        if (op >= oend)
            return 0;
        op++;
        *token = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
        if (lit >= 15) {
            op = emit_length(op, oend, lit - 15);
            if (op == NULL)
                return 0;
        }
        if ((size_t)(oend - op) < lit)
            return 0;
        memcpy(op, anchor, lit);
        op += lit;

        if ((size_t)(oend - op) < 2)
            return 0;
        unsigned off = (unsigned)(ip - match);
        *op++ = (uint8_t)(off & 0xFF);
        *op++ = (uint8_t)(off >> 8);

        size_t code = mlen - LZ4_MIN_MATCH;
        *token |= (uint8_t)(code >= 15 ? 15 : code);
        if (code >= 15) {
            op = emit_length(op, oend, code - 15);
            if (op == NULL)
                return 0;
        }
        ip += mlen;
        anchor = ip;
    }

last_literals:;
    size_t lit = (size_t)(iend - anchor);
    if (op >= oend)
        return 0;
    uint8_t *token = op++;
    *token = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) {
        op = emit_length(op, oend, lit - 15);
        if (op == NULL)
            return 0;
    }
    if ((size_t)(oend - op) < lit)
        return 0;
    memcpy(op, anchor, lit);
    op += lit;
    return (size_t)(op - (uint8_t *)dst);
}

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
