/*
 * part.h - Partitions as block devices (docs/kernel/device/design.md,
 * "Partitions").
 *
 * A scan reads a disk's GUID Partition Table (gpt.h) and registers each
 * partition as a block device of its own, named after the disk: vda ->
 * vda1, nvme0n1 -> nvme0n1p1 (a "p" when the disk's name ends in a digit).
 * A partition device forwards every bio to its disk at the partition's
 * offset; the block layer's own range check against the partition's
 * capacity is what keeps a bio inside the partition, and the forwarding
 * checks it again before it translates (invariant D16).
 *
 * Scans happen at two points only: boot composition, once, for every disk
 * registered by then (blk_part_scan_all), and on request (blk_part_scan,
 * reached from user space through /dev/blkctl). Removing a disk removes
 * its partitions first. Nothing here writes to a disk.
 */

#ifndef KERNEL_PART_H
#define KERNEL_PART_H

#include <kernel/blk.h>

/* Called by blk_init. */
void blk_part_init(void);

/*
 * (Re)read `disk`'s partition table and register its partitions, after
 * removing the ones a previous scan registered. -EBUSY, and nothing
 * changes, when any of those is in use (anything beyond the registry and
 * the scan holds a reference: a mount, a blk_find caller). -EINVAL for a
 * partition, or a table that is refused (logged with the reason, and the
 * disk is left with no partitions); -EIO when the disk cannot be read.
 * 0 otherwise, with *nparts (if not NULL) the number registered, 0 for a
 * disk without a table. Sleeps.
 */
int blk_part_scan(struct blkdev *disk, unsigned *nparts);

/* Boot composition: scan every disk registered so far, logging. */
void blk_part_scan_all(void);

/* blk_unregister's first step for a disk: unregister its partitions and
 * wait for the bios they forwarded to complete. A no-op for a disk with
 * none, and for a partition. Sleeps. */
void blk_part_remove(struct blkdev *disk);

bool blk_is_partition(const struct blkdev *bd);

struct blk_part_info {
    char disk[BLKDEV_NAME_MAX];
    uint32_t index;          /* the partition's number in the table, from 1 */
    uint64_t start;          /* first sector on the disk */
    uint8_t uuid[16];        /* on-disk byte order (gpt.h) */
    uint8_t type[16];
};

/* False when `bd` is not a partition. */
bool blk_part_info(const struct blkdev *bd, struct blk_part_info *out);

/* The registered partition whose unique GUID is `uuid`, referenced, or
 * NULL. Sleeps. */
struct blkdev *blk_find_partuuid(const uint8_t uuid[16]);

#endif /* KERNEL_PART_H */
