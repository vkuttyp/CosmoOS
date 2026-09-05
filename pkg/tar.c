/* tar.c - A ustar reader over an in-memory buffer (docs/pkg/design.md). */

#include <stdio.h>
#include <string.h>

#include "pkg.h"

#define BLOCK 512u

void tar_open(struct tar_reader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

static bool octal_field(const uint8_t *f, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && f[i] == ' ')
        i++;
    if (i == n || f[i] < '0' || f[i] > '7')
        return false;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (UINT64_MAX >> 3))
            return false;
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != '\0' && f[i] != ' ')
            return false;
    *out = v;
    return true;
}

int tar_next(struct tar_reader *r, struct tar_member *m, char *err, size_t errlen)
{
    if (r->pos == r->len)
        return 0;   /* no end-of-archive blocks: the buffer simply ends */
    if (r->pos + BLOCK > r->len) {
        snprintf(err, errlen, "archive truncated at offset %zu", r->pos);
        return -1;
    }
    const uint8_t *h = r->buf + r->pos;
    bool zero = true;
    for (size_t i = 0; i < BLOCK && zero; i++)
        zero = h[i] == 0;
    if (zero)
        return 0;   /* end of archive */
    if (memcmp(h + 257, "ustar", 5) != 0) {
        snprintf(err, errlen, "member at offset %zu is not ustar", r->pos);
        return -1;
    }
    /* Header checksum: the sum of all bytes with the checksum field as spaces. */
    uint64_t want;
    if (!octal_field(h + 148, 8, &want)) {
        snprintf(err, errlen, "member at offset %zu: bad checksum field", r->pos);
        return -1;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != want) {
        snprintf(err, errlen, "member at offset %zu: header checksum mismatch", r->pos);
        return -1;
    }
    if (h[156] != '0' && h[156] != '\0') {
        snprintf(err, errlen, "member at offset %zu: not a regular file", r->pos);
        return -1;
    }
    uint64_t mode, size;
    if (!octal_field(h + 100, 8, &mode) || !octal_field(h + 124, 12, &size)) {
        snprintf(err, errlen, "member at offset %zu: bad mode or size", r->pos);
        return -1;
    }
    size_t nlen = 0;
    while (nlen < 100 && h[nlen])
        nlen++;
    if (nlen == 0 || h[345] != '\0') {   /* no prefix field: names are 100 bytes at most */
        snprintf(err, errlen, "member at offset %zu: bad name", r->pos);
        return -1;
    }
    memcpy(r->name, h, nlen);
    r->name[nlen] = '\0';
    size_t data = r->pos + BLOCK;
    size_t padded = (size_t)((size + BLOCK - 1) / BLOCK * BLOCK);
    if (size > r->len || data + padded > r->len) {
        snprintf(err, errlen, "member '%s' runs past the archive", r->name);
        return -1;
    }
    m->name = r->name;
    m->mode = (unsigned)(mode & 07777);
    m->size = size;
    m->data = r->buf + data;
    r->pos = data + padded;
    return 1;
}
