/*
 * cosmofs_internal.h - In-memory state shared by the cosmofs sources.
 */

#ifndef COSMOFS_INTERNAL_H
#define COSMOFS_INTERNAL_H

#include <kernel/list.h>
#include <kernel/mutex.h>
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

    uint64_t *pending_free; /* blocks freed in this transaction, reusable after commit */
    unsigned nr_pending, pending_cap;

    struct list_node bufs;  /* metadata buffer cache, MRU first */
    unsigned nr_bufs, nr_dirty;

    struct mutex lock;
    bool discard_on_unmount;
    uint64_t commits;
};

/* Per-vnode private state: the inode as last written through. */
struct cfs_vnode {
    struct cfs_inode inode;
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
int cfs_alloc_block(struct cfs *fs, uint64_t *out);
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
int cfs_set_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t pblk, uint64_t *old);
int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep_blocks);

#endif /* COSMOFS_INTERNAL_H */
