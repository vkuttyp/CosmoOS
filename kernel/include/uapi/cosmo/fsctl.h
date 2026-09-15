/*
 * fsctl.h - The /dev/fsctl control ABI (docs/audit/next-subsystem-fsctl.md).
 *
 * A privileged operator names one mount and asks the filesystem to check
 * or scrub itself. A command is one fixed-layout struct written whole at
 * its exact size; the result is read back from the same open file, and
 * belongs to that file alone.
 *
 * A mount is named by id, not by path. The same mount is at different
 * paths in different mount namespaces, and a path names different mounts
 * over time, so a command against a path is a command against whatever
 * is there when it arrives.
 */
#ifndef UAPI_COSMO_FSCTL_H
#define UAPI_COSMO_FSCTL_H

#include <stdint.h>

/* Version 1: LIST, CHECK and SCRUB. Every later version documents itself
 * here, beside the constant, as netctl.h does. */
#define COSMO_FSCTL_VERSION 1

#define COSMO_FSCTL_LIST  1   /* the mounts this namespace holds */
#define COSMO_FSCTL_CHECK 2   /* the structural check, against one id */
#define COSMO_FSCTL_SCRUB 3   /* the scrub, against one id */

/* CHECK only: repair what has one right answer. Off by default. */
#define COSMO_FSCTL_F_REPAIR (1u << 0)

/* What a filesystem offers, in a listing's `caps`. */
#define COSMO_FSCTL_CAP_CHECK (1u << 0)
#define COSMO_FSCTL_CAP_SCRUB (1u << 1)

/* The command. Written whole, at exactly this size: a write of any other
 * length is -EINVAL rather than a struct reinterpreted. */
struct cosmo_fsctl {
    uint16_t version;
    uint16_t op;
    uint32_t flags;
    uint64_t mount_id;        /* ignored by LIST */
};

/*
 * Every result begins with this, so a reader knows what follows and how
 * big one record is before parsing any of it. `count` is how many
 * records follow; `total` is how many there were to report, which a
 * listing that raced a mount reports larger than `count` -- a short
 * answer that says it is short, rather than one that looks complete.
 */
struct cosmo_fsctl_result {
    uint16_t version;
    uint16_t kind;            /* the op this answers */
    uint32_t count;
    uint32_t total;
    uint32_t bytes;           /* of one record, so a reader can skip one it does not know */
};

struct cosmo_fsctl_mount {    /* LIST: one per mount */
    uint64_t id;
    uint64_t ns_id;           /* the namespace whose path this is */
    uint32_t flags;           /* the mount flags */
    uint32_t caps;            /* COSMO_FSCTL_CAP_* */
    char fstype[16];
    char path[1024];          /* VFS_PATH_MAX; as recorded when this namespace gained the mount */
};

/*
 * CHECK. The ten classes are an array rather than ten named fields, so a
 * version that adds one grows `nclasses` and moves nothing. The index
 * order is part of the ABI:
 *
 *   0 alloc_not_seen   the bitmap says allocated, nothing reaches it
 *   1 seen_not_alloc   something reaches it, the bitmap says free
 *   2 dup              the live generation claims one block twice
 *   3 nlink_wrong      an inode's link count differs from the entries
 *   4 orphan           an inode allocated and unreachable
 *   5 dangling_entry   an entry naming a free or out-of-range slot
 *   6 dir_bad          a malformed entry or a bad pointer
 *   7 counter_wrong    a superblock total the walk disagrees with
 *   8 chain_cycle      a metadata chain that revisits a block
 *   9 unreadable       a block that could not be read
 */
#define COSMO_FSCTL_CLASSES 10
#define COSMO_FSCTL_NAMES   8

struct cosmo_fsctl_class {
    uint64_t count;
    uint64_t repaired;
    uint64_t name[COSMO_FSCTL_NAMES];   /* the first few offenders, to find them by */
    uint32_t named;
    uint32_t reserved;
};

#define COSMO_FSCTL_R_PARTIAL        (1u << 0)   /* something was unreadable */
#define COSMO_FSCTL_R_CLEAN          (1u << 1)   /* every class empty */
#define COSMO_FSCTL_R_REPAIR_REFUSED (1u << 2)   /* repair asked for, the walk was not sure */

struct cosmo_fsctl_check {
    uint32_t nclasses;        /* COSMO_FSCTL_CLASSES for version 1 */
    uint32_t flags;           /* COSMO_FSCTL_R_* */
    uint64_t blocks_seen;
    uint64_t inodes_seen;
    uint64_t dirs_seen;
    uint64_t snapshots_seen;
    uint64_t counted_free;
    uint64_t counted_inodes;
    uint64_t bytes_allocated;
    uint64_t elapsed_ns;
    struct cosmo_fsctl_class class[COSMO_FSCTL_CLASSES];
};

struct cosmo_fsctl_scrub {
    uint64_t blocks_read;
    uint64_t inodes;
    uint64_t repaired;        /* blocks another copy could satisfy, and now do */
    uint64_t unrecoverable;   /* blocks no copy could satisfy */
};

#endif /* UAPI_COSMO_FSCTL_H */
