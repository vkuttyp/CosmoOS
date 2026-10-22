/*
 * storage.h - The storage pool: block addressing over member devices.
 *
 * A filesystem addresses pool blocks (4 KiB), never a block device. A
 * block is named by a DVA: which member holds it, and where on that
 * member. Member 0 alone is the single-device pool every earlier format
 * assumed, which is why a version-2 or -3 filesystem needs no
 * conversion (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 4: many members").
 *
 * The pool knows nothing about labels or filesystems: it is an ordered
 * set of devices with a common block size. Deciding *which* devices
 * belong together is the filesystem's, because the identity is written
 * in the filesystem's own format.
 */

#ifndef KERNEL_STORAGE_H
#define KERNEL_STORAGE_H

#include <kernel/types.h>

#define POOL_BLOCK 4096u
#define POOL_MAX_MEMBERS 255u   /* the DVA's vdev field is 8 bits, 255 reserved */
#define POOL_MAX_COPIES 4u      /* devices in one mirror group */

/* bits 63:56 name the member, bits 55:0 the block within it. */
#define POOL_DVA_SHIFT 56
#define POOL_DVA_BLK_MASK ((1ull << POOL_DVA_SHIFT) - 1)
#define POOL_DVA(vdev, blk) (((uint64_t)(vdev) << POOL_DVA_SHIFT) | ((uint64_t)(blk) & POOL_DVA_BLK_MASK))
#define POOL_DVA_VDEV(d) ((unsigned)((uint64_t)(d) >> POOL_DVA_SHIFT))
#define POOL_DVA_BLK(d) ((uint64_t)(d) & POOL_DVA_BLK_MASK)

struct blkdev;

/*
 * A copy's state (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 13"). Read from ONLINE copies only; written to ONLINE
 * and RESILVERING ones. The values are the on-disk record's (CFS_COPY_*).
 */
enum pool_copy_state {
    POOL_COPY_ONLINE = 0,
    POOL_COPY_MISSING = 1,       /* the device is gone, or was not found at import */
    POOL_COPY_OFFLINE = 2,       /* taken out by the operator */
    POOL_COPY_FAULTED = 3,       /* failed more than fs.fault_errors times */
    POOL_COPY_RESILVERING = 4,   /* being rebuilt: written, not read */
    POOL_COPY_NSTATES
};

/* What a copy missed, one bit a region of `region_blocks` blocks. The
 * cosmofs record (struct cfs_copy_rec) holds exactly these bytes. */
#define POOL_DIRTY_BYTES 960u
#define POOL_DIRTY_BITS (POOL_DIRTY_BYTES * 8u)

struct pool_copy {
    struct blkdev *dev;          /* referenced; NULL when the copy has no device */
    unsigned sectors_per_block;
    enum pool_copy_state state;
    bool whole;                  /* the bitmap does not cover what it missed: rebuild all */
    uint64_t since;              /* the last root it holds whole (pool_commit_done's) */
    uint64_t cursor;             /* resilver progress, in blocks */
    uint32_t read_errors, write_errors, csum_errors;
    uint8_t *dirty;              /* POOL_DIRTY_BYTES; allocated with the member's second slot */
};

/* A member is a mirror group: `ncopies` copies holding the same blocks
 * at the same offsets. The DVA names the group, so nothing above the
 * pool changes when a group gains a copy
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 5").
 * A copy whose device is gone keeps its slot: the slot is what the
 * record of what it missed belongs to. */
struct spool_member {
    struct pool_copy copy[POOL_MAX_COPIES];
    unsigned ncopies;
    uint64_t nblocks;            /* the group's, which every copy must cover */
    uint32_t region_blocks;      /* blocks per dirty bit */
    /* Regions written since the last durable root: what a copy leaving
     * ONLINE now may hold only in its cache. POOL_DIRTY_BYTES, allocated
     * with the second slot: a member of one copy has nobody to tell. */
    uint8_t *recent;
};

struct spool {
    struct spool_member *m;      /* nmembers of them, grown by pool_add_member */
    unsigned nmembers, cap;
    uint32_t block_size;         /* POOL_BLOCK */
    uint64_t nblocks;            /* member 0's, the only one a v2/v3 filesystem knows */
    uint64_t reads, writes, flushes;
    uint64_t epoch;              /* the last durable root (pool_commit_done) */
    uint64_t state_seq;          /* advances at every copy state change */
    const char *name;            /* for the console lines: the filesystem's, or NULL */
    /* Called after every copy state change, with the caller's lock held
     * as for the I/O that caused it. */
    void (*notify)(void *arg);
    void *notify_arg;
};

/* sysctl fs.fault_errors: failures after which a copy is FAULTED (0: never). */
extern uint32_t g_pool_fault_errors;

/* Take a reference on bd and describe it as a one-member pool. -EINVAL
 * if the device's sector size does not divide the pool block. Sleeps. */
int pool_open(struct blkdev *bd, struct spool **out);
/* Append a member of one device; its index is the returned vdev number.
 * The pool takes a reference. -EINVAL for a bad sector size, -ENOSPC
 * past POOL_MAX_MEMBERS. Sleeps. */
int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev);
/* Add a copy to an existing member. The device must cover the group's
 * blocks. -EINVAL for a bad sector size or too small a device, -ENOSPC
 * past POOL_MAX_COPIES. Sleeps. */
int pool_add_copy(struct spool *p, unsigned vdev, struct blkdev *bd);
/* Copy slots of the member `dva` names (present or not), or 0 when it
 * names none. */
unsigned pool_copies(const struct spool *p, uint64_t dva);
/* True if copy `c` of the member `dva` names may be read (ONLINE). */
bool pool_copy_readable(const struct spool *p, uint64_t dva, unsigned c);
/* True if the member `dva` names has a copy that is not ONLINE. */
bool pool_member_degraded(const struct spool *p, unsigned vdev);
bool pool_degraded(const struct spool *p);
/* Give member `vdev` `n` copy slots; slots past the attached ones are
 * MISSING (whole) until a device fills them. -EINVAL past POOL_MAX_COPIES. */
int pool_set_copies(struct spool *p, unsigned vdev, unsigned n);
/* Put `bd` in slot `c` of member `vdev` in `state`; the pool takes a
 * reference. The device must cover the group's blocks. */
int pool_attach_copy(struct spool *p, unsigned vdev, unsigned c, struct blkdev *bd, enum pool_copy_state state);
/* Swap two copy slots of a member, records and all: the records are
 * interchangeable, and import puts them in the order it needs. */
void pool_swap_copies(struct spool *p, unsigned vdev, unsigned a, unsigned b);
/*
 * Move copy `c` to `state` (MISSING, OFFLINE, FAULTED or ONLINE), with
 * the bookkeeping a transition needs: a copy leaving ONLINE starts its
 * bitmap from the regions written since the last root and records
 * `since`. Prints one console line. -EBUSY to take the last ONLINE copy
 * of a member OFFLINE; -EAGAIN to bring a copy ONLINE that missed writes
 * (it needs a resilver).
 */
int pool_copy_set_state(struct spool *p, unsigned vdev, unsigned c, enum pool_copy_state state, int why);
/* A checksum mismatch the caller's verifier found on copy `c`: counted,
 * not towards FAULTED (read repair rewrites it). */
void pool_copy_csum_error(struct spool *p, uint64_t dva, unsigned c);
/* The root of generation `gen` is durable on every writable copy. */
void pool_commit_done(struct spool *p, uint64_t gen);
/* Set the region of `dva` in every copy of its member that is not
 * ONLINE: a write about to happen that they will miss. */
void pool_mark_missed(struct spool *p, uint64_t dva);
/* The region a block falls in, and whether copy `c` missed it. */
bool pool_copy_missed(const struct spool *p, uint64_t dva, unsigned c);
const char *pool_copy_state_name(enum pool_copy_state s);
void pool_close(struct spool *p);

/* One pool block, addressed by DVA; buf is DMA-able (kmalloc/dma_alloc).
 * Sleep. -EINVAL for an unknown member or past that member's end.
 *
 * A read takes the first ONLINE copy that answers; a mirror is only
 * worth having if what it returns is checked, and the check belongs to
 * the caller who knows the block's format, so a caller that cares reads
 * the other copies itself with pool_read_copy (-ENODEV for a copy that
 * is not ONLINE) and repairs with pool_write_copy. A write goes to every
 * writable copy, records the block's region for each copy that did not
 * take it, and succeeds when an ONLINE copy did (design.md, "Degraded
 * operation"). The pool's state is the caller's to serialise (cosmofs:
 * cfs->lock). */
int pool_read(struct spool *p, uint64_t dva, void *buf);
int pool_write(struct spool *p, uint64_t dva, const void *buf);
int pool_read_copy(struct spool *p, uint64_t dva, unsigned copy, void *buf);
int pool_write_copy(struct spool *p, uint64_t dva, unsigned copy, const void *buf);
/* The same with bio flags (BIO_PREFLUSH, BIO_FUA): the commit's root write. */
int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags);
/* Flush every writable copy of every member: a commit is durable only
 * when all of them are. A copy whose flush fails has missed the commit
 * and is treated as a failed write; the flush fails only when a member
 * has no ONLINE copy left. */
int pool_flush(struct spool *p);

#endif /* KERNEL_STORAGE_H */
