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
#define CFS_VERSION      7u   /* version 7: encryption (CFS_KIND_KEYS, CFS_CSUM_POLY1305) */
#define CFS_VERSION_MIN  2u   /* versions 2 and 3 mount unchanged: their pointers are vdev-0 DVAs */
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
    CFS_KIND_SNAPLIST = 9,  /* snapshots: CFS_SNAPS_PER_BLOCK entries and a `next` */
    CFS_KIND_DEADLIST = 10, /* a snapshot's freed blocks: block numbers and a `next` */
    CFS_KIND_MEMBERS = 11,  /* the pool's member table: CFS_MEMBERS_PER_BLOCK entries */
    CFS_KIND_KEYS = 12,     /* the wrapped master key (struct cfs_keys) */
};

/* --- device-virtual addresses --------------------------------------------
 *
 * Every pointer on disk is a DVA: the member that holds the block in the
 * top 8 bits, the block within that member in the low 56. A version-2 or
 * -3 pointer is a version-4 DVA with vdev 0, which is why those formats
 * mount unchanged (design.md, "Format version 4: many members").
 */
#define CFS_DVA_VDEV_SHIFT 56
#define CFS_DVA_BLK_MASK   ((1ull << CFS_DVA_VDEV_SHIFT) - 1)
#define CFS_MAX_MEMBERS    255u   /* vdev 255 is reserved: see CFS_DVA_NONE */
#define CFS_DVA_NONE       0xFFFFFFFFFFFFFFFFull   /* not a block; distinct from 0 */

#define CFS_DVA(vdev, blk) (((uint64_t)(vdev) << CFS_DVA_VDEV_SHIFT) | ((uint64_t)(blk) & CFS_DVA_BLK_MASK))
#define CFS_DVA_VDEV(d)    ((unsigned)((uint64_t)(d) >> CFS_DVA_VDEV_SHIFT))
#define CFS_DVA_BLK(d)     ((uint64_t)(d) & CFS_DVA_BLK_MASK)

/* Inode checksum algorithms (cfs_inode.csum_algo). */
#define CFS_CSUM_NONE     0u
#define CFS_CSUM_CRC32C   1u
/* Version 7: the entry is a Poly1305 tag over the block's *ciphertext*
 * and the generation that wrote it, which is half of the nonce. A CRC
 * says accident from intact; it says nothing about a deliberate change,
 * because anyone can recompute it (design.md, "Integrity"). */
#define CFS_CSUM_POLY1305 2u

/* Compression algorithms (cfs_inode.compress_algo, and the algorithm of
 * a compressed extent). */
#define CFS_COMPRESS_NONE 0u
#define CFS_COMPRESS_LZ4  1u

/* Logical blocks compressed as one unit. A record is the smallest thing
 * compression can shrink -- a single block that compresses to a quarter
 * still occupies a block -- and the largest thing a read of one page has
 * to decompress (design.md, "Format version 6"). */
#define CFS_RECORD_BLOCKS 8u

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
/* One authenticated entry per logical block. The generation is here
 * because a data block carries no header to hold it, and the nonce
 * needs it: a block is written once per generation, so (block, its
 * generation) never repeats with different contents under one key. */
struct cfs_csum_aead {          /* 32 bytes */
    uint8_t tag[16];
    uint64_t generation;
    uint64_t reserved;
};
#define CFS_AEAD_PER_BLOCK   (CFS_PAYLOAD / sizeof(struct cfs_csum_aead))
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

/*
 * A run of logical blocks and where they are. `count` carries the
 * compression state, because there is nowhere else to put it: every
 * structure on disk is exactly full (design.md, "Where the size goes").
 *
 *   bit 31 clear: an ordinary run of `count` blocks, one physical block
 *                 per logical one, up to two billion of them.
 *   bit 31 set:   a compressed record. 30..24 the algorithm, 23..16 the
 *                 physical block count less one, 15..0 the logical block
 *                 count. Its physical blocks are consecutive from
 *                 `start`, and mean nothing apart from each other.
 *
 * Every filesystem written before version 6 has counts far below 2^31,
 * so its runs read as uncompressed without a conversion.
 */
struct cfs_extent {
    uint64_t start;        /* first pool block */
    uint32_t count;        /* blocks, > 0 in a live run; see above */
    uint32_t lblk;         /* first logical block; runs are sorted by lblk, gaps are holes */
};

#define CFS_EXT_COMPRESSED  (1u << 31)
#define CFS_EXT_ALGO_SHIFT  24u
#define CFS_EXT_ALGO_MASK   0x7Fu
#define CFS_EXT_PSIZE_SHIFT 16u
#define CFS_EXT_PSIZE_MASK  0xFFu
#define CFS_EXT_COUNT_MASK  0xFFFFu

static inline int cfs_ext_compressed(const struct cfs_extent *e)
{
    return (e->count & CFS_EXT_COMPRESSED) != 0;
}

/* Logical blocks the run covers. */
static inline uint32_t cfs_ext_count(const struct cfs_extent *e)
{
    return cfs_ext_compressed(e) ? (e->count & CFS_EXT_COUNT_MASK) : e->count;
}

/* Physical blocks it occupies: the same, unless it is compressed. */
static inline uint32_t cfs_ext_psize(const struct cfs_extent *e)
{
    return cfs_ext_compressed(e) ? ((e->count >> CFS_EXT_PSIZE_SHIFT) & CFS_EXT_PSIZE_MASK) + 1u : cfs_ext_count(e);
}

static inline unsigned cfs_ext_algo(const struct cfs_extent *e)
{
    return cfs_ext_compressed(e) ? (unsigned)((e->count >> CFS_EXT_ALGO_SHIFT) & CFS_EXT_ALGO_MASK) : CFS_COMPRESS_NONE;
}

/* The `count` word for a compressed record; psize and count are both at
 * least 1 and within their fields, which the caller has already bounded
 * by CFS_RECORD_BLOCKS. */
static inline uint32_t cfs_ext_pack(unsigned algo, uint32_t psize, uint32_t count)
{
    return CFS_EXT_COMPRESSED | ((algo & CFS_EXT_ALGO_MASK) << CFS_EXT_ALGO_SHIFT) |
           (((psize - 1u) & CFS_EXT_PSIZE_MASK) << CFS_EXT_PSIZE_SHIFT) | (count & CFS_EXT_COUNT_MASK);
}

/* Payload of a CFS_KIND_EXTENTS block: the chain continues at `next` (0 ends it). */
struct cfs_extent_block {
    uint64_t next;
    struct cfs_extent ext[CFS_EXTENTS_PER_BLOCK];
};

#define CFS_SNAP_NAME_MAX 31u

/* A snapshot is the tuple a commit publishes, kept: every tree it names
 * is copy-on-write, so nothing has to be copied to take one
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 3"). */
struct cfs_snapshot {          /* 96 bytes */
    uint64_t generation;       /* the commit this snapshot pins */
    uint64_t imap_root;
    uint64_t alloc_root;
    uint64_t next_ino;
    uint64_t inode_count;
    uint64_t deadlist;         /* head of a CFS_KIND_DEADLIST chain, or 0 */
    uint64_t created_ns;
    /* Never reused while the filesystem lives: it is the tag a
     * snapshot's vnodes carry, and a cached vnode must never be handed
     * to a different snapshot (design.md, "Reading a snapshot"). */
    uint64_t id;
    char name[CFS_SNAP_NAME_MAX + 1];
};

#define CFS_SNAPS_PER_BLOCK ((CFS_BLOCK - CFS_MHDR_SIZE - 8) / sizeof(struct cfs_snapshot))

/* A snapshot's vnodes share the mount's inode numbers, so they carry a
 * tag in bits above the inode-number space: the VFS caches vnodes by
 * (mount, ino) and a snapshot's root must not collide with the live
 * one. CFS_MAX_INODES is far below 2^48, which is what makes the
 * partition safe (design.md, "Reading a snapshot"). */
#define CFS_SNAP_INO_SHIFT 48
#define CFS_SNAPDIR_INO    ((uint64_t)0xFFFFull << CFS_SNAP_INO_SHIFT)   /* the .snapshots directory */
#define CFS_SNAP_ID_MAX    0xFFFEu   /* 0xFFFF is the .snapshots tag; 0 is the live tree */
#define CFS_SNAP_TAG(ino)  ((unsigned)(((uint64_t)(ino)) >> CFS_SNAP_INO_SHIFT))
#define CFS_SNAP_INO(tag, ino) (((uint64_t)(tag) << CFS_SNAP_INO_SHIFT) | (uint64_t)(ino))
#define CFS_INO_OF(ino)    ((ino) & ((1ull << CFS_SNAP_INO_SHIFT) - 1))
#define CFS_SNAPDIR_NAME   ".snapshots"

/* Payload of a CFS_KIND_SNAPLIST block; the chain continues at `next`. */
struct cfs_snap_block {
    uint64_t next;
    struct cfs_snapshot snap[CFS_SNAPS_PER_BLOCK];
};

#define CFS_DEAD_PER_BLOCK ((CFS_BLOCK - CFS_MHDR_SIZE - 16) / sizeof(uint64_t))

/* Payload of a CFS_KIND_DEADLIST block: blocks a snapshot still names
 * that the live tree has released. */
struct cfs_dead_block {
    uint64_t next;
    uint64_t count;            /* entries used in this block */
    uint64_t blk[CFS_DEAD_PER_BLOCK];
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
    uint32_t compress_algo;/* CFS_COMPRESS_*: what new records of this file are written with */
    uint32_t reserved;
};

/* One pool member. Its allocation metadata lives on the member it
 * describes: losing a member must not take another member's bitmap with
 * it (design.md, "The member table"). */
struct cfs_member {            /* 64 bytes */
    uint8_t uuid[16];
    uint64_t nblocks;
    uint64_t first_usable;     /* 2 on member 0 (the superblocks), 1 elsewhere (the label) */
    uint64_t alloc_root;       /* this member's CFS_KIND_ALLOCIDX block, as a DVA */
    uint64_t free_blocks;
    uint32_t flags;
    /* Devices holding this member's blocks: a mirror group. Written as 0
     * by version 4, which had one device per member and read the field
     * as padding, so 0 and 1 mean the same thing. */
    uint32_t copies;
    uint64_t reserved;
};

#define CFS_MAX_COPIES 4u

#define CFS_MEMBERS_PER_BLOCK ((CFS_BLOCK - CFS_MHDR_SIZE - 8) / sizeof(struct cfs_member))

/* Payload of a CFS_KIND_MEMBERS block. */
struct cfs_member_block {
    uint64_t count;
    struct cfs_member m[CFS_MEMBERS_PER_BLOCK];
};

/* Block 0 of every member past the first: how a mount finds the rest of
 * the pool from the one device it was given. Member 0 is identified by
 * its superblock, which carries the same uuid. */
#define CFS_LABEL_MAGIC "COSMOMBR"

struct cfs_label {
    uint8_t magic[8];
    uint32_t version;
    uint32_t index;            /* this member's vdev number */
    uint8_t uuid[16];          /* the pool's, matching cfs_super.uuid */
    uint64_t nblocks;
    uint32_t copy;             /* which copy of that member this device holds */
    uint32_t pad2;
    /* The commit this device last took part in. A device that was
     * detached while the pool went on being written has an older one,
     * and is refused rather than mirrored: its blocks would pass every
     * checksum and still be the wrong contents (design.md, "A mirror is
     * only as good as its verifier"). */
    uint64_t generation;
    uint64_t reserved[2];
    uint32_t crc;              /* CRC32C over the block with this field zero */
    uint32_t pad;
};

/*
 * The wrapped master key (CFS_KIND_KEYS), named by cfs_super.key_root.
 *
 * The master key is random and never leaves the kernel in the clear.
 * The user's key derives a wrapping key through the salt; the wrapping
 * key encrypts the master and authenticates it, so a wrong key is
 * refused at mount instead of producing plausible rubbish. Rotation
 * rewrites this block alone -- no file is touched, because the user's
 * key never encrypted any of them (design.md, "Key storage and
 * rotation").
 */
struct cfs_keys {
    uint32_t version;          /* 1 */
    uint32_t kdf;              /* CFS_KDF_* */
    uint8_t salt[16];
    uint8_t wrapped[32];       /* the master key, encrypted with the wrapping key */
    uint8_t tag[16];           /* over `wrapped`: what tells a wrong key from a right one */
    uint64_t reserved[4];
};

#define CFS_KDF_SHA512 1u

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
    uint64_t members;      /* v4: DVA of the CFS_KIND_MEMBERS block. v2/v3: the constant 1 */
    uint8_t uuid[16];      /* v4: the pool's uuid, matching every member's label */
    uint64_t key_root;     /* v7: DVA of the CFS_KIND_KEYS block, or 0 when not encrypted */
    uint64_t reserved[5];
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
        if (lblk >= ext[i].lblk && lblk < (uint64_t)ext[i].lblk + cfs_ext_count(&ext[i])) {
            if (cfs_ext_compressed(&ext[i]))
                return -1;   /* a record: its blocks mean nothing apart from each other */
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
