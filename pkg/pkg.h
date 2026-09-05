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
#define PKG_MAX_FILES    4096
#define PKG_MAX_INDEX    1024
#define PKG_MAX_DIRS     256
#define PKG_ERR_MAX      256
#define PKG_PACKAGE_LIMIT (64u << 20)
#define PKG_INDEX_LIMIT   (4u << 20)
#define SHA512_LEN       64

#define PKG_DB_DIR       "/var/db/pkg"
#define PKG_DB_INSTALLED PKG_DB_DIR "/installed"
#define PKG_DB_INDEX     PKG_DB_DIR "/index"
#define PKG_DB_LOCK      PKG_DB_DIR "/lock"
#define PKG_REPOS_CONF   "/etc/pkg/repos.conf"
#define PKG_KEYS_DIR     "/etc/pkg/keys"

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
};

struct index {
    struct index_entry *entries;  /* malloc'd */
    int n;
};

int manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen);
void manifest_free(struct manifest *m);
/* Render a manifest (plus "dir:" lines for `dirs`, may be NULL) as text; returns the length or -1 (too small). */
int manifest_format(const struct manifest *m, const struct dirlist *dirs, char *out, size_t cap);
int index_parse(const char *text, size_t len, struct index *ix, char *err, size_t errlen);
void index_free(struct index *ix);
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
