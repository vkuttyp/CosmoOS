/*
 * cosmofs.h - The persistent copy-on-write filesystem (kernel API).
 */

#ifndef KERNEL_COSMOFS_H
#define KERNEL_COSMOFS_H

#include <kernel/types.h>

struct blkdev;
struct mount;
struct fs_type;

extern struct fs_type cosmofs_fs_type;

/* Register with the VFS. Once, from boot composition (bootfs_init). */
void cosmofs_init(void);

/* Write a fresh filesystem (generation 1, empty root) over the device.
 * Sleeps. -EINVAL for a device too small (< 64 blocks) or too large. */
int cosmofs_format(struct blkdev *bd);
/* The same, with a label of up to 16 bytes (NULL or "": none); -EINVAL
 * for a longer one. */
int cosmofs_format_label(struct blkdev *bd, const char *label);
/* Format `n` devices as one pool: member 0 carries the superblocks and
 * the member table, the rest a label (design.md, "The member table"). */
int cosmofs_format_pool(struct blkdev **bd, unsigned n);
/* The same with `copies` devices per member: bd holds n*copies devices,
 * member by member. A member is then a mirror group and every write
 * goes to all of its devices. */
int cosmofs_format_mirror(struct blkdev **bd, unsigned n, unsigned copies);
/* Format an encrypted filesystem: a random master key wrapped with
 * `key`. File contents and directory names become ciphertext; the
 * allocation and inode metadata stay in the clear
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 7"). */
int cosmofs_format_encrypted(struct blkdev *bd, const void *key, size_t len);
/*
 * The key of an encrypted pool (roadmap 1.2 B5; design.md, "The key
 * handle"): /dev/fsctl's KEY_* operations. A pool is named by a device
 * (`bd`) or a mount (`mnt`), not both. LOAD checks `key` against the pool
 * before it keeps it (-EKEYREJECTED when it does not open it); SLOT
 * needs the mount's own master key in `key`, and refuses to empty the
 * last slot (-EBUSY).
 */
struct cosmo_keyslot;
struct cosmo_fsctl_keyinfo;
enum cosmofs_key_op { COSMOFS_KEY_INFO, COSMOFS_KEY_LOAD, COSMOFS_KEY_UNLOAD, COSMOFS_KEY_SLOT };
struct cosmofs_key_req {
    enum cosmofs_key_op op;
    unsigned slot;                       /* SLOT */
    const uint8_t *key;                  /* LOAD, SLOT: 32 bytes */
    const struct cosmo_keyslot *rec;     /* SLOT */
    struct cosmo_fsctl_keyinfo *info;    /* INFO: filled in */
};
int cosmofs_keyctl(struct blkdev *bd, struct mount *mnt, struct cosmofs_key_req *req);
/* An encrypted pool from /dev/blkctl's FORMAT_KEYED: `n` devices as one
 * mirrored member, the uuid the slots were sealed for, the master key and
 * the slots user space sealed. */
int cosmofs_format_keys(struct blkdev **bd, unsigned n, const char *label, const uint8_t uuid[16], const uint8_t master[32],
                        const struct cosmo_keyslot *slots);

#if CONFIG_DEBUG
/* Test hook: an encrypted pool of `members` members, one device each. */
int cosmofs_test_format_keyed_members(struct blkdev **bd, unsigned members, const uint8_t uuid[16], const uint8_t master[32],
                                      const struct cosmo_keyslot *slots);
#endif

/* Test hook: supply the key for a mount, as the unlock channel would. */
int cosmofs_test_unlock(struct mount *mnt, const void *key, size_t len);
/* Rewrap the master key with a new user key. No file is rewritten. */
int cosmofs_rekey(struct mount *mnt, const void *key, size_t len);

struct cosmofs_stats {
    uint64_t generation;      /* last committed */
    uint64_t free_blocks;     /* in-memory count */
    uint64_t total_blocks;
    uint64_t inode_count;
    unsigned dirty_buffers;
    unsigned pending_frees;
    uint64_t reserve_blocks;  /* blocks only metadata may take */
    uint64_t commits;         /* since mount */
    uint64_t wb_commits;      /* of which by the writeback thread */
    uint64_t csum_failures;   /* data or directory blocks refused for a bad checksum */
    unsigned members;         /* pool members; 1 before format version 4 */
    unsigned devices;         /* devices behind them: more than members means mirroring */
    uint64_t repairs;         /* blocks written back from a good copy since mount */
    uint64_t degraded;        /* copies the member table promises that are not ONLINE */
    unsigned version;         /* the on-disk format version this filesystem was written at */
    uint64_t free_root;       /* v9: head of the record of what the last root freed, or 0 */
    uint64_t orphan_root;     /* v10: head of the record of what this root still owes, or 0 */
    unsigned pending_orphans; /* inodes unlinked with something still holding them */
};
int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out);

/* What a scrub found (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Scrub"). */
struct cosmofs_scrub_stats {
    uint64_t blocks_read;
    uint64_t inodes;
    uint64_t repaired;       /* blocks another copy could satisfy, and now do */
    uint64_t unrecoverable;  /* blocks no copy could satisfy */
    uint64_t bad_copies;     /* copies that did not verify while another did (repaired or not) */
    uint64_t skipped;        /* blocks the open transaction holds in memory */
    uint64_t elapsed_ns;
    uint64_t rate;           /* the bound it ran at, blocks a second; 0 for none */
};
/* cosmofs_scrub: write a good copy over each bad one. Without it the
 * scrub reads and verifies and writes nothing (roadmap 1.2 B2). */
#define COSMOFS_SCRUB_REPAIR (1u << 0)
/* The scrub's bound, blocks a second over every member (sysctl
 * fs.scrub_rate; 0 = none). Read at the start of each scrub. */
extern unsigned g_cosmofs_scrub_rate;
#define COSMOFS_SCRUB_RATE_DEFAULT 4096u
/* Read every block the filesystem reaches through the ordinary
 * verifying path, repairing a mirrored member where a copy has rotted.
 * 0 whenever the walk ran -- `unrecoverable` counts what no copy could
 * satisfy (CFS-11) -- -EIO for an abandoned transaction, -EINVAL for a
 * mount that is not a cosmofs. Sleeps; takes the mount's lock for the
 * whole walk. */
int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out, unsigned flags);

/* --- the structural check (docs/audit/next-subsystem-fsck.md) ---------------
 *
 * The scrub asks whether every block is still what was written; this asks
 * whether the blocks add up. Ten finding classes, each naming its first
 * offenders; four of them are repaired with COSMOFS_CHECK_REPAIR, and the
 * rest are reported because no repair has one right answer.
 */
#define COSMOFS_CHECK_REPAIR (1u << 0)
/* With REPAIR, on a mount nothing else is using (cosmofs_check_dev): a
 * block something reaches and the bitmap calls free is marked allocated.
 * On a mounted filesystem the allocator may already have handed it out,
 * so it is only reported there. */
#define COSMOFS_CHECK_OFFLINE (1u << 1)
/* With REPAIR: only the classes a boot repairs without asking, and
 * nothing at all when anything else was found (`boot_refused`). */
#define COSMOFS_CHECK_BOOT   (1u << 2)
#define CFS_CHECK_NAMES      8u

struct cosmofs_check_class {
    uint64_t count;
    uint64_t repaired;                  /* with COSMOFS_CHECK_REPAIR */
    uint64_t name[CFS_CHECK_NAMES];     /* a block or an inode, per class */
    unsigned named;
};

struct cosmofs_check_report {
    struct cosmofs_check_class alloc_not_seen;   /* allocated, nothing reaches it */
    struct cosmofs_check_class seen_not_alloc;   /* reached, the bitmap says free */
    struct cosmofs_check_class dup;              /* the live generation claims it twice */
    struct cosmofs_check_class nlink_wrong;      /* nlink differs from the entries naming it */
    struct cosmofs_check_class orphan;           /* an inode no name reaches */
    struct cosmofs_check_class dangling_entry;   /* an entry naming a free slot */
    struct cosmofs_check_class dir_bad;          /* a malformed entry or a bad pointer */
    /* The format says runs are sorted by lblk and never overlap
     * (docs/.../cosmofs/design.md). Two classes, not one, because they
     * are different repairs: an ordering fault may be repairable by
     * sorting, an overlap is data loss
     * (docs/audit/next-subsystem-fsck-unchecked.md). */
    struct cosmofs_check_class extent_order;     /* an inode whose runs do not ascend by lblk */
    struct cosmofs_check_class extent_overlap;   /* two runs of one inode covering one lblk */
    struct cosmofs_check_class dir_dup_name;     /* a name that repeats in one directory */
    struct cosmofs_check_class counter_wrong;    /* a superblock total the walk disagrees with */
    struct cosmofs_check_class chain_cycle;      /* a metadata chain that revisits a block */
    struct cosmofs_check_class unreadable;       /* a metadata block that could not be read */
    /* Format version 12 (design.md, "Format version 12"): a snapshot with
     * no uuid, one another shares, or an id past the list's high-water
     * mark; a pending rollback record that does not check out or names a
     * snapshot that is not there; a hold naming one that is not there. */
    struct cosmofs_check_class snap_identity;
    struct cosmofs_check_class rollback_record;
    struct cosmofs_check_class hold;
    uint64_t blocks_seen, inodes_seen, dirs_seen, snapshots_seen;
    uint64_t csum_two_level;            /* checksum trees walked with two levels (format 14, CFS-10) */
    uint64_t counted_free;              /* free blocks the walk counted */
    uint64_t counted_inodes;            /* inodes with links the walk counted */
    uint64_t bytes_allocated;           /* the chunked maps, so the cost is visible */
    bool partial;                       /* something was unreadable: the answer is incomplete */
    bool clean;                         /* every class empty */
    /* Repair was asked for and refused: the walk met something it had to
     * skip, so "nothing reaches this" may mean "this pass did not get
     * there", and every repair here is an argument from absence. */
    bool repair_refused;
    bool boot_refused;                  /* COSMOFS_CHECK_BOOT: a finding outside the boot's classes */
    uint64_t elapsed_ns;
};

/*
 * The check on a device nothing has mounted (roadmap 1.2 B2,
 * fsck.cosmofs): both superblock slots compared, the pool mounted where
 * no namespace sees it -- read-only, so it writes nothing (CF21), unless
 * COSMOFS_CHECK_REPAIR -- checked, a repair committed, and unmounted.
 * The fields mirror uapi/cosmo/fsctl.h's struct cosmo_fsctl_dev, whose
 * flag and stage values they use. 0 whenever it answered, including
 * "the check could not run, and here is why"; -EBUSY for a mounted
 * device, -ENOMEM.
 */
struct cosmofs_dev_report {
    unsigned flags;
    int mount_error;
    unsigned mount_stage;
    int rollback_error;
    unsigned format;
    unsigned replayed_orphans;
    uint64_t generation;
    uint64_t slot_generation[2];
    uint64_t replayed_frees;
    char rollback_target[32];
};
struct blkdev;
int cosmofs_check_dev(struct blkdev *bd, unsigned flags, struct cosmofs_check_report *rep,
                      struct cosmofs_dev_report *dev);

/* Walks the live tree, every snapshot, both allocation maps and the inode
 * map under the mount's lock. -EINVAL for a mount that is not a cosmofs,
 * -EIO for an abandoned transaction, -ENOMEM before the walk starts rather
 * than half way through it. A filesystem with findings is not an error:
 * the report says what they are. In every build: an operator reaches it
 * through /dev/fsctl (docs/audit/next-subsystem-fsctl.md). */
int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);

/*
 * Snapshots, through /dev/fsctl (roadmap 1.2 B1; design.md, "Format
 * version 12"). One request struct for every operation, cosmofs's own as
 * the check report is: the VFS hands it over and copies records out.
 */
#define COSMOFS_SNAP_NAME_MAX 31u

struct cosmofs_snap_info {
    uint64_t id;
    uint64_t generation;
    uint64_t created_ns;
    uint64_t origin_id;
    uint64_t referenced;      /* bytes */
    uint64_t unique;          /* bytes */
    uint8_t uuid[16];
    uint32_t flags;           /* COSMOFS_SNAP_I_* */
    uint32_t format;          /* the filesystem's format version */
    char name[COSMOFS_SNAP_NAME_MAX + 1];
};

#define COSMOFS_SNAP_I_CLONE  (1u << 0)
#define COSMOFS_SNAP_I_TARGET (1u << 1)
#define COSMOFS_SNAP_I_SAFETY (1u << 2)
#define COSMOFS_SNAP_I_HELD   (1u << 3)
#define COSMOFS_SNAP_I_LIVE   (1u << 4)   /* a held rollback's target: the live tree was rolled back to it */

enum cosmofs_snap_op {
    COSMOFS_SNAP_CREATE = 1,
    COSMOFS_SNAP_LIST,
    COSMOFS_SNAP_DELETE,
    COSMOFS_SNAP_ROLLBACK,
    COSMOFS_SNAP_CANCEL,
    COSMOFS_SNAP_CLONE,
    COSMOFS_SNAP_BOOT_OK,
};

struct cosmofs_snap_req {
    enum cosmofs_snap_op op;
    const char *name;               /* the snapshot operated on */
    const char *name2;              /* CLONE: the clone; ROLLBACK: the safety snapshot's name, or NULL */
    struct cosmofs_snap_info *out;  /* LIST: up to `max` records; CREATE/ROLLBACK: the one made */
    unsigned max;
    bool no_sizes;                  /* LIST: skip the walks; sizes are 0 */
    uint64_t after;                 /* LIST: only snapshots with an id above this (a page's cursor) */
    unsigned count;                 /* filled in */
    unsigned total;                 /* LIST: how many there were */
    unsigned format;                /* LIST: the filesystem's format version */
    unsigned pool_flags;            /* LIST: COSMOFS_POOL_* */
    uint64_t free_bytes;            /* LIST: what data can still use */
    uint64_t total_bytes;           /* LIST: the pool's size */
    int rollback_error;             /* LIST: why this mount did not apply a pending rollback (CFS-20), or 0 */
};

/*
 * The pool's copies (roadmap 1.2 B3; design.md, "Format version 13"):
 * what /dev/fsctl's POOL_* operations reach. Devices are named by their
 * block device names; a copy without a device by "member:copy".
 */
enum cosmofs_pool_op {
    COSMOFS_POOL_STATUS,
    COSMOFS_POOL_OFFLINE,
    COSMOFS_POOL_ONLINE,
    COSMOFS_POOL_REPLACE,
    COSMOFS_POOL_ATTACH,
    COSMOFS_POOL_DETACH,
    COSMOFS_POOL_SPARE_ADD,
    COSMOFS_POOL_SPARE_REMOVE,
};

#define COSMOFS_COPY_SPARE 5u           /* cosmofs_copy_info.state: a spare, not a copy */
#define COSMOFS_COPY_I_WHOLE  (1u << 0)
#define COSMOFS_COPY_I_DETACH (1u << 1)

struct cosmofs_copy_info {
    unsigned member, copy, state, flags;   /* state: CFS_COPY_* or COSMOFS_COPY_SPARE */
    uint64_t since, cursor, blocks, to_do;
    uint32_t read_errors, write_errors, csum_errors;
    char dev[16];
};

struct cosmofs_pool_req {
    enum cosmofs_pool_op op;
    const char *name, *name2;
    struct cosmofs_copy_info *out;  /* STATUS: up to `max` */
    unsigned max, count, total;     /* count, total: filled in (STATUS); total > count when `max` was short */
    uint64_t generation;            /* STATUS: the pool's */
    unsigned members, format, rate;
    bool degraded, resilvering;
    char label[16];
};

/* sysctl fs.resilver_rate: blocks a second a resilver copies (0: no bound). */
extern unsigned g_cosmofs_resilver_rate;
#define COSMOFS_RESILVER_RATE_DEFAULT 8192u

int cosmofs_poolctl(struct mount *mnt, struct cosmofs_pool_req *req);
#if CONFIG_DEBUG
/* Test seams (roadmap 1.2 B3): hold every mount's resilver thread; run a
 * mount's resilver in the caller for at most `blocks` blocks (0: to the
 * end, takeovers by spares included). */
void cosmofs_test_resilver_hold(bool hold);
int cosmofs_test_resilver_run(struct mount *mnt, uint64_t blocks);
#endif

#define COSMOFS_POOL_PENDING (1u << 0)
#define COSMOFS_POOL_HELD    (1u << 1)

/* -EINVAL for a mount that is not a cosmofs; -EOPNOTSUPP for a pool too
 * old to carry snapshot identity (below format 11, which a snapshot
 * operation raises to 12). */
int cosmofs_snapctl(struct mount *mnt, struct cosmofs_snap_req *req);

/* Test hook: the next unmount discards the open transaction instead of
 * committing it, as a crash before the root write would. */
void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard);
/* Test hooks for the writeback thread: autonomous commits on/off (the
 * replay harness wants every root write to be one it asked for), and the
 * age trigger in milliseconds. */
void cosmofs_test_set_writeback(struct mount *mnt, bool on);
struct spool;
struct spool *cosmofs_test_pool(struct mount *mnt);
void cosmofs_test_set_writeback_interval(struct mount *mnt, unsigned ms);
/* Test hook: a lower bound on the runs one inode may have, so a test can
 * reach CFS_MAX_EXTENTS's behaviour in a few records (CFS-32). */
void cosmofs_test_set_max_extents(struct mount *mnt, unsigned n);
/* Test hook: format at an older on-disk version, to check that this
 * kernel still mounts and writes what an older one wrote. */
int cosmofs_test_format_version(struct blkdev *bd, unsigned version);
/* Test hook: put a value in the superblock word version 9 calls
 * `free_root`, on a filesystem too old to have one. Every image this
 * tree formats zeroes its reserved words, so the gate that ignores that
 * word below version 9 cannot be tested without manufacturing an image
 * that does not (docs/audit/next-subsystem-unmount-leak.md). */
int cosmofs_test_poison_free_root(struct blkdev *bd, uint64_t value);

/* Test hooks for the free record's failure paths: a commit that fails
 * after reserving the record's blocks, and a record whose count is past
 * what a block holds (docs/audit/next-subsystem-unmount-leak.md). */
void cosmofs_test_fail_freelog(struct mount *mnt, bool on);
int cosmofs_test_poison_freelog_count(struct blkdev *bd, uint64_t count);
/* The same for the deadlist fill, and the count of snapshot-list walks
 * a mount has done -- the measurement behind "one verdict, not two"
 * (docs/audit/next-subsystem-snap-deadlist.md). */
void cosmofs_test_fail_snapfill(struct mount *mnt, bool on);
/* The same for the orphan record's fill. */
void cosmofs_test_fail_orphan(struct mount *mnt, bool on);
/* Name a still-linked inode in the orphan record: the image a wrong
 * record makes, which the replay must refuse to act on. */
void cosmofs_test_orphan_add(struct mount *mnt, uint64_t ino);
uint64_t cosmofs_test_snap_walks(struct mount *mnt);
uint64_t cosmofs_test_snap_verdicts(struct mount *mnt);
/*
 * Test hook: break the filesystem in one named way, so that a finding
 * of the structural check is one a test produced on purpose. Eight of
 * the ten classes are manufactured here; `chain_cycle` is not, and nor
 * are four of the six places `dir_bad` is reported from -- both are
 * recorded in docs/audit/2026-09-deferred-work-inventory.md rather than
 * left to this comment.
 *
 * `what` returns the block or inode the corruption touched, which is
 * what the check must name back, and `ino` is the inode a case needs one
 * for (0 where none does). One hook rather than nine, because the list
 * of ways to break a filesystem belongs in one place.
 */
enum cosmofs_corruption {
    COSMOFS_CORRUPT_LEAK,        /* allocate a block and reference it from nothing */
    COSMOFS_CORRUPT_FREE_IN_USE, /* clear the bit of a block a file is using */
    COSMOFS_CORRUPT_CROSSLINK,   /* point a second inode at a block a file owns */
    COSMOFS_CORRUPT_NLINK,       /* add one to an inode's link count */
    COSMOFS_CORRUPT_ORPHAN,      /* an inode with blocks that no entry names */
    COSMOFS_CORRUPT_DANGLING,    /* a directory entry naming a free inode slot */
    COSMOFS_CORRUPT_DIRENT,      /* an entry whose type disagrees with its inode */
    COSMOFS_CORRUPT_COUNTER,     /* both superblock totals, each wrong by one */
    COSMOFS_CORRUPT_INO_SLOT,    /* an inode slot whose number is not its position */
    COSMOFS_CORRUPT_EXTENT_ORDER,   /* swap two runs so lblk descends */
    COSMOFS_CORRUPT_BAD_PTR,        /* a block pointer past the end of the pool */
    COSMOFS_CORRUPT_TWO_PARENTS,    /* a directory named by an entry in a second directory */
    COSMOFS_CORRUPT_SNAP_MEMBERS,   /* a snapshot member count larger than its block holds */
    COSMOFS_CORRUPT_DUP_NAME,       /* one name twice in a directory, at two inodes */
    COSMOFS_CORRUPT_CHAIN_CYCLE,    /* an extent chain whose last block names an earlier one */
    COSMOFS_CORRUPT_NAMELEN,        /* an entry whose namelen exceeds its slot */
    COSMOFS_CORRUPT_EXTENT_OVERLAP, /* a second run covering an lblk the first covers,
                                     * at a DIFFERENT pool block -- the case block_seen
                                     * cannot see (next-subsystem-fsck-unchecked.md) */
    COSMOFS_CORRUPT_SNAP_UUID,      /* v12: zero the uuid of the list head's first snapshot */
    COSMOFS_CORRUPT_ROLLBACK_CRC,   /* v12: flip a byte of the rollback record's own checksum */
    COSMOFS_CORRUPT_ROOT_TYPE,      /* the named inode (the root, 1) says it is a regular file */
};
int cosmofs_test_corrupt(struct mount *mnt, enum cosmofs_corruption kind, uint64_t ino, uint64_t *what);
/* Test hook: where an inode's logical block actually lives, as a DVA;
 * -ENOENT for a hole. Lets a test rot the disk under a known block. */
int cosmofs_test_block_of(struct mount *mnt, uint64_t ino, uint64_t lblk, uint64_t *dva);
#if CONFIG_DEBUG
/* Test hook (CF16): read and verify every block of an open file through
 * the map and checksums its in-memory inode holds; `*bad` counts the
 * failures. Debug builds only. */
struct vnode;
int cosmofs_test_verify_file(struct vnode *vn, uint64_t *bad);
#endif
/* Test hook: free blocks on one member, or UINT64_MAX past the last. */
uint64_t cosmofs_test_member_free(struct mount *mnt, unsigned vdev);
void cosmofs_test_mount_writeback(struct mount *mnt, uint64_t *dirty_notes, uint64_t *early);

/*
 * Entries on every snapshot's deadlist: all of them when `of` is 0, or
 * the ones naming that block. A held block is not a leak and not a
 * finding -- the checker claims a deadlist as metadata -- so this is the
 * only number that distinguishes a block freed from a block held
 * (docs/audit/next-subsystem-unmount-leak.md).
 */
uint64_t cosmofs_test_deadlist_len(struct mount *mnt, uint64_t of);
/* Test hook: snapshots taken while on are the version-11 kind, so a pool
 * at 11 is not raised -- how a test builds the list an older kernel left
 * behind, for the raise to convert (design.md, "Format version 12"). */
#if CONFIG_DEBUG
void cosmofs_test_snap_no_raise(struct mount *mnt, bool on);
/* Test hook: `left` more commits are published, the next is abandoned as
 * if the machine stopped (armed false: off). The unmount then drops what
 * is open, so the remount sees exactly the commits before the stop. */
void cosmofs_test_commit_budget(struct mount *mnt, bool armed, unsigned left);
#endif

#if CONFIG_DEBUG
/* The checker's duplicate-name hash. Only `cosmofs-check-dup-name`
 * uses it, to assert that the two names it relies on really do
 * collide -- otherwise a change to the hash would make that test's
 * false-positive half vacuous. */
unsigned cosmofs_test_name_hash(const char *name, unsigned len);
#endif

/*
 * The invariant a copied deadlist keeps: no block is named by more than
 * one entry, across every snapshot. Returns the entries whose block
 * another entry also names, with `*first` the first such block and
 * `*examined` how many entries were looked at -- because "none
 * duplicated" is also true of a filesystem with no deadlist, and a test
 * that cannot tell those apart proves nothing
 * (docs/audit/next-subsystem-snap-deadlist.md).
 */
uint64_t cosmofs_test_deadlist_dups(struct mount *mnt, uint64_t *examined, uint64_t *first);

/* The block the snapshot list starts at: the number that says whether a
 * change copied the block it changed or edited it where it lay. Taken
 * either side of one commit, because the allocator hands a superseded
 * block straight back to the next one. 0 when there is no list. */
uint64_t cosmofs_test_snap_root(struct mount *mnt);

#endif /* KERNEL_COSMOFS_H */
