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
};
int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out);

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
/* Test hook: free blocks on one member, or UINT64_MAX past the last. */
uint64_t cosmofs_test_member_free(struct mount *mnt, unsigned vdev);

#endif /* KERNEL_COSMOFS_H */
