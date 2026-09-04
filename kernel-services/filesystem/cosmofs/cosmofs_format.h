/*
 * cosmofs_format.h - cosmofs on-disk layout (version 1).
 *
 * Pure definitions and inline helpers; compiled on the host by
 * tests/host/test_cosmofs.c. All integers little-endian. Every metadata
 * block starts with struct cfs_mhdr; data blocks have no header. See
 * docs/kernel-services/vfs/design.md.
 */

#ifndef COSMOFS_FORMAT_H
#define COSMOFS_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#define CFS_BLOCK        4096u
#define CFS_MAGIC        "COSMOFS1"
#define CFS_VERSION      1u
#define CFS_MHDR_MAGIC   0x4d534643u   /* "CFSM" */
#define CFS_ROOT_INO     1u
#define CFS_SUPER_A      0u
#define CFS_SUPER_B      1u

enum cfs_kind {
    CFS_KIND_IMAP1 = 1,
    CFS_KIND_IMAP0 = 2,
    CFS_KIND_INODES = 3,
    CFS_KIND_ALLOCIDX = 4,
    CFS_KIND_BITMAP = 5,
    CFS_KIND_EXTENTS = 6,
};

struct cfs_mhdr {
    uint32_t magic;
    uint32_t kind;
    uint64_t generation;
    uint64_t blkno;
    uint32_t crc;          /* CRC32C over the block with this field zero */
    uint32_t pad;
};

#define CFS_MHDR_SIZE        32u
#define CFS_PAYLOAD          (CFS_BLOCK - CFS_MHDR_SIZE)
#define CFS_PTRS_PER_BLOCK   (CFS_PAYLOAD / 8u)           /* 508 */
#define CFS_INODE_SIZE       256u
#define CFS_INODES_PER_BLOCK (CFS_PAYLOAD / CFS_INODE_SIZE) /* 15 */
#define CFS_BITS_PER_BITMAP  (CFS_PAYLOAD * 8u)           /* 32512 */
#define CFS_EXTENTS_PER_BLOCK (CFS_PAYLOAD / 16u)         /* 254 */
#define CFS_DIRECT           10u
#define CFS_MAX_EXTENTS      (CFS_DIRECT + CFS_EXTENTS_PER_BLOCK)
#define CFS_DIRENT_SIZE      64u
#define CFS_DIRENTS_PER_BLOCK (CFS_BLOCK / CFS_DIRENT_SIZE)
#define CFS_NAME_MAX         47u
#define CFS_MAX_INODES       ((uint64_t)CFS_PTRS_PER_BLOCK * CFS_PTRS_PER_BLOCK * CFS_INODES_PER_BLOCK)
#define CFS_MAX_BLOCKS       ((uint64_t)CFS_PTRS_PER_BLOCK * CFS_BITS_PER_BITMAP)

#define CFS_TYPE_REG 1u
#define CFS_TYPE_DIR 2u
#define CFS_MODE_TYPE(mode) ((mode) >> 12)
#define CFS_MODE(type, perm) (((type) << 12) | ((perm) & 07777u))

struct cfs_extent {
    uint64_t start;        /* first pool block */
    uint32_t count;        /* blocks, > 0 in a live run */
    uint32_t pad;
};

struct cfs_inode {
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
    uint64_t generation;
    uint64_t ino;
    struct cfs_extent direct[CFS_DIRECT];
    uint64_t indirect;     /* CFS_KIND_EXTENTS block or 0 */
    uint64_t parent;       /* directories: parent inode number */
    uint64_t reserved[3];
};

struct cfs_dirent {
    uint64_t ino;          /* 0 = free slot */
    uint8_t type;          /* CFS_TYPE_* */
    uint8_t namelen;
    uint8_t pad[6];
    char name[48];
};

struct cfs_super {
    uint8_t magic[8];
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t generation;
    uint64_t imap_root;
    uint64_t alloc_root;
    uint64_t next_ino;
    uint64_t inode_count;
    uint64_t free_blocks;
    uint64_t csum_root;    /* reserved: data checksum tree */
    uint64_t snap_root;    /* reserved: snapshot roots */
    uint64_t members;      /* pool members, 1 */
    uint64_t reserved[8];
    uint32_t crc;          /* CRC32C over the whole block with this field zero */
    uint32_t pad;
};

/* --- inline helpers ------------------------------------------------------- */

static inline uint64_t cfs_inode_block_index(uint64_t ino) { return ino / CFS_INODES_PER_BLOCK; }
static inline unsigned cfs_inode_slot(uint64_t ino) { return (unsigned)(ino % CFS_INODES_PER_BLOCK); }
static inline unsigned cfs_imap_l0_index(uint64_t ino) { return (unsigned)(cfs_inode_block_index(ino) % CFS_PTRS_PER_BLOCK); }
static inline unsigned cfs_imap_l1_index(uint64_t ino) { return (unsigned)(cfs_inode_block_index(ino) / CFS_PTRS_PER_BLOCK); }

/* Map logical block `lblk` through `n` runs. Returns 1 and the pool
 * block, or 0 for a hole beyond the runs. */
static inline int cfs_map_block(const struct cfs_extent *ext, unsigned n, uint64_t lblk, uint64_t *pblk)
{
    uint64_t base = 0;
    for (unsigned i = 0; i < n; i++) {
        if (lblk < base + ext[i].count) {
            *pblk = ext[i].start + (lblk - base);
            return 1;
        }
        base += ext[i].count;
    }
    return 0;
}

/* Total logical blocks covered by the runs. */
static inline uint64_t cfs_extent_blocks(const struct cfs_extent *ext, unsigned n)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++)
        total += ext[i].count;
    return total;
}

#endif /* COSMOFS_FORMAT_H */
