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

#endif
