/*
 * cosmofs_format.h - cosmofs on-disk layout (version 2).
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
#define CFS_VERSION      2u   /* version 2: extents carry lblk, extent blocks chain, per-inode checksum tree */
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
    CFS_KIND_CSUMIDX = 7,   /* an inode's checksum index: 508 pointers to CSUM blocks */
    CFS_KIND_CSUM = 8,      /* 1016 CRC32C values, one per logical block */
};

/* Inode checksum algorithms (cfs_inode.csum_algo). */
#define CFS_CSUM_NONE   0u
#define CFS_CSUM_CRC32C 1u

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
#define CFS_EXTENTS_PER_BLOCK ((CFS_PAYLOAD - 8u) / 16u)  /* 253: after the chain pointer */
#define CFS_DIRECT           10u
#define CFS_MAX_EXTENTS      4096u   /* runs per inode the implementation will load (a fragmentation bound, not a format limit) */
#define CFS_CSUMS_PER_BLOCK  (CFS_PAYLOAD / 4u)           /* 1016 */
#define CFS_CSUM_MAX_BLOCKS  ((uint64_t)CFS_PTRS_PER_BLOCK * CFS_CSUMS_PER_BLOCK)
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
    uint32_t lblk;         /* first logical block; runs are sorted by lblk, gaps are holes */
};

/* Payload of a CFS_KIND_EXTENTS block: the chain continues at `next` (0 ends it). */
struct cfs_extent_block {
    uint64_t next;
    struct cfs_extent ext[CFS_EXTENTS_PER_BLOCK];
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
    uint64_t indirect;     /* head of the CFS_KIND_EXTENTS chain, or 0 */
    uint64_t parent;       /* directories: parent inode number */
    uint32_t csum_algo;    /* CFS_CSUM_*: how data and directory blocks are checksummed */
    uint32_t csum_pad;
    uint64_t csum_root;    /* CFS_KIND_CSUMIDX block or 0 (no block written yet) */
    uint64_t reserved;
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

/* Map logical block `lblk` through `n` runs sorted by lblk. Returns 1 and
 * the pool block, or 0 for a hole (no run covers it). */
static inline int cfs_map_block(const struct cfs_extent *ext, unsigned n, uint64_t lblk, uint64_t *pblk)
{
    for (unsigned i = 0; i < n; i++) {
        if (lblk >= ext[i].lblk && lblk < (uint64_t)ext[i].lblk + ext[i].count) {
            *pblk = ext[i].start + (lblk - ext[i].lblk);
            return 1;
        }
        if (ext[i].lblk > lblk)
            break;
    }
    return 0;
}

/* One past the highest mapped logical block (0 when nothing is mapped). */
static inline uint64_t cfs_extent_blocks(const struct cfs_extent *ext, unsigned n)
{
    uint64_t end = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t e = (uint64_t)ext[i].lblk + ext[i].count;
        if (e > end)
            end = e;
    }
    return end;
}

/* Where logical block `lblk`'s checksum lives: index slot and entry. */
static inline unsigned cfs_csum_index(uint64_t lblk) { return (unsigned)(lblk / CFS_CSUMS_PER_BLOCK); }
static inline unsigned cfs_csum_slot(uint64_t lblk) { return (unsigned)(lblk % CFS_CSUMS_PER_BLOCK); }

#endif /* COSMOFS_FORMAT_H */
