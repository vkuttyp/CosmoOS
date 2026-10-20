/*
 * rootsnap -- a mount's snapshots through /dev/fsctl, for the programs that
 * take them on the operator's behalf: pkg's transactions, init's recovery
 * of an interrupted one, cosmo-upgrade's boot environments (roadmap 1.2 B4;
 * docs/userland/design.md, "Snapshots from programs"). cosmo-snap is the
 * operator's own front end and keeps its own code.
 *
 * Every call returns 0, or -1 with errno set: from the kernel's answer to
 * the command, or EOPNOTSUPP when the mount keeps no snapshots.
 */
#ifndef ROOTSNAP_H
#define ROOTSNAP_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/cosmo/fsctl.h>

struct rootsnap {
    int fd;                          /* /dev/fsctl */
    unsigned long long mount_id;
};

/* The snapshots of the mount at `path` ("/" for the root). */
int rootsnap_open(struct rootsnap *r, const char *path);
void rootsnap_close(struct rootsnap *r);

/* The pool record (format, rollback state, free and total bytes) and
 * every snapshot, oldest first, without sizes; `*out` is malloc'd. */
int rootsnap_list(struct rootsnap *r, struct cosmo_fsctl_snap_pool *pool, struct cosmo_fsctl_snap **out,
                  unsigned *n);

int rootsnap_create(struct rootsnap *r, const char *name, struct cosmo_fsctl_snap *out);
int rootsnap_delete(struct rootsnap *r, const char *name);
/* A rollback to `name` at the next mount, the tree it replaces kept as
 * `safety` (NULL: the kernel's pre-rollback-<seconds>); `*out` is it. */
int rootsnap_rollback(struct rootsnap *r, const char *name, const char *safety, struct cosmo_fsctl_snap *out);
int rootsnap_cancel(struct rootsnap *r);
int rootsnap_clone(struct rootsnap *r, const char *name, const char *clone, struct cosmo_fsctl_snap *out);

/* The snapshot named `name` in a list, or NULL. */
const struct cosmo_fsctl_snap *rootsnap_find(const struct cosmo_fsctl_snap *v, unsigned n, const char *name);

#endif
