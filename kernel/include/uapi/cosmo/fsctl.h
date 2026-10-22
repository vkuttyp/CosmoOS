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
/*
 * Version 2 grows the CHECK result's class array from ten to thirteen
 * (below). The result is a fixed-size struct the kernel writes whole,
 * so growing it is NOT prefix-compatible: an old client asking for the
 * ten-class size would get -ERANGE from a new kernel, part way through
 * a command that had already passed the version check. Bumping is what
 * turns that into an early, explicit refusal -- a version-1 client and
 * a version-2 kernel now disagree at the gate instead of in the middle
 * (docs/audit/next-subsystem-fsck-unchecked.md).
 */
/*
 * Version 3 (roadmap 1.2 B1) adds the snapshot operations, which name a
 * snapshot: the command grows two name fields and so a version-2
 * command, sixteen bytes, is refused by its length before anything is
 * read (docs/kernel-services/filesystem/cosmofs/design.md, "Format
 * version 12"). The check result grows from thirteen classes to sixteen
 * on the same terms as version 2's growth.
 */
#define COSMO_FSCTL_VERSION 3

#define COSMO_FSCTL_LIST  1   /* the mounts this namespace holds */
#define COSMO_FSCTL_CHECK 2   /* the structural check, against one id */
#define COSMO_FSCTL_SCRUB 3   /* the scrub, against one id */
/* Version 3: snapshots of one mount, named by `name` (and `name2`). */
#define COSMO_FSCTL_SNAP_CREATE   4   /* take snapshot `name` of the live tree */
#define COSMO_FSCTL_SNAP_LIST     5   /* every snapshot, with its sizes: cosmo_fsctl_snap records */
#define COSMO_FSCTL_SNAP_DELETE   6   /* delete snapshot `name` */
#define COSMO_FSCTL_SNAP_ROLLBACK 7   /* make `name` the live tree at the next mount */
#define COSMO_FSCTL_SNAP_CANCEL   8   /* withdraw a pending rollback */
#define COSMO_FSCTL_SNAP_CLONE    9   /* `name2`: a writable clone of snapshot `name` */
#define COSMO_FSCTL_BOOT_OK      10   /* this boot completed: release the hold a rollback left */
/*
 * Roadmap 1.2 B2: two more operations, and no version bump. Each answers
 * with a record of its own, so a version-3 client that never sends them
 * is unaffected, and a kernel without them refuses the op with -EINVAL --
 * which is how fsck.cosmofs and fsctl tell an older kernel.
 */
#define COSMO_FSCTL_CHECK_DEV    11   /* the check on an UNMOUNTED device `name` (fstype `name2`, "" = cosmofs):
                                       * one cosmo_fsctl_devcheck. Read-only unless F_REPAIR */
#define COSMO_FSCTL_SCRUB_EX     12   /* the scrub, reporting only unless F_REPAIR: one cosmo_fsctl_scrub_ex */

/* CHECK, CHECK_DEV, SCRUB_EX: repair what has one right answer. Off by
 * default. */
#define COSMO_FSCTL_F_REPAIR (1u << 0)
/* CHECK_DEV with F_REPAIR: repair only what a boot may repair without
 * asking (leaked blocks, totals, a stale rollback record or hold,
 * snapshot identity, orphans, link counts), and nothing at all when the
 * check found anything else (docs/kernel-services/filesystem/cosmofs/
 * design.md, "fsck at boot"). */
#define COSMO_FSCTL_F_BOOT (1u << 3)

/* What a filesystem offers, in a listing's `caps`. */
#define COSMO_FSCTL_CAP_CHECK (1u << 0)
#define COSMO_FSCTL_CAP_SCRUB (1u << 1)
#define COSMO_FSCTL_CAP_SNAP  (1u << 2)   /* version 3: the SNAP_* operations */

/* The command. Written whole, at exactly this size: a write of any other
 * length is -EINVAL rather than a struct reinterpreted. */
#define COSMO_FSCTL_NAME_MAX 31   /* a snapshot's name, as the filesystem allows it */

struct cosmo_fsctl {
    uint16_t version;
    uint16_t op;
    uint32_t flags;
    uint64_t mount_id;        /* ignored by LIST */
    /* Version 3: NUL-terminated; zero for the operations that take none. */
    char name[COSMO_FSCTL_NAME_MAX + 1];
    char name2[COSMO_FSCTL_NAME_MAX + 1];
    /* Version 3, SNAP_LIST: only snapshots with an id above this. A
     * listing returns at most COSMO_FSCTL_SNAP_PAGE records, oldest
     * (lowest id) first, and `total` says how many there are; a reader
     * asks again with the last id it got until it has them all. */
    uint64_t cursor;
};

#define COSMO_FSCTL_SNAP_PAGE 1024

/* SNAP_ROLLBACK only: leave the safety snapshot's name to the caller
 * (`name2`) instead of pre-rollback-<seconds>. */
#define COSMO_FSCTL_F_SAFETY_NAME (1u << 1)
/* SNAP_LIST only: no sizes (0 in every record), so no tree is walked --
 * what a caller wanting the pool record alone asks for. */
#define COSMO_FSCTL_F_NO_SIZES (1u << 2)

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
 * CHECK. The classes are an array rather than named fields, so a
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
 *
 * Version 2 appends three and moves nothing, which is what the array
 * is for (docs/audit/next-subsystem-fsck-unchecked.md):
 *
 *  10 extent_order     an inode whose runs do not ascend by lblk
 *  11 extent_overlap   two runs of one inode covering one lblk
 *  12 dir_dup_name     a name that repeats inside one directory
 *
 * Version 3 appends three for the structures format version 12 adds:
 *
 *  13 snap_identity    a snapshot with no uuid, or one another shares,
 *                      or an id above the list's high-water mark
 *  14 rollback_record  a pending rollback whose record does not check
 *                      out or names a snapshot that is not there
 *  15 hold             a hold naming a snapshot that is not there
 *
 * A version-1 reader stops at `nclasses` and sees the first ten, which
 * is the contract; it will report "not clean" with ten zeroes if a
 * filesystem has only one of the new faults, so a reader that shows
 * `clean` should also show `nclasses`.
 */
#define COSMO_FSCTL_CLASSES 16
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
    uint32_t nclasses;        /* COSMO_FSCTL_CLASSES; 10 in version 1, 13 in version 2, 16 in version 3 */
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

/*
 * SCRUB_EX: every block every tree reaches (the live tree, each snapshot
 * and clone), each read on every copy of its member and verified, at
 * most `rate` blocks a second (sysctl fs.scrub_rate; 0 = no bound). A
 * bad copy is rewritten from a good one only with F_REPAIR; without it
 * the scrub writes nothing and `bad_copies` says what a repair would do.
 */
struct cosmo_fsctl_scrub_ex {
    uint64_t blocks_read;     /* distinct blocks verified */
    uint64_t inodes;
    uint64_t bad_copies;      /* copies that did not verify while another did */
    uint64_t repaired;        /* of those, rewritten from a good copy (F_REPAIR) */
    uint64_t unrecoverable;   /* blocks no copy could satisfy */
    uint64_t skipped;         /* blocks the open transaction holds in memory: the next scrub's */
    uint64_t elapsed_ns;
    uint64_t rate;            /* the bound it ran at, blocks a second; 0 for none */
};

/*
 * CHECK_DEV: what was found before the check could run, and around it.
 * The 16 check classes are the structure's; these are the device's: a
 * superblock pair that names no pool or two, a mount that failed and
 * where, and what the pool's rollback record says. When F_CHECKED is
 * clear the check record is zero and these say why.
 */
#define COSMO_FSCTL_DEV_F_CHECKED          (1u << 0)   /* the check ran: `check` is its record */
#define COSMO_FSCTL_DEV_F_SUPER_NONE       (1u << 1)   /* neither superblock slot verifies */
#define COSMO_FSCTL_DEV_F_SUPER_DISAGREE   (1u << 2)   /* both verify and name different pools */
#define COSMO_FSCTL_DEV_F_MOUNT_FAILED     (1u << 3)   /* `mount_error` at `mount_stage` */
#define COSMO_FSCTL_DEV_F_FELL_BACK        (1u << 4)   /* the newer root did not load; the older one did */
#define COSMO_FSCTL_DEV_F_ROLLBACK_PENDING (1u << 5)   /* a rollback record waits for a writable mount */
#define COSMO_FSCTL_DEV_F_ROLLBACK_HELD    (1u << 6)   /* a rollback applied; held until BOOT_OK */
#define COSMO_FSCTL_DEV_F_ROLLBACK_APPLIED (1u << 7)   /* this (writable) check's mount switched to it */
#define COSMO_FSCTL_DEV_F_ROLLBACK_REFUSED (1u << 8)   /* its mount tried and could not: `rollback_error` */
#define COSMO_FSCTL_DEV_F_COMMITTED        (1u << 9)   /* a repair was made and committed */
#define COSMO_FSCTL_DEV_F_BOOT_REFUSED     (1u << 10)  /* F_BOOT: a finding outside the boot's classes */

/* Where a failed mount stopped (`mount_stage`). */
#define COSMO_FSCTL_STAGE_NONE          0
#define COSMO_FSCTL_STAGE_SUPER         1   /* no superblock */
#define COSMO_FSCTL_STAGE_MEMBERS       2   /* the member table, a missing device, the key */
#define COSMO_FSCTL_STAGE_BITMAP        3   /* the allocation maps */
#define COSMO_FSCTL_STAGE_FREE_RECORD   4   /* the record of what the last commit freed */
#define COSMO_FSCTL_STAGE_SNAPSHOTS     5   /* the snapshot list, the clones */
#define COSMO_FSCTL_STAGE_ROLLBACK      6
#define COSMO_FSCTL_STAGE_ORPHAN_RECORD 7   /* the inodes the last root still owed */
#define COSMO_FSCTL_STAGE_ROOT          8   /* the root directory (not a directory: -ENOTDIR) */

struct cosmo_fsctl_dev {
    uint32_t flags;           /* COSMO_FSCTL_DEV_F_* */
    int32_t mount_error;      /* a negative errno, with F_MOUNT_FAILED */
    uint32_t mount_stage;     /* COSMO_FSCTL_STAGE_* */
    int32_t rollback_error;   /* with F_ROLLBACK_REFUSED */
    uint32_t format;          /* the pool's format version */
    uint32_t replayed_orphans;/* inodes the mount's orphan replay released (in memory when read-only) */
    uint64_t generation;      /* of the root that was checked */
    uint64_t slot_generation[2];   /* each superblock slot's, 0 when it does not verify */
    uint64_t replayed_frees;  /* blocks the mount's free-record replay released */
    char rollback_target[COSMO_FSCTL_NAME_MAX + 1];   /* a pending or held rollback's target, "" when gone */
    uint64_t reserved[4];
};

struct cosmo_fsctl_devcheck {
    struct cosmo_fsctl_dev dev;
    struct cosmo_fsctl_check check;
};

/*
 * SNAP_LIST's result is the header, then one cosmo_fsctl_snap_pool, then
 * `count` cosmo_fsctl_snap records (`bytes` is one record's size). The
 * pool record says what is true of the filesystem rather than of one
 * snapshot: its format version -- which a kernel must read to mount it,
 * so cosmo-upgrade asks before switching to an older one -- and whether a
 * rollback is pending or held.
 */
#define COSMO_FSCTL_POOL_F_PENDING (1u << 0)   /* a rollback applies at the next mount */
#define COSMO_FSCTL_POOL_F_HELD    (1u << 1)   /* a rollback applied; the hold lasts until BOOT_OK */

struct cosmo_fsctl_snap_pool {
    uint32_t format;          /* the filesystem's format version */
    uint32_t flags;           /* COSMO_FSCTL_POOL_F_* */
    uint64_t free_bytes;      /* what data can still use (the metadata reserve excluded) */
    uint64_t total_bytes;     /* the pool's size */
    /* Roadmap 1.2 B2 (CFS-20), in what was a reserved word that every
     * earlier kernel wrote 0: why this mount did not apply the pending
     * rollback it found, a negative errno; 0 when it applied it or there
     * was none. */
    int32_t rollback_error;
    uint32_t reserved;
};

/*
 * SNAP_LIST: one per snapshot, oldest first. The sizes are a walk of
 * every tree under the mount's lock: `referenced` is what the snapshot's
 * tree reaches, `unique` the part of it no other tree (the live one, any
 * other snapshot or clone) reaches -- what deleting it would give back.
 */
#define COSMO_FSCTL_SNAP_F_CLONE    (1u << 0)   /* a writable clone; `origin_id` is its snapshot */
#define COSMO_FSCTL_SNAP_F_TARGET   (1u << 1)   /* a pending rollback names it as the tree to become live */
#define COSMO_FSCTL_SNAP_F_SAFETY   (1u << 2)   /* a pending rollback took it of the live tree */
#define COSMO_FSCTL_SNAP_F_HELD     (1u << 3)   /* not deletable until the next boot completes */
#define COSMO_FSCTL_SNAP_F_LIVE     (1u << 4)   /* a held rollback's target: the live tree was rolled back to it */

struct cosmo_fsctl_snap {
    uint64_t id;
    uint64_t generation;      /* the commit it pins */
    uint64_t created_ns;      /* wall clock */
    uint64_t origin_id;
    uint64_t referenced;      /* bytes */
    uint64_t unique;          /* bytes */
    uint8_t uuid[16];
    uint32_t flags;           /* COSMO_FSCTL_SNAP_F_* */
    uint32_t format;          /* the filesystem's format version */
    char name[COSMO_FSCTL_NAME_MAX + 1];
};

#endif /* UAPI_COSMO_FSCTL_H */
