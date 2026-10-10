/*
 * blkctl.h - The /dev/blkctl control ABI (docs/kernel-services/vfs/design.md,
 * "The block-device channel"; roadmap M2).
 *
 * A privileged operator lists the block devices, reads and writes their
 * sectors, flushes them, has a disk's partition table re-read, and makes
 * a filesystem on one. It exists for the installer: everything it needs
 * from the kernel to put a system on a blank disk, and nothing that
 * interprets what is written.
 *
 * As with /dev/fsctl, a command is one struct written whole -- a WRITE is
 * the struct immediately followed by its data, in the same write -- and
 * its result is read back from the same open file, whole or not at all.
 * A command that fails leaves no result.
 */
#ifndef UAPI_COSMO_BLKCTL_H
#define UAPI_COSMO_BLKCTL_H

#include <stdint.h>

#define COSMO_BLKCTL_VERSION 1

#define COSMO_BLKCTL_LIST   1   /* every registered block device: struct cosmo_blkctl_dev[] */
#define COSMO_BLKCTL_READ   2   /* `count` sectors from `sector`: the bytes */
#define COSMO_BLKCTL_WRITE  3   /* `count` sectors at `sector`, the data after the struct */
#define COSMO_BLKCTL_FLUSH  4   /* the device's volatile cache to stable media */
#define COSMO_BLKCTL_RESCAN 5   /* re-read a disk's partition table: struct cosmo_blkctl_rescan */
#define COSMO_BLKCTL_FORMAT 6   /* make an empty filesystem of type `fstype` on the device */

/* The most data one READ or WRITE carries. */
#define COSMO_BLKCTL_IO_MAX (32u * 1024u)

#define COSMO_BLKCTL_NAME 16u

struct cosmo_blkctl {
    uint16_t version;
    uint16_t op;
    uint32_t count;                    /* READ, WRITE: sectors */
    uint64_t sector;                   /* READ, WRITE */
    char name[COSMO_BLKCTL_NAME];      /* the device (every op but LIST) */
    char fstype[COSMO_BLKCTL_NAME];    /* FORMAT */
};

/* cosmo_blkctl_dev.flags */
#define COSMO_BLKCTL_PART     (1u << 0)   /* a partition; `disk`, `index`, `start`, `uuid`, `type` say which */
#define COSMO_BLKCTL_RDONLY   (1u << 1)
#define COSMO_BLKCTL_BOOT     (1u << 2)   /* the partition the loader was read from */
#define COSMO_BLKCTL_MOUNTED  (1u << 3)   /* it, its disk or one of its partitions is mounted */

struct cosmo_blkctl_dev {
    char name[COSMO_BLKCTL_NAME];
    char disk[COSMO_BLKCTL_NAME];      /* partitions: the disk; "" otherwise */
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t flags;
    uint64_t start;                    /* partitions: first sector on the disk */
    uint32_t index;                    /* partitions: entry number, from 1 */
    uint32_t reserved;
    uint8_t uuid[16];                  /* partitions: unique GUID, on-disk byte order */
    uint8_t type[16];                  /* partitions: type GUID, on-disk byte order */
};

/* LIST: this header, then `count` records. */
struct cosmo_blkctl_list {
    uint32_t version;
    uint32_t count;
};

struct cosmo_blkctl_rescan {
    uint32_t partitions;               /* registered by the scan */
    uint32_t reserved;
};

#endif /* UAPI_COSMO_BLKCTL_H */
