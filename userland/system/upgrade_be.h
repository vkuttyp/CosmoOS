/*
 * upgrade_be.h -- the boot-environment pairing file, \cosmo\be (roadmap
 * 1.2 B4; docs/userland/design.md, "Boot environments"). Text, read and
 * written whole by cosmo-upgrade; the loader never reads it:
 *
 *   cosmo-be 1
 *   intent <set>
 *   env <name> <uuid> <set>
 *   ...
 *
 * <set> is the SHA-512 of a boot set's manifest (128 hex digits); <uuid>
 * a root snapshot's uuid (32 hex digits); <name> that snapshot's name. An
 * environment pairs the snapshot with the set it ran on; `intent` names
 * the set the root and the ESP are meant to run together now. Nothing in
 * it is trusted beyond that: each snapshot it names is looked up by name
 * and its uuid compared, and each set is read and verified from the ESP.
 */
#ifndef UPGRADE_BE_H
#define UPGRADE_BE_H

#include <stdbool.h>
#include <stddef.h>

#define BE_MAX 8          /* environments a file holds */
#define BE_KEEP 2         /* the newest kept by an upgrade and by --prune */
#define BE_NAME_MAX 31    /* a snapshot's name (COSMO_FSCTL_NAME_MAX) */
#define BE_SET_HEX 128
#define BE_UUID_HEX 32
#define BE_MAGIC "cosmo-be 1\n"   /* the file's first line */

_Static_assert(BE_KEEP < BE_MAX, "an upgrade adds one environment to BE_KEEP and drops the rest");

struct be_env {
    char name[BE_NAME_MAX + 1];
    char uuid[BE_UUID_HEX + 1];
    char set[BE_SET_HEX + 1];
};

struct be_file {
    char intent[BE_SET_HEX + 1];   /* empty: none */
    struct be_env env[BE_MAX];
    int n;                         /* oldest first */
};

/* 0, or -1 with `err` for a file that is not this format. */
int be_parse(const char *text, size_t len, struct be_file *b, char *err, size_t errlen);
/* The file's text; the length, or 0 when `cap` is too small. */
size_t be_format(const struct be_file *b, char *out, size_t cap);
/* Appends an environment, the oldest dropped when BE_MAX are held. */
void be_add(struct be_file *b, const char *name, const char *uuid, const char *set);
/* The newest environment for `set`, or NULL. */
const struct be_env *be_for_set(const struct be_file *b, const char *set);
/* Removes environment `i`. */
void be_drop(struct be_file *b, int i);
/* A name for a new environment from a release tag: "be-" and the tag,
 * characters a snapshot name cannot hold replaced, cut to fit; `taken`
 * says whether a name is in use, and a suffix -2, -3 ... is added until
 * one is not. */
void be_name(const char *tag, bool (*taken)(const char *name, void *arg), void *arg, char out[BE_NAME_MAX + 1]);
/* Whether a cosmo-upgrade binary reads this file: it holds BE_MAGIC, which
 * be_format writes. One that does not (v1.0.7 and older) refuses an ESP
 * whose \cosmo has a `be` entry. */
bool be_aware(const unsigned char *bin, size_t len);

/*
 * The root's record, /var/lib/cosmo-upgrade/root-set: which set's base
 * system the root holds. It is not trusted: cosmo-upgrade checks it
 * against the root itself (docs/userland/design.md, "Boot environments").
 *
 *   <set>
 *   cosmo-root-set 2
 *   release <the set's kernel build id, or ->
 *   from <set, or ->
 *
 * The first line alone is version 1, what v1.0.8 writes and reads (it
 * reads the first 128 bytes). `from` is the set the root was brought
 * from when the record was written, by that set's run: a record naming
 * another set than the current one, written on the way from the current
 * one, was left behind when an older cosmo-upgrade (v1.0.7 or before,
 * which keeps no record) brought the root back.
 */
#define ROOTSET_MAGIC "cosmo-root-set 2\n"
#define ROOTSET_RELEASE_MAX 63

struct root_set {
    int version;                            /* 0: none, 1: the set alone, 2 */
    char set[BE_SET_HEX + 1];
    char release[ROOTSET_RELEASE_MAX + 1];  /* empty: not recorded */
    char from[BE_SET_HEX + 1];              /* empty: not recorded, or none */
};

/* 0, or -1 for text that is not a record (r->version 0). */
int rootset_parse(const char *text, size_t len, struct root_set *r);
/* The record's text (version 2); the length, or 0 when `cap` is too small. */
size_t rootset_format(const struct root_set *r, char *out, size_t cap);
/* The first build id in git describe's form (vX.Y.Z[-rcN][-N-gHEX][-dirty],
 * not inside a longer word) in a kernel image, or "": the release a set
 * is, since a release's kernel carries its tag and no other
 * (scripts/check-release-version.py). */
void release_of(const unsigned char *elf, size_t len, char out[ROOTSET_RELEASE_MAX + 1]);

#endif
