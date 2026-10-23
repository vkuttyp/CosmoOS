/*
 * cosmofs_internal.h - In-memory state shared by the cosmofs sources.
 */

#ifndef COSMOFS_INTERNAL_H
#define COSMOFS_INTERNAL_H

#include <kernel/chacha20.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <kernel/storage.h>
#include <kernel/types.h>
#include <kernel/vfs.h>

#include "cosmofs_format.h"

#define CFS_BUF_CACHE 64u
#define CFS_MIN_BLOCKS 64u

/* The metadata MAC's key (format version 14). `on` false: CRC32C. */
struct cfs_mac {
    bool on;
    uint8_t key[32];
};

struct cfs_buf {
    struct list_node link;
    uint64_t blkno;
    uint8_t *data;          /* CFS_BLOCK bytes, DMA-able */
    bool dirty;
    unsigned refs;
};

/* Per-member state. The allocator works on a linear index that
 * concatenates the members, so that one flat bitmap and every algorithm
 * over it stay as they were; `base` is where this member starts in that
 * space and is a whole number of bitmap chunks, so no chunk straddles
 * two members (design.md, "One bitmap, many members"). */
struct cfs_memstate {
    uint64_t nblocks;        /* blocks on the device */
    uint64_t first_usable;   /* 2 on member 0 (the superblocks), 1 elsewhere (the label) */
    uint64_t base;           /* linear index of this member's block 0 */
    unsigned chunk0, nchunks;
    uint64_t alloc_root;     /* this member's CFS_KIND_ALLOCIDX block, as a DVA */
    uint64_t free_blocks;
    uint64_t alloc_hint;     /* linear */
    unsigned copies;         /* devices the table says this member has */
    uint64_t state_root;     /* v13: the member's CFS_KIND_COPYSTATE block, or 0 */
    uint8_t uuid[16];
};

/*
 * A clone (design.md, "Clones"): a writable tree in the snapshot list,
 * whose roots the commits advance. The list entry holds them as of the
 * last commit; this holds them as the open transaction has them, and
 * `dirty` says the entry is behind. `origin_reach` is every block the
 * origin's tree reaches, built on first need -- the origin is a snapshot
 * and does not change, so the set does not either.
 */
struct cfs_clone {
    uint64_t id;
    uint64_t origin_id;
    uint64_t imap_root;
    uint64_t next_ino;
    uint64_t inode_count;
    bool dirty;
    uint8_t *origin_reach;
};

struct cfs {
    struct spool *pool;
    struct mount *mnt;
    struct cfs_super sb;    /* the in-memory root: fields advance during the open transaction */
    unsigned sb_slot;       /* slot the current committed root came from */
    uint64_t gen;           /* the open transaction */
    uint64_t nblocks;

    uint8_t *bitmap;        /* in-memory allocation bitmap, authoritative during a transaction */
    uint8_t *bitmap_dirty;  /* one flag per bitmap chunk */
    unsigned nr_chunks;
    uint64_t free_blocks;
    uint64_t alloc_hint;

    /* The pool's members (one, for a version-2 or -3 filesystem). */
    struct cfs_memstate *mem;
    unsigned nmembers;

    uint64_t *pending_free; /* blocks freed in this transaction, reusable after commit */
    unsigned nr_pending, pending_cap;
    /* The same, for blocks no snapshot may hold: a superseded free
     * record. A snapshot's recorded bitmap marks one allocated, but no
     * snapshot's *tree* can reach it (a snapshot preserves imap_root
     * and alloc_root, never free_root), so the filter phase 7 applies
     * to pending_free would keep it for a reader that cannot exist
     * (docs/audit/next-subsystem-unmount-leak.md). */
    uint64_t *pending_exempt;
    unsigned nr_exempt, exempt_cap;
    /*
     * Inodes this mount has unlinked to zero links and not yet evicted:
     * their names are gone and their blocks are not free. Derived state,
     * written out whole by each commit rather than edited on disk, so
     * nothing here is copy-on-write (docs/audit/next-subsystem-orphan.md).
     * Usually empty, because an ordinary unlink's eviction lands in the
     * same transaction and the pair cancels here before anything is
     * written.
     */
    uint64_t *orphans;
    unsigned nr_orphans, orphan_cap;

    struct list_node bufs;  /* metadata buffer cache, MRU first */
    unsigned nr_bufs, nr_dirty;

    struct mutex lock;
    bool discard_on_unmount;
    /* Test hook: freelog_fill returns -EIO, so a commit fails after the
     * record's blocks have been reserved. What that must not leave
     * behind is the reservation (docs/audit/next-subsystem-unmount-leak.md). */
    bool test_fail_freelog;
    /* Test hook: cfs_snapshot_fill returns -EIO after its first append,
     * so a commit fails with part of the deadlist written and the whole
     * reservation outstanding (docs/audit/next-subsystem-snap-deadlist.md). */
    bool test_fail_snapfill;
    /* Test hook: orphan_fill returns -EIO, so a commit fails with the
     * record's blocks reserved (docs/audit/next-subsystem-orphan.md). */
    bool test_fail_orphan;
#if CONFIG_DEBUG
    /* Test hook: no raise to version 12; snapshots taken are version 11's. */
    bool test_no_raise;
    /* Test hook: while armed, `test_commits_left` more commits are
     * published and the next one abandons the transaction, as if the
     * machine stopped there; the unmount then drops what is open. */
    bool test_commit_armed;
    unsigned test_commits_left;
#endif
    /* Clones, and the one this operation writes (NULL: the live tree).
     * While one is entered its roots are in `sb` and the live tree's are
     * in `live_*` (cfs_clone_enter); a commit leaves it first. */
    struct cfs_clone *clones;
    unsigned nclones, clone_cap;
    struct cfs_clone *cur_clone;
    uint64_t live_imap_root, live_next_ino, live_inode_count;
    /* A rollback switch whose transaction failed was abandoned, and the
     * mount loads the current tree again without trying it (design.md,
     * "Rollback"). */
    bool rollback_off;
    bool rollback_failed;
    /* The switch's commit is in progress: the seam CONFIG_COSMOFS_STOP
     * stops there, before its root (test-snapshot's crash point). */
    bool switching;
    /* Walks of the snapshot list, since mount. The verdict used to be
     * taken twice per freed block -- once for the record, once by the
     * release loop -- and is now taken once; this is how that is
     * measured rather than argued (`cosmofs-snap-onewalk`). */
    uint64_t snap_walks;
    /* Verdicts the commit needed: one per pending free it considered.
     * `snap_walks` minus this is how many walks were spent on anything
     * other than answering a question once. */
    uint64_t snap_verdicts;
    int failed;             /* nonzero: the open transaction is abandoned, never committed */
    /* Mounted read-only (MOUNT_RDONLY): nothing reaches the device -- no
     * commit, no read repair, no rollback switch. The replay of the free
     * and orphan records still runs, in memory only, so what a reader or
     * the checker sees is what a writable mount would see (design.md,
     * "Read-only mounts"; CFS-1). */
    bool rdonly;
    unsigned replay_freed;     /* blocks the mount's free-record replay released */
    unsigned replay_orphans;   /* inodes its orphan replay released */
    /* What the mount did, for the check on an unmounted device
     * (cosmofs_check_dev) and the operator (roadmap 1.2 B2): how far the
     * load got (COSMO_FSCTL_STAGE_*), whether a pending rollback was
     * switched to or why not (CFS-20), and whether the older slot's root
     * was used because the newer one did not load. */
    unsigned load_stage;
    bool rollback_switched;
    int rollback_error;
    bool fell_back;
    uint64_t commits;
    uint64_t reserve;       /* blocks only metadata may take (design.md, "the metadata reserve") */
    uint64_t csum_failures;
    uint64_t repairs;       /* blocks written back from a good copy */
    /* Encryption (design.md, "Format version 7"). The master key is
     * unwrapped at mount and never written out in the clear; without it
     * the metadata still mounts and every read of a file's contents is
     * -ENOKEY. */
    bool encrypted;         /* the filesystem has a key block */
    bool have_key;          /* and this mount unwrapped it */
    uint8_t master_key[CHACHA20_KEY_SIZE];
    /* Version 14 (design.md, "The metadata MAC"): every header, superblock
     * and label is sealed and checked under this key when `on`. Set by
     * the superblock read, from the key table; `key_held` when this mount
     * holds the table's entry and its destruction releases it. */
    struct cfs_mac mac;
    bool key_held;
    uint64_t degraded;      /* copies the member table promised and the mount did not find */
    /* The pool's state_seq the last published root recorded: a commit is
     * due when the pool's has moved (design.md, "Format version 13"). */
    uint64_t copystate_seq;
    char pool_name[CFS_LABEL_MAX + 1];   /* the console lines' name for the pool */
    unsigned snap_count;    /* live snapshots; commit holds their blocks rather than freeing */
    uint64_t snap_max_id;   /* the highest snapshot id ever used here: ids are never reused */
    /* The writeback thread (design.md, "The writeback thread"). */
    struct thread *wb_thread;
    bool wb_stop;
    /* The resilver (cosmofs_resilver.c): one thread a mount, from the
     * first copy that needs one until the unmount. */
    struct thread *rs_thread;
    bool rs_stop;
    bool maint;                 /* a maintenance mount (MOUNT_DETACHED): no resilver, no pool commands */
    uint64_t rs_since_record;   /* blocks copied since the cursor was last made due for the record */
    uint64_t rs_started_ns;
    uint64_t rs_copied;         /* blocks this resilver copied */
    uint8_t *rs_buf;            /* the thread's block, allocated before the thread (rs_wake) */
    uint64_t rs_disagree;       /* blocks two ONLINE sources held differently (rs_step) */
    uint32_t rs_region_errors;  /* the target's write errors when its current region began (an ONLINE copy's holes) */
    bool wb_enabled;        /* test hook: autonomous commits on */
    unsigned wb_interval_ms;
    uint64_t first_dirty_ns; /* when the open transaction first became non-empty; 0 when empty */
    uint64_t wb_commits;
    bool mount_done;          /* the mount finished: before this there is no autonomous committer */
    uint64_t mount_dirty_notes; /* dirty marks the mount's own replay made */
    uint64_t wb_early;          /* how many of those found a committer already running */
};

#define CFS_WB_POLL_MS       50u
#define CFS_WB_DIRTY_BUFS    64u
#define CFS_WB_PENDING       512u
#define CFS_WB_DIRTY_PAGES   256u
#define CFS_WB_INTERVAL_MS   5000u

enum cfs_alloc_class {
    CFS_ALLOC_META,   /* may use the reserve */
    CFS_ALLOC_DATA,   /* refused at or below the reserve */
};

/* Abandon the open transaction after a mutation that could not be
 * completed consistently; commits refuse until unmount discards it. */
void cfs_fail(struct cfs *fs, int rc);

/* Per-vnode private state: the inode as last written through. */
struct cfs_vnode {
    struct cfs_inode inode;
    /* Zero for the live tree; otherwise the snapshot this vnode reads
     * through, and the tag its inode numbers carry. */
    unsigned snap_tag;
    uint64_t snap_imap_root;
    uint64_t snap_next_ino;
};

static inline struct cfs *cfs_of(struct mount *mnt) { return mnt->fs_priv; }
static inline struct cfs_inode *cfs_inode_of(struct vnode *vn) { return &((struct cfs_vnode *)vn->fs_priv)->inode; }

/* core */
int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out);
void cfs_buf_put(struct cfs *fs, struct cfs_buf *b);
struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b);
/* Make `b` writable in the open transaction (copy-on-write if it
 * belongs to a committed generation). *bp may change; the parent pointer
 * `*parent_slot` (in another writable block or the superblock) is
 * updated to the new block number. */
/*
 * Blocks a commit reserved before the bitmap fixpoint and hands out
 * after it. The fixpoint forbids allocating once it has run, and cannot
 * say before it runs what there will be to write; a reservation is how
 * both are true at once (docs/audit/next-subsystem-snap-deadlist.md).
 */
struct cfs_res {
    uint64_t *blk;
    unsigned n;      /* reserved */
    unsigned used;   /* handed out, from the front */
};
/* Take the next reserved block, or 0 when the reservation is spent. A
 * caller that takes one and then cannot use it must give it back with
 * cfs_res_untake before returning: the commit keeps the blocks the
 * reservation handed out, on the grounds that the snapshot list names
 * them, so one taken and referenced by nothing is a block the next
 * successful commit makes durable and unreachable. Only the block most
 * recently taken can be given back, which is all any caller needs. */
uint64_t cfs_res_take(struct cfs_res *r);
void cfs_res_untake(struct cfs_res *r);

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot);
/* The same copy, giving the superseded block back exempt: for a chain
 * named by a superblock field a snapshot does not preserve, which no
 * snapshot's tree can reach (the free record, the snapshot list, a
 * deadlist). `res` is where the copy's block comes from: a reservation
 * inside the commit's window, or NULL to allocate, which only a caller
 * running before the fixpoint may do. */
int cfs_buf_cow_exempt(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_res *res);
/* A fresh zeroed dirty buffer at a block the caller already owns. */
int cfs_buf_new_at(struct cfs *fs, uint32_t kind, uint64_t blk, struct cfs_buf **out);
/* Metadata blocks set aside before a change that must not stop half way
 * (CF17; CFS-6, cosmofs_core.c). The bound is the longest extent chain. */
#define CFS_SNAP_MAX_CHAIN 4096u   /* no sound snapshot list or deadlist is this long */
#define CFS_PREP_MAX (CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2)
struct cfs_prep {
    unsigned n, used;
    uint64_t blk[CFS_PREP_MAX];
    struct cfs_buf *buf[CFS_PREP_MAX];
};
int cfs_prep_meta(struct cfs *fs, struct cfs_prep *p, unsigned n);
void cfs_prep_release(struct cfs *fs, struct cfs_prep *p);
void cfs_buf_cow_prep(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_prep *p);
struct cfs_buf *cfs_buf_new_prep(struct cfs *fs, uint32_t kind, struct cfs_prep *p);
int cfs_inode_prepare(struct cfs *fs, uint64_t ino);
/* Mark block `lin` (a linear index) allocated: the offline repair of a
 * block something reaches and the bitmap calls free. */
void cfs_bitmap_claim(struct cfs *fs, uint64_t lin);
/* A set of block numbers in page-sized chunks (cosmofs_check.c): the
 * checker's maps and the scrub's record of what it verified. */
struct cfs_bits {
    uint8_t **chunk;
    unsigned nchunks;
};
int cfs_bits_alloc(struct cfs_bits *m, uint64_t bits);
void cfs_bits_free(struct cfs_bits *m);
bool cfs_bits_test(const struct cfs_bits *m, uint64_t bit);
bool cfs_bits_set(struct cfs_bits *m, uint64_t bit);   /* true if it was already set */
/* Both superblock slots of the pool on `bd`, as the mount reads them:
 * each result 0 or the reason the slot does not verify. */
int cfs_super_slots(struct blkdev *bd, struct cfs_super *a, int *ra, struct cfs_super *b, int *rb);
/* The stage a mount of `bd` failed at, and its error, if the last failed
 * mount of it was this kernel's (cosmofs_check_dev). */
bool cfs_mount_failure(const struct blkdev *bd, unsigned *stage, int *rc);
int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out);   /* fresh zeroed block, dirty */
void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b);
/* Dirty in the open transaction: in memory only (the scrub's question). */
bool cfs_buf_is_dirty(struct cfs *fs, uint64_t blkno);

/* Snapshots (cosmofs_snap.c; design.md, "Format version 3"). */
int cfs_inode_read_at(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, uint64_t ino,
                      struct cfs_inode *out);
int cfs_snapshot_create(struct cfs *fs, const char *name, bool identity, struct cfs_snapshot *out);
/* Raise an 11 pool to 12 in the open transaction; 0 at 12 or above,
 * -EOPNOTSUPP below 11. */
int cfs_snap_raise(struct cfs *fs);
int cfs_snapshot_create_ex(struct cfs *fs, const char *name, bool identity, bool commit, const uint8_t *uuid,
                           struct cfs_snapshot *out);
bool cfs_snapshot_find_uuid(struct cfs *fs, const uint8_t uuid[16], struct cfs_snapshot *out);
/* The same, telling "not there" (-ENOENT) from "the list does not read"
 * (-EIO, a cycle included), which only the first may act on. */
int cfs_snapshot_lookup_uuid(struct cfs *fs, const uint8_t uuid[16], struct cfs_snapshot *out);
int cfs_snapshot_deadlists_filter(struct cfs *fs, bool (*live)(void *arg, uint64_t dva), void *arg,
                                  uint64_t *dropped);
/*
 * Rollback (cosmofs_rollback.c; design.md, "Rollback"). The record is
 * read and checked by cfs_rollback_read (-ENOENT: none; -EIO: it does not
 * check out). A request writes a PENDING record and a safety snapshot in
 * one transaction; the mount's switch applies it and leaves a HELD
 * record; BOOT_OK clears that.
 */
int cfs_rollback_read(struct cfs *fs, struct cfs_rollback *out);
/* The record's bytes alone: seal sets magic, version and crc; parse
 * copies a block's payload out and checks all three and the state
 * (-EIO when any is wrong). */
void cfs_rollback_seal(struct cfs_rollback *r);
int cfs_rollback_parse(const void *payload, struct cfs_rollback *out);
int cfs_rollback_request(struct cfs *fs, const char *target, const char *safety_name, struct cfs_snapshot *safety);
int cfs_rollback_cancel(struct cfs *fs);
int cfs_rollback_drop(struct cfs *fs);
int cfs_rollback_boot_ok(struct cfs *fs, bool *released);
bool cfs_rollback_protects(struct cfs *fs, const struct cfs_snapshot *s);
int cfs_rollback_at_mount(struct cfs *fs, bool *switched);
/*
 * Clones (cosmofs_clone.c; design.md, "Clones"). Under fs->lock: enter
 * swaps the clone's roots into the in-memory superblock, so every inode
 * and allocation path writes the clone; leave swaps them back and keeps
 * what moved. A free while one is entered keeps a block the origin's tree
 * reaches and frees any other outright (cfs_clone_free).
 */
struct cfs_clone *cfs_clone_by_id(struct cfs *fs, uint64_t id);
void cfs_clone_enter(struct cfs *fs, struct cfs_clone *c);
void cfs_clone_leave(struct cfs *fs);
void cfs_clone_free(struct cfs *fs, uint64_t dva);
int cfs_clones_load(struct cfs *fs);
void cfs_clones_free(struct cfs *fs);
int cfs_clones_publish(struct cfs *fs);
int cfs_clone_create(struct cfs *fs, const char *origin, const char *name, struct cfs_snapshot *out);
int cfs_clone_ready(struct cfs *fs, uint64_t id);
int cfs_clone_delete(struct cfs *fs, const struct cfs_snapshot *doomed, uint64_t *freed);
bool cfs_snapshot_has_clones(struct cfs *fs, uint64_t id);
int cfs_snapshot_list_cow(struct cfs *fs);
int cfs_snapshot_walk_write(struct cfs *fs, bool (*fn)(struct cfs_snapshot *s, void *arg), void *arg);
int cfs_snapshot_add_clone(struct cfs *fs, const struct cfs_snapshot *origin, const char *name,
                           struct cfs_snapshot *out);
/* The roots a tree has now: a clone's from the table (the open
 * transaction), a snapshot's from its entry. */
void cfs_tree_roots(struct cfs *fs, const struct cfs_snapshot *s, uint64_t *imap_root, uint64_t *next_ino);
int cfs_snapshot_delete(struct cfs *fs, const char *name);
int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count);
bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out);
bool cfs_has_snapshots(struct cfs *fs);
/* The list's two layouts (version 11 and 12) behind one interface: the
 * block kind and slot count for this filesystem's version, a block's
 * `next`, and entry `i` read into / written from the in-memory struct
 * (design.md, "Format version 12"). */
/* The checker's repair for a version-12 list's identity faults. */
int cfs_snapshot_fix_identity(struct cfs *fs, unsigned *fixed);
/* A uuid no snapshot on the list, and none of `also`, has (CF18). */
void cfs_snapshot_new_uuid(struct cfs *fs, uint8_t out[16], const uint8_t (*also)[16], unsigned nalso);
uint32_t cfs_snaplist_kind(const struct cfs *fs);
unsigned cfs_snaplist_slots(const struct cfs *fs);
uint64_t cfs_snaplist_next(const struct cfs_buf *b);
bool cfs_snaplist_get(const struct cfs *fs, const struct cfs_buf *b, unsigned i, struct cfs_snapshot *out);
void cfs_snaplist_set(const struct cfs *fs, struct cfs_buf *b, unsigned i, const struct cfs_snapshot *s);
/*
 * Every block one tree reaches (cosmofs_reach.c): its inode map, its
 * inode blocks, and each inode's extents, extent chain and checksum
 * tree. `mark` is called once per block per walk, with the block's
 * linear index; an unreadable block makes the walk return -EIO after
 * finishing what it could reach. `next_ino` bounds the inode numbers the
 * tree may hold, which is the tree's own and not the live one's.
 */
int cfs_tree_walk(struct cfs *fs, uint64_t imap_root, uint64_t next_ino,
                  void (*mark)(void *arg, uint64_t lin), void *arg);
/* The allocation maps a root names (a member table, or before version 4
 * an index) and, for a snapshot, its deadlist: what the listing's sizes
 * add to a tree's walk (cosmofs_snap.c). */
int cfs_alloc_walk(struct cfs *fs, uint64_t root, void (*mark)(void *arg, uint64_t lin), void *arg);
int cfs_snapshot_walk_held(struct cfs *fs, const struct cfs_snapshot *s, void (*mark)(void *arg, uint64_t lin),
                           void *arg);
/* Commit-time: hold `blk` for the newest snapshot instead of freeing it.
 * False when no snapshot exists and the caller should free it. */

/* Test hook: entries on every snapshot's deadlist -- all of them when
 * `of` is 0, or the ones naming that block. */
uint64_t cfs_snapshot_deadlist_len(struct cfs *fs, uint64_t of);
uint64_t cfs_snapshot_deadlist_dups(struct cfs *fs, uint64_t *examined, uint64_t *first);

/* What the commit's deadlist work may need from the reservation, and
 * the exempt frees it may add -- which are entries in the record.
 * Both are zero on a filesystem with no snapshot. */
unsigned cfs_snapshot_reserve_bound(struct cfs *fs, unsigned pending_bound);
unsigned cfs_snapshot_exempt_bound(struct cfs *fs);
/*
 * The commit's append, run *before* the root with blocks from `res`.
 * `held[i]` comes back true for every `pending_free[i]` a snapshot still
 * names, which is the verdict the record and the release loop both use:
 * one walk of the list per block, not one each. `*appended` is how many
 * entries it added: a commit that fails after any cannot be retried.
 */
int cfs_snapshot_fill(struct cfs *fs, struct cfs_res *res, bool *held, unsigned n, unsigned *appended);
/* Does this snapshot's tree still occupy `blk`? One lookup in the
 * allocation bitmap the snapshot recorded. */
bool cfs_snapshot_references(struct cfs *fs, const struct cfs_snapshot *s, uint64_t blk);
/* Does any snapshot still name this block? The question alone, with no
 * deadlist append: cfs_snapshot_fill is the one caller that appends, and
 * it does so before the root. Kept for callers that only want to ask. */
bool cfs_snapshot_holds(struct cfs *fs, uint64_t blk);
/* Members and DVAs (cosmofs_member.c; design.md, "Format version 4"). */
bool cfs_dva_valid(const struct cfs *fs, uint64_t dva);
uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva);   /* CFS_DVA_NONE if the DVA is not ours */
/* Is the linear block allocated, per the in-memory bitmap that is
 * authoritative during a transaction? For the structural check. */
bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin);
/* One directory block by its inode rather than its vnode: the VFS path
 * has a vnode, the structural check has only the inode it read. */
int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *buf);
int cfs_dir_write_block_at(struct cfs *fs, struct cfs_inode *dir, uint64_t lblk, const uint8_t *buf);
uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin);   /* CFS_DVA_NONE for padding or past the end */
/* Fill fs->mem from the superblock: the member table for version 4, a
 * single synthesised member for versions 2 and 3. Assembles the other
 * members' devices by label. */
int cfs_members_load(struct cfs *fs);
/* Write fs->mem back into the (copy-on-write) member table; the version
 * decides whether member 0's root lives there or in the superblock. */
int cfs_members_store(struct cfs *fs);
void cfs_members_free(struct cfs *fs);
/* Format-time: the label that lets a mount find this member again. */
int cfs_label_write(struct spool *pool, const struct cfs_mac *mac, unsigned vdev, unsigned copy, uint64_t generation,
                    const uint8_t uuid[16], uint64_t nblocks);
/* Stamp every attached copy's label with this generation, just before
 * the root that generation publishes. */
int cfs_labels_update(struct cfs *fs);
/* Copies the member table promises that are not ONLINE with a device. */
unsigned cfs_copies_out(struct cfs *fs);
/* The commit's two steps for the copy state (design.md, "The record,
 * written by the commit"): before anything is reserved, the state blocks
 * each degraded member needs (and the raise to 13); just before the dirty
 * metadata is written, their contents. */
int cfs_copystate_prepare(struct cfs *fs);
/* The scrub's and the resilver's bound (fs.scrub_rate, fs.resilver_rate):
 * `used` counts blocks; cfs_pace lets fs->lock go for the rest of the
 * window once a window's share is spent, and says whether it did. */
struct cfs_pace {
    unsigned quota;          /* blocks a window; 0 = unpaced */
    uint64_t window_ns;
    unsigned used;
    uint64_t window;         /* when this window began */
};
void cfs_pace_init(struct cfs_pace *p, unsigned rate);
bool cfs_pace(struct cfs *fs, struct cfs_pace *p);
int cfs_copystate_fill(struct cfs *fs);
bool cfs_copystate_recorded(struct cfs *fs);
/* The pool's other devices held by the mount (CFS-5). */
void cfs_holds_sync(struct cfs *fs);
int cfs_holds_sync_with(struct cfs *fs, struct blkdev *extra);
/* Labels on devices that are not (yet) copies: resilver targets and
 * spares (design.md, "Resilvering", "Spares"). */
int cfs_label_write_dev(struct cfs *fs, struct blkdev *bd, unsigned vdev, uint64_t flags, uint64_t tag,
                        uint64_t nblocks);
void cfs_label_clear_dev(struct blkdev *bd);
struct blkdev *cfs_find_tagged(struct cfs *fs, uint64_t flags, uint64_t tag);
void cfs_spares_find(struct cfs *fs);
struct blkdev *cfs_find_lagging(struct cfs *fs, unsigned vdev, uint64_t since, bool *exact);
/* The resilver (cosmofs_resilver.c): started after a mount, stopped
 * before the filesystem is torn down; the holds (CFS-5) set again. */
void cfs_resilver_mount(struct cfs *fs);
void cfs_resilver_stop(struct cfs *fs);
int cosmofs_poolctl(struct mount *mnt, struct cosmofs_pool_req *req);
int cosmofs_commit_mount(struct mount *mnt);
/* The newest member-0 device of the pool `uuid` names, if it is newer
 * than `gen` and is not `bd`: referenced, or NULL (CF27). */
struct blkdev *cfs_newer_member0(struct blkdev *bd, const uint8_t uuid[16], uint64_t gen, uint64_t *newer);
/* Read `dva` into `buf` and check it with `verify`, trying the member's
 * copies in turn and writing the first good one back over the copies
 * that failed. -EIO when no copy satisfies `verify`. */
/* `read_ok` (optional) describes the bytes left in `buf`, not the call's
 * history: true when they came from a copy that read cleanly and the
 * verifier then rejected them, false when the last copy attempted failed
 * to read and what is in the buffer is whatever it managed to transfer.
 * Both return -EIO, and only the first is worth describing. */
int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired, bool *read_ok);
/* Read *every* copy and check each one, writing a good copy back over
 * the bad ones. This is what a scrub needs and a read does not: a read
 * stops at the first copy that verifies, so rot on any other copy stays
 * invisible until that copy is the one answering. `buf` comes back
 * holding a good copy when there was one. -EIO when there was none. */
int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   bool repair, unsigned *bad, unsigned *repaired);
/* Why a metadata block did not verify. MHDR_ALGO is a header whose
 * csum_algo this build cannot verify -- a format or algorithm skew, distinct
 * from MHDR_CRC (a checksum that did not match). Order is the check order, so
 * the algorithm is resolved before the checksum it names. */
enum mhdr_fault { MHDR_OK = 0, MHDR_MAGIC, MHDR_BLKNO, MHDR_KIND, MHDR_ALGO, MHDR_CRC, MHDR_MAC };
/* `mac` NULL or off: CRC32C, as before version 14. */
enum mhdr_fault cfs_mhdr_fault_of(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind);
/* A metadata block's own check: kind, its own DVA, and its CRC or tag. */
bool cfs_mhdr_ok(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind);
/* Seal a metadata block outside a transaction (format time). */
void cfs_mhdr_seal_raw(const struct cfs_mac *mac, void *block, uint32_t kind, uint64_t dva, uint64_t generation);
/* The version-14 tag: domain CFS_MAC_*, the 8 bytes at `off` taken as zero. */
void cfs_mac_tag(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off, uint8_t tag[CFS_MAC_SIZE]);
bool cfs_mac_ok(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off);
void cfs_mac_seal(const struct cfs_mac *m, char domain, uint8_t *block, size_t off);

/* Encryption (cosmofs_crypt.c; design.md, "Format version 7"). */
void cfs_file_key(const struct cfs *fs, uint64_t ino, uint8_t out[CHACHA20_KEY_SIZE]);
void cfs_block_nonce(uint8_t nonce[CHACHA20_NONCE_SIZE]);
/* Where a block belongs, authenticated with it but never stored: the
 * reader supplies what it believes the position to be, so a block moved
 * to another offset of the same file fails its tag. */
struct cfs_block_aad {
    uint64_t ino;
    uint64_t lblk;
};
int cfs_keys_write(struct spool *pool, uint64_t dva, uint64_t generation, const uint8_t master[CHACHA20_KEY_SIZE],
                   const void *user_key, size_t user_len);
int cfs_keys_unwrap(const struct cfs_keys *k, const void *user_key, size_t user_len,
                    uint8_t master[CHACHA20_KEY_SIZE]);
int cfs_need_seeded(const char *what);
int cfs_keys_load(struct cfs *fs, const void *user_key, size_t user_len);
int cfs_keys_rotate(struct cfs *fs, const void *new_key, size_t new_len);
/* A superblock's own check: magic, a version this kernel reads, and its
 * CRC or tag. A zeroed slot is not a superblock and not an error. */
bool cfs_super_ok(const void *block);
/* The same, saying why: -ENOKEY for a version-14 encrypted pool whose key
 * is not loaded, -EIO otherwise; `mac` (may be NULL) gets the MAC key. */
int cfs_super_verify(const void *block, struct cfs_mac *mac);
/* The newest slot, unverified when it is a version-14 encrypted pool's:
 * for the key operations alone. */
int cfs_super_peek(struct spool *p, struct cfs_super *out);

/* What an encrypted format is given (design.md, "Making an encrypted pool"). */
struct cfs_format_keys {
    uint8_t master[CHACHA20_KEY_SIZE];
    struct cfs_keyslot slot[CFS_KEY_SLOTS];
};
int cosmofs_format_keyed(struct blkdev **bd, unsigned n, const char *label, const struct cfs_format_keys *fk);

/* Version 14 (cosmofs_crypt.c; design.md, "The key hierarchy", "The key handle"). */
void cfs_mac_derive(const uint8_t master[CHACHA20_KEY_SIZE], struct cfs_mac *out);
bool cfs_keyslot_valid(unsigned index, const struct cfs_keyslot *s);
int cfs_keys2_write(struct spool *pool, const struct cfs_mac *mac, uint64_t dva, uint64_t generation,
                    const uint8_t uuid[16], const struct cfs_keyslot slots[CFS_KEY_SLOTS]);
int cfs_keytab_mac(const uint8_t uuid[16], struct cfs_mac *mac);
int cfs_keytab_take(const uint8_t uuid[16], bool hold, uint8_t master[CHACHA20_KEY_SIZE]);
void cfs_keytab_release(const uint8_t uuid[16]);
int cfs_keytab_install(const uint8_t uuid[16], const uint8_t master[CHACHA20_KEY_SIZE]);
int cfs_keytab_unload(const uint8_t uuid[16]);
bool cfs_keytab_has(const uint8_t uuid[16]);

int cfs_alloc_block(struct cfs *fs, uint64_t *out);                    /* one metadata block */
/* Up to `want` consecutive data blocks at or after `hint` (0: the hint of
 * the last allocation); at least one. -ENOSPC when only the reserve is left. */
int cfs_alloc_data(struct cfs *fs, uint64_t hint, uint32_t want, uint64_t *start, uint64_t *got);
int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got);
void cfs_free_block_deferred(struct cfs *fs, uint64_t blk);
void cfs_free_block_exempt(struct cfs *fs, uint64_t blk);

/*
 * This inode's last name is gone and its blocks are not free yet; and
 * the eviction that frees them has happened. The add is in the unlink
 * and rename paths, the remove is in the evict path, and a commit
 * between them writes a record naming it -- which the next commit
 * retires (docs/audit/next-subsystem-orphan.md).
 */
bool cfs_orphan_reserve(struct cfs *fs);
void cfs_orphan_add(struct cfs *fs, uint64_t ino);
void cfs_orphan_remove(struct cfs *fs, uint64_t ino);
bool cfs_orphan_named(const struct cfs *fs, uint64_t ino);
int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out);
/* The slot as it is, even with no links: for the structural check. */
int cfs_inode_read_raw(struct cfs *fs, uint64_t ino, struct cfs_inode *out);
int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in);
int cfs_inode_alloc(struct cfs *fs, uint64_t *ino);
/* Undo the most recent cfs_inode_alloc, under fs->lock. */
void cfs_inode_discard(struct cfs *fs, uint64_t ino);
int cfs_commit(struct cfs *fs);
int cfs_sync_vnodes(struct cfs *fs);
int cfs_data_read(struct cfs *fs, uint64_t blk, void *buf);
int cfs_data_write(struct cfs *fs, uint64_t blk, const void *buf);

/* vnodes and extents (cosmofs.c) */
int cfs_vnode_get(struct cfs *fs, uint64_t ino, struct vnode **out);
int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk);
/* The run covering `lblk`, whether it is a plain run or a compressed
 * record: 1 with the extent, 0 for a hole, or an error. */
int cfs_map_ext(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, struct cfs_extent *out);
/* Read a compressed record and decompress it into `out`, which must hold
 * cfs_ext_count(e) blocks. Every physical block is verified and repaired
 * on the way, as any other read is. */
int cfs_record_read(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *e, uint8_t *out);
int cfs_set_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t pblk, uint64_t *old);
int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep_blocks);
/* The per-inode checksum tree (cosmofs.c). */
int cfs_csum_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t crc);
int cfs_csum_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t *crc);   /* -ENOENT: none stored */
int cfs_csum_verify(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *block);
int cfs_aead_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const struct cfs_csum_aead *e);
int cfs_aead_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, struct cfs_csum_aead *e);
/* Read a data or directory block and check it against the checksum its
 * inode records, taking another copy of a mirrored member when the
 * first does not match and repairing what did not. */
int cfs_data_read_verified(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf);
/* The same block, every copy of it: what a scrub reads. */
int cfs_data_scrub_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf,
                         bool repair, unsigned *bad, unsigned *repaired);
void cfs_csum_free(struct cfs *fs, struct cfs_inode *in);

#endif /* COSMOFS_INTERNAL_H */
