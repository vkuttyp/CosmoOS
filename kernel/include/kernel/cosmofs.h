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

/* Register with the VFS. Once, after vfs_init(). */
void cosmofs_init(void);

/* Write a fresh filesystem (generation 1, empty root) over the device.
 * Sleeps. -EINVAL for a device too small (< 64 blocks) or too large. */
int cosmofs_format(struct blkdev *bd);
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
    uint64_t degraded;        /* copies the member table promised and the mount did not find */
    unsigned version;         /* the on-disk format version this filesystem was written at */
    uint64_t free_root;       /* v9: head of the record of what the last root freed, or 0 */
};
int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out);

/* What a scrub found (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Scrub"). */
struct cosmofs_scrub_stats {
    uint64_t blocks_read;
    uint64_t inodes;
    uint64_t repaired;       /* blocks another copy could satisfy, and now do */
    uint64_t unrecoverable;  /* blocks no copy could satisfy */
};
/* Read every block the filesystem reaches through the ordinary
 * verifying path, repairing a mirrored member where a copy has rotted.
 * -EIO when something was unrecoverable (the counts still say what),
 * -EINVAL for a mount that is not a cosmofs. Sleeps; takes the mount's
 * lock for the whole walk. */
int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out);

/* --- the structural check (docs/audit/next-subsystem-fsck.md) ---------------
 *
 * The scrub asks whether every block is still what was written; this asks
 * whether the blocks add up. Ten finding classes, each naming its first
 * offenders; four of them are repaired with COSMOFS_CHECK_REPAIR, and the
 * rest are reported because no repair has one right answer.
 */
#define COSMOFS_CHECK_REPAIR (1u << 0)
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
    struct cosmofs_check_class counter_wrong;    /* a superblock total the walk disagrees with */
    struct cosmofs_check_class chain_cycle;      /* a metadata chain that revisits a block */
    struct cosmofs_check_class unreadable;       /* a metadata block that could not be read */
    uint64_t blocks_seen, inodes_seen, dirs_seen, snapshots_seen;
    uint64_t counted_free;              /* free blocks the walk counted */
    uint64_t counted_inodes;            /* inodes with links the walk counted */
    uint64_t bytes_allocated;           /* the chunked maps, so the cost is visible */
    bool partial;                       /* something was unreadable: the answer is incomplete */
    bool clean;                         /* every class empty */
    /* Repair was asked for and refused: the walk met something it had to
     * skip, so "nothing reaches this" may mean "this pass did not get
     * there", and every repair here is an argument from absence. */
    bool repair_refused;
    uint64_t elapsed_ns;
};

/* Walks the live tree, every snapshot, both allocation maps and the inode
 * map under the mount's lock. -EINVAL for a mount that is not a cosmofs,
 * -EIO for an abandoned transaction, -ENOMEM before the walk starts rather
 * than half way through it. A filesystem with findings is not an error:
 * the report says what they are. In every build: an operator reaches it
 * through /dev/fsctl (docs/audit/next-subsystem-fsctl.md). */
int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);

/* Test hook: the next unmount discards the open transaction instead of
 * committing it, as a crash before the root write would. */
void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard);
/* Test hooks for the writeback thread: autonomous commits on/off (the
 * replay harness wants every root write to be one it asked for), and the
 * age trigger in milliseconds. */
void cosmofs_test_set_writeback(struct mount *mnt, bool on);
void cosmofs_test_set_writeback_interval(struct mount *mnt, unsigned ms);
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
};
int cosmofs_test_corrupt(struct mount *mnt, enum cosmofs_corruption kind, uint64_t ino, uint64_t *what);
/* Test hook: where an inode's logical block actually lives, as a DVA;
 * -ENOENT for a hole. Lets a test rot the disk under a known block. */
int cosmofs_test_block_of(struct mount *mnt, uint64_t ino, uint64_t lblk, uint64_t *dva);
/* Test hook: free blocks on one member, or UINT64_MAX past the last. */
uint64_t cosmofs_test_member_free(struct mount *mnt, unsigned vdev);

/*
 * Entries on every snapshot's deadlist: all of them when `of` is 0, or
 * the ones naming that block. A held block is not a leak and not a
 * finding -- the checker claims a deadlist as metadata -- so this is the
 * only number that distinguishes a block freed from a block held
 * (docs/audit/next-subsystem-unmount-leak.md).
 */
uint64_t cosmofs_test_deadlist_len(struct mount *mnt, uint64_t of);

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
