/*
 * fdt_read.c - The reader a guest needs (tools/fdt/fdt.h).
 *
 * Find a node by its path, read a property, count children. No library:
 * the same file is compiled into the host test, where it checks what
 * the writer wrote, and into the C guest fixture, which is freestanding.
 */
#include "fdt.h"

static unsigned fr_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static int fr_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int fr_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

uint32_t fdt_be32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

uint64_t fdt_be64(const void *p)
{
    const uint8_t *b = p;
    return ((uint64_t)fdt_be32(b) << 32) | fdt_be32(b + 4);
}

uint32_t fdt_check(const void *blob, size_t cap)
{
    const uint8_t *h = blob;
    if (cap < 40 || fdt_be32(h) != FDT_MAGIC)
        return 0;
    uint32_t total = fdt_be32(h + 4);
    uint32_t soff = fdt_be32(h + 8), stroff = fdt_be32(h + 12);
    uint32_t ssize = fdt_be32(h + 36), strsize = fdt_be32(h + 32);
    if (total > cap || soff + ssize > total || stroff + strsize > total)
        return 0;
    if (fdt_be32(h + 24) > FDT_VERSION)   /* last_comp_version: newer than this reader */
        return 0;
    return total;
}

/* The segment of `path` at index `i` ("/a/b": 0 -> "a", 1 -> "b"), or
 * NULL past the end; `*len` its length. */
static const char *path_seg(const char *path, unsigned i, unsigned *len)
{
    const char *p = path;
    while (*p == '/')
        p++;
    for (unsigned k = 0; k < i; k++) {
        while (*p && *p != '/')
            p++;
        while (*p == '/')
            p++;
    }
    if (!*p)
        return 0;
    unsigned n = 0;
    while (p[n] && p[n] != '/')
        n++;
    *len = n;
    return p;
}

static int seg_eq(const char *name, const char *seg, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        if (name[i] != seg[i])
            return 0;
    return name[len] == 0;
}

/* The offset (from the blob) of the FDT_BEGIN_NODE token of the node at
 * `path`, or 0. The root is "/". */
static uint32_t find_node(const void *blob, const char *path)
{
    const uint8_t *b = blob;
    uint32_t off = fdt_be32(b + 8), end = off + fdt_be32(b + 36);
    unsigned nseg = 0, l;
    while (path_seg(path, nseg, &l))
        nseg++;
    int depth = 0;       /* the depth of the node about to begin; root is 0 */
    int matched = 0;     /* how many path segments the current ancestry matches */
    while (off + 4 <= end) {
        uint32_t tok = fdt_be32(b + off);
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + off + 4);
            if (depth == 0) {
                if (nseg == 0)
                    return off;
            } else if (depth == matched + 1) {
                unsigned sl;
                const char *seg = path_seg(path, (unsigned)matched, &sl);
                if (seg && seg_eq(name, seg, sl)) {
                    matched++;
                    if ((unsigned)matched == nseg)
                        return off;
                }
            }
            depth++;
            off += 4 + ((fr_strlen(name) + 1 + 3) & ~3u);
        } else if (tok == FDT_END_NODE) {
            depth--;
            if (matched > depth)
                matched = depth;
            off += 4;
        } else if (tok == FDT_PROP) {
            off += 12 + ((fdt_be32(b + off + 4) + 3) & ~3u);
        } else if (tok == FDT_NOP) {
            off += 4;
        } else {
            break;
        }
    }
    return 0;
}

const void *fdt_get_prop(const void *blob, const char *path, const char *name, uint32_t *len)
{
    const uint8_t *b = blob;
    uint32_t node = find_node(blob, path);
    if (node == 0)
        return 0;
    uint32_t end = fdt_be32(b + 8) + fdt_be32(b + 36);
    const char *strings = (const char *)(b + fdt_be32(b + 12));
    uint32_t off = node + 4 + ((fr_strlen((const char *)(b + node + 4)) + 1 + 3) & ~3u);
    int depth = 0;
    while (off + 4 <= end) {
        uint32_t tok = fdt_be32(b + off);
        if (tok == FDT_BEGIN_NODE) {
            depth++;
            off += 4 + ((fr_strlen((const char *)(b + off + 4)) + 1 + 3) & ~3u);
        } else if (tok == FDT_END_NODE) {
            if (depth == 0)
                return 0;
            depth--;
            off += 4;
        } else if (tok == FDT_PROP) {
            uint32_t plen = fdt_be32(b + off + 4), nameoff = fdt_be32(b + off + 8);
            if (depth == 0 && fr_streq(strings + nameoff, name)) {
                *len = plen;
                return b + off + 12;
            }
            off += 12 + ((plen + 3) & ~3u);
        } else if (tok == FDT_NOP) {
            off += 4;
        } else {
            return 0;
        }
    }
    return 0;
}

unsigned fdt_count_children(const void *blob, const char *path, const char *prefix)
{
    const uint8_t *b = blob;
    uint32_t node = find_node(blob, path);
    if (node == 0)
        return 0;
    uint32_t end = fdt_be32(b + 8) + fdt_be32(b + 36);
    uint32_t off = node + 4 + ((fr_strlen((const char *)(b + node + 4)) + 1 + 3) & ~3u);
    int depth = 0;
    unsigned n = 0;
    while (off + 4 <= end) {
        uint32_t tok = fdt_be32(b + off);
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(b + off + 4);
            if (depth == 0 && fr_starts(name, prefix))
                n++;
            depth++;
            off += 4 + ((fr_strlen(name) + 1 + 3) & ~3u);
        } else if (tok == FDT_END_NODE) {
            if (depth == 0)
                return n;
            depth--;
            off += 4;
        } else if (tok == FDT_PROP) {
            off += 12 + ((fdt_be32(b + off + 4) + 3) & ~3u);
        } else if (tok == FDT_NOP) {
            off += 4;
        } else {
            return n;
        }
    }
    return n;
}
