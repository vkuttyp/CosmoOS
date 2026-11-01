/*
 * pkgparse.h - The package manager's decoders of what a repository, a
 * package or an installed record says (roadmap 2.0 SA5, the pkg family):
 * the ustar reader, versions and dependencies, names, paths, sonames, hex,
 * times, manifests and the INDEX, in CeeSafe (pkg/pkgparse.cee; the C
 * is generated, build/cee.mk). They read only the bytes they are given
 * and report why they refuse, with the line and the key; manifest.c and
 * tar.c allocate, word the messages and keep the API of pkg.h.
 * tests/host/test_pkg.c holds them to a recorded digest; fuzz_pkg drives
 * them. name_valid, version_parse, version_cmp, depend_parse,
 * depend_satisfied, hex_decode, path_allowed, soname_valid and time_parse,
 * declared in pkg.h, are decoders too.
 */

#ifndef PKG_PKGPARSE_H
#define PKG_PKGPARSE_H

#include "pkg.h"

/* Why a manifest or an index is refused. */
enum {
    PKG_PE_OK = 0,
    PKG_PE_LINE,             /* line `line` is not "key: value" */
    PKG_PE_ENTRY,            /* line `line`: a bad `key` entry */
    PKG_PE_NAME_VERSION,     /* a manifest without a name or a version */
    PKG_PE_HEADER_KEY,       /* line `line`: `key` in the index's header */
    PKG_PE_HEADER_BAD,       /* line `line`: a bad `key` header */
    PKG_PE_LACKS_SERIAL,     /* the index's header lacks serial, */
    PKG_PE_LACKS_PUBLISHED,  /* published, */
    PKG_PE_LACKS_EXPIRES,    /* or expires */
    PKG_PE_TOO_MANY,         /* more than PKG_MAX_INDEX entries */
    PKG_PE_INCOMPLETE,       /* entry `line` (from 1) lacks name, version, file or size */
};

struct pkg_perr {
    int why;                 /* PKG_PE_* */
    int line;
    size_t key_at, key_len;  /* the key: text[key_at .. key_at + key_len), up to its first NUL */
};

/* A manifest into `m`, which is zeroed but for `files` (PKG_MAX_FILES
 * entries) and `dirs`, both zeroed. The arrays of arrays come beside it
 * as their first rows, since CeeSafe imports no member of that shape
 * (type-system §31.1): m->provides[0], m->requires[0] and
 * m->dirs->paths[0]. 0, or -1 with `pe`. */
int manifest_decode(const char *text, size_t len, struct manifest *m, char *provides, char *requires, char *dirs,
                    struct pkg_perr *pe);
/* The index's header stanza into `h`. 0, or -1 with `pe`. */
int index_header_decode(const char *text, size_t len, struct index_header *h, struct pkg_perr *pe);
/* An index into `ix`, which is zeroed but for `entries` (PKG_MAX_INDEX
 * of them, zeroed). 0, or -1 with `pe`. */
int index_decode(const char *text, size_t len, struct index *ix, struct pkg_perr *pe);

/* What tar_decode found at the reader's position. */
enum {
    TAR_DEC_END = 0,         /* the buffer ends, or a zero block */
    TAR_DEC_MEMBER,          /* a member: `d` and the reader's name */
    TAR_DEC_TRUNCATED,       /* less than a block left */
    TAR_DEC_NOT_USTAR,
    TAR_DEC_CHECKSUM_FIELD,  /* the checksum field is not octal */
    TAR_DEC_CHECKSUM,        /* the header's checksum does not match */
    TAR_DEC_NOT_REGULAR,
    TAR_DEC_MODE_SIZE,       /* the mode or the size is not octal */
    TAR_DEC_NAME,            /* empty, or a prefix field */
    TAR_DEC_PAST_END,        /* the data runs past the buffer (the name is set) */
};

struct tar_decoded {
    unsigned mode;           /* the permission bits */
    uint64_t size;
    size_t data;             /* the data's offset in the buffer */
    size_t next;             /* the next header's */
};

/* The member at r->pos: its name into r->name, the rest into `d`. The
 * reader's position is the caller's to advance. */
int tar_decode(struct tar_reader *r, struct tar_decoded *d);

#endif /* PKG_PKGPARSE_H */
