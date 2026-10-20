/*
 * pkg.h - Internal declarations of the package manager (docs/pkg/design.md).
 *
 * manifest.c, version.c and tar.c depend only on standard headers so the
 * host test (tests/host/test_pkg.c) can compile them unchanged.
 */

#ifndef PKG_H
#define PKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PKG_NAME_MAX     64
#define PKG_VERSION_MAX  32
#define PKG_SUMMARY_MAX  128
#define PKG_PATH_MAX     256
#define PKG_FILE_MAX     128    /* a package file name in the index */
#define PKG_MAX_DEPENDS  64
/* Shared libraries (roadmap 1.1 P4): a manifest's "provides: SONAME" (the
 * libraries it installs, by DT_SONAME) and "requires: SONAME" (what its
 * programs and libraries name in DT_NEEDED and it does not provide). */
#define PKG_MAX_LIBS     32
#define PKG_SONAME_MAX   64
#define PKG_BASE_PROVIDES "/etc/pkg/provides"   /* the base system's libraries, one soname a line */
#define PKG_MAX_FILES    4096
#define PKG_MAX_INDEX    1024
#define PKG_MAX_DIRS     256
#define PKG_ERR_MAX      256
#define PKG_PACKAGE_LIMIT (64u << 20)
#define PKG_INDEX_LIMIT   (4u << 20)
#define SHA512_LEN       64

#define PKG_DB_DIR       "/var/db/pkg"
#define PKG_DB_INSTALLED PKG_DB_DIR "/installed"
#define PKG_DB_LOCK      PKG_DB_DIR "/lock"
#define PKG_REPOS_CONF   "/etc/pkg/repos.conf"
#define PKG_KEYS_DIR     "/etc/pkg/keys"
#define PKG_REPOS_DIR    "/var/lib/pkg/repos"   /* <name>/INDEX: each repository's last accepted index */
#define PKG_CACHE_DIR    "/var/cache/pkg"       /* <name>/<file>: a package being fetched */
#define PKG_KEPT_DIR     PKG_CACHE_DIR "/kept"  /* <name>/: the installed version and the one before (pkg rollback) */
#define PKG_TX_PREFIX    "pkg-tx-"             /* a transaction's root snapshot (init undoes a leftover one) */
#define PKG_OLDEST_FORMAT "/var/lib/cosmo-upgrade/oldest-format"   /* cosmo-upgrade's record (B4) */
#define PKG_TX_ACTIVE    PKG_DB_DIR "/tx-active"   /* the open transaction's snapshot, for init */
#define PKG_MAX_REPOS    8
#define PKG_URL_MAX      256
#define PKG_INDEX_MAX_DAYS 90                    /* PK10: an index is valid for at most 90 days */
/* A network repository's index: INDEX.90, signed for 90 days, and beside it
 * INDEX, signed for 30, which pkg before v1.0.7 reads (it refused a longer
 * validity). pkg reads INDEX when the server has no INDEX.90. */
#define PKG_NET_INDEX     "INDEX.90"
#define PKG_NET_INDEX_OLD "INDEX"

/* Exit statuses: 0 ok, 1 failure, 2 usage, 3 refused (verification). */
#define EXIT_FAILED  1
#define EXIT_USAGE   2
#define EXIT_REFUSED 3

/* --- version.c --- */

struct version {
    unsigned comp[8];
    int ncomp;
    unsigned rev;
};

enum cmp_op { OP_NONE, OP_EQ, OP_GE, OP_LE, OP_LT, OP_GT };

struct depend {
    char name[PKG_NAME_MAX];
    enum cmp_op op;
    char version[PKG_VERSION_MAX];
};

bool version_parse(const char *s, struct version *v);
int version_cmp(const char *a, const char *b);               /* both must parse; -1 0 1 */
bool depend_parse(const char *text, struct depend *d);       /* "name", "name >= 1.0" */
bool depend_satisfied(const struct depend *d, const char *version);
const char *op_text(enum cmp_op op);
bool name_valid(const char *s);

/* --- manifest.c --- */

struct file_entry {
    char path[PKG_PATH_MAX];      /* relative to /, no leading slash */
    unsigned mode;
    uint64_t size;
    uint8_t sha[SHA512_LEN];
};

struct dirlist {
    char paths[PKG_MAX_DIRS][PKG_PATH_MAX];
    int n;
};

struct manifest {
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char summary[PKG_SUMMARY_MAX];
    struct depend depends[PKG_MAX_DEPENDS];
    int ndepends;
    char provides[PKG_MAX_LIBS][PKG_SONAME_MAX];
    int nprovides;
    char requires[PKG_MAX_LIBS][PKG_SONAME_MAX];
    int nrequires;
    struct file_entry *files;     /* malloc'd, nfiles entries */
    int nfiles;
    struct dirlist *dirs;         /* malloc'd when a record carries "dir:" lines (installed records only) */
};

struct index_entry {
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char summary[PKG_SUMMARY_MAX];
    char file[PKG_FILE_MAX];
    struct depend depends[PKG_MAX_DEPENDS];
    int ndepends;
    uint64_t size;
    uint8_t sha[SHA512_LEN];
    int repo;                     /* which repository (pkg.c; index_parse leaves 0) */
};

/* An index's header stanza (PK10): "serial: N", "published: T" and
 * "expires: T", T as YYYY-MM-DDTHH:MM:SSZ. A repository's own disk
 * (file://) may omit it; a network repository's index must carry it. */
struct index_header {
    bool present;
    uint64_t serial;
    int64_t published, expires;   /* seconds since 1970, UTC */
};

struct index {
    struct index_entry *entries;  /* malloc'd */
    int n;
    struct index_header header;
};

/* One /etc/pkg/repos.conf line: "NAME URL [pin sha256:HEX] [insecure]",
 * URL file:///DIR, https://... or (with insecure) http://...; or, the
 * form before P6, a directory alone, named "local" (then local2...). */
struct repo {
    char name[PKG_NAME_MAX];
    char url[PKG_URL_MAX];        /* without a trailing '/' */
    char pin[72];                 /* "sha256:HEX" or empty */
    bool insecure;
    bool local;                   /* file:// */
    char dir[PKG_PATH_MAX];       /* the directory, when local */
};

int manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen);
void manifest_free(struct manifest *m);
bool soname_valid(const char *s);                              /* a "provides"/"requires" value */
/* Render a manifest (plus "dir:" lines for `dirs`, may be NULL) as text; returns the length or -1 (too small). */
int manifest_format(const struct manifest *m, const struct dirlist *dirs, char *out, size_t cap);
int index_parse(const char *text, size_t len, struct index *ix, char *err, size_t errlen);
/* The header stanza alone, read before any package line (PK10). */
int index_header_parse(const char *text, size_t len, struct index_header *h, char *err, size_t errlen);
/* PK10, for a network repository: a header, a validity of at most
 * PKG_INDEX_MAX_DAYS, `now` within it, and a serial no lower than
 * `last_serial`, the last one accepted from the repository (0: none). 0, or
 * -1 with `err`; -2 when the index has expired (`err` names the time). */
int index_fresh(const struct index_header *h, uint64_t last_serial, int64_t now, char *err, size_t errlen);
void index_free(struct index *ix);
bool time_parse(const char *s, int64_t *t);                   /* YYYY-MM-DDTHH:MM:SSZ */
void time_format(int64_t t, char out[21]);
/* Parse a repos.conf line (comments and blank lines are the caller's);
 * `nlocal` counts the directory-alone lines before it. 0, or -1 with `err`. */
int repo_parse(const char *line, int nlocal, struct repo *r, char *err, size_t errlen);
bool path_allowed(const char *path);                          /* relative, no "..", not under boot/dev/tmp/mnt */
bool hex_decode(const char *hex, uint8_t *out, size_t n);
void hex_encode(const uint8_t *in, size_t n, char *out);      /* out has 2n+1 bytes */

/* --- tar.c --- */

struct tar_member {
    const char *name;             /* NUL-terminated copy inside the reader */
    unsigned mode;
    uint64_t size;
    const uint8_t *data;
};

struct tar_reader {
    const uint8_t *buf;
    size_t len, pos;
    char name[101];
};

void tar_open(struct tar_reader *r, const uint8_t *buf, size_t len);
int tar_next(struct tar_reader *r, struct tar_member *m, char *err, size_t errlen);   /* 1 member, 0 end, -1 bad */

#endif /* PKG_H */
