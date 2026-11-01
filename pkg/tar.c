/* tar.c - A ustar reader over an in-memory buffer (docs/pkg/design.md): the
 * messages around tar_decode (pkgparse.h), which reads each header. */

#include <stdio.h>

#include "pkgparse.h"

void tar_open(struct tar_reader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

int tar_next(struct tar_reader *r, struct tar_member *m, char *err, size_t errlen)
{
    struct tar_decoded d;
    switch (tar_decode(r, &d)) {
    case TAR_DEC_END:
        return 0;
    case TAR_DEC_MEMBER:
        m->name = r->name;
        m->mode = d.mode;
        m->size = d.size;
        m->data = r->buf + d.data;
        r->pos = d.next;
        return 1;
    case TAR_DEC_TRUNCATED:
        snprintf(err, errlen, "archive truncated at offset %zu", r->pos);
        return -1;
    case TAR_DEC_NOT_USTAR:
        snprintf(err, errlen, "member at offset %zu is not ustar", r->pos);
        return -1;
    case TAR_DEC_CHECKSUM_FIELD:
        snprintf(err, errlen, "member at offset %zu: bad checksum field", r->pos);
        return -1;
    case TAR_DEC_CHECKSUM:
        snprintf(err, errlen, "member at offset %zu: header checksum mismatch", r->pos);
        return -1;
    case TAR_DEC_NOT_REGULAR:
        snprintf(err, errlen, "member at offset %zu: not a regular file", r->pos);
        return -1;
    case TAR_DEC_MODE_SIZE:
        snprintf(err, errlen, "member at offset %zu: bad mode or size", r->pos);
        return -1;
    case TAR_DEC_NAME:
        snprintf(err, errlen, "member at offset %zu: bad name", r->pos);
        return -1;
    case TAR_DEC_PAST_END:
        snprintf(err, errlen, "member '%s' runs past the archive", r->name);
        return -1;
    default:
        snprintf(err, errlen, "member at offset %zu: refused", r->pos);
        return -1;
    }
}
