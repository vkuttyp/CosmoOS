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
    uint8_t uuid[16];
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

    struct list_node bufs;  /* metadata buffer cache, MRU first */
    unsigned nr_bufs, nr_dirty;

    struct mutex lock;
    bool discard_on_unmount;
    int failed;             /* nonzero: the open transaction is abandoned, never committed */
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
    uint64_t degraded;      /* copies the member table promised and the mount did not find */
    unsigned snap_count;    /* live snapshots; commit holds their blocks rather than freeing */
    uint64_t snap_max_id;   /* the highest snapshot id ever used here: ids are never reused */
    /* The writeback thread (design.md, "The writeback thread"). */
    struct thread *wb_thread;
    bool wb_stop;
    bool wb_enabled;        /* test hook: autonomous commits on */
    unsigned wb_interval_ms;
    uint64_t first_dirty_ns; /* when the open transaction first became non-empty; 0 when empty */
    uint64_t wb_commits;
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
int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot);
int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out);   /* fresh zeroed block, dirty */
void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b);

/* Snapshots (cosmofs_snap.c; design.md, "Format version 3"). */
int cfs_inode_read_at(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, uint64_t ino,
                      struct cfs_inode *out);
int cfs_snapshot_create(struct cfs *fs, const char *name);
int cfs_snapshot_delete(struct cfs *fs, const char *name);
int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count);
bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out);
bool cfs_has_snapshots(struct cfs *fs);
/* Commit-time: hold `blk` for the newest snapshot instead of freeing it.
 * False when no snapshot exists and the caller should free it. */
bool cfs_snapshot_hold_block(struct cfs *fs, uint64_t blk);
/* Does this snapshot's tree still occupy `blk`? One lookup in the
 * allocation bitmap the snapshot recorded. */
bool cfs_snapshot_references(struct cfs *fs, const struct cfs_snapshot *s, uint64_t blk);
/* Members and DVAs (cosmofs_member.c; design.md, "Format version 4"). */
bool cfs_dva_valid(const struct cfs *fs, uint64_t dva);
uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva);   /* CFS_DVA_NONE if the DVA is not ours */
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
int cfs_label_write(struct spool *pool, unsigned vdev, unsigned copy, uint64_t generation, const uint8_t uuid[16],
                    uint64_t nblocks);
/* Stamp every attached copy's label with this generation, just before
 * the root that generation publishes. */
int cfs_labels_update(struct cfs *fs);
/* Read `dva` into `buf` and check it with `verify`, trying the member's
 * copies in turn and writing the first good one back over the copies
 * that failed. -EIO when no copy satisfies `verify`. */
int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired);
/* Read *every* copy and check each one, writing a good copy back over
 * the bad ones. This is what a scrub needs and a read does not: a read
 * stops at the first copy that verifies, so rot on any other copy stays
 * invisible until that copy is the one answering. `buf` comes back
 * holding a good copy when there was one. -EIO when there was none. */
int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   unsigned *repaired);
/* A metadata block's own check: kind, its own DVA, and the CRC. */
bool cfs_mhdr_ok(const void *block, uint64_t dva, uint32_t kind);
/* Seal a metadata block outside a transaction (format time). */
void cfs_mhdr_seal_raw(void *block, uint32_t kind, uint64_t dva, uint64_t generation);

/* Encryption (cosmofs_crypt.c; design.md, "Format version 7"). */
void cfs_file_key(const struct cfs *fs, uint64_t ino, uint8_t out[CHACHA20_KEY_SIZE]);
void cfs_block_nonce(uint8_t nonce[CHACHA20_NONCE_SIZE]);
int cfs_keys_write(struct spool *pool, uint64_t dva, uint64_t generation, const uint8_t master[CHACHA20_KEY_SIZE],
                   const void *user_key, size_t user_len);
int cfs_keys_unwrap(const struct cfs_keys *k, const void *user_key, size_t user_len,
                    uint8_t master[CHACHA20_KEY_SIZE]);
int cfs_keys_load(struct cfs *fs, const void *user_key, size_t user_len);
int cfs_keys_rotate(struct cfs *fs, const void *new_key, size_t new_len);
/* A superblock's own check: magic, a version this kernel reads, and the
 * CRC. A zeroed slot is not a superblock and not an error. */
bool cfs_super_ok(const void *block);

int cfs_alloc_block(struct cfs *fs, uint64_t *out);                    /* one metadata block */
/* Up to `want` consecutive data blocks at or after `hint` (0: the hint of
 * the last allocation); at least one. -ENOSPC when only the reserve is left. */
int cfs_alloc_data(struct cfs *fs, uint64_t hint, uint32_t want, uint64_t *start, uint64_t *got);
int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got);
void cfs_free_block_deferred(struct cfs *fs, uint64_t blk);
int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out);
int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in);
int cfs_inode_alloc(struct cfs *fs, uint64_t *ino);
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
                         unsigned *repaired);
void cfs_csum_free(struct cfs *fs, struct cfs_inode *in);

#endif /* COSMOFS_INTERNAL_H */
