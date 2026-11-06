/*
 * cosmofs_corrupt.c - The structural check's test hook: break a mounted
 * filesystem in one named way, so that every finding the check can report
 * has a test that produced it on purpose. Moved out of cosmofs_core.c
 * (roadmap 2.0 SA7) so that it can be held to a digest and made CeeSafe on
 * its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mutex.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/*
 * Break the filesystem in one named way (kernel/include/kernel/cosmofs.h).
 * Each case manufactures exactly one finding of the structural check, so
 * that every class it can report has a test that produced it on purpose.
 */
int cosmofs_test_corrupt(struct mount *mnt, enum cosmofs_corruption kind, uint64_t ino, uint64_t *what)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;

    /*
     * An inode number rather than a path: a filesystem-level hook has no
     * business resolving names, it has to work on a filesystem whose
     * directories are the broken part, and resolving one here would put
     * a VFS symbol in this file that the fuzz harness has to link.
     */
    struct cfs_inode in;
    if (ino == 0) {
        /* Only these whole-filesystem faults are valid without a target
         * inode. Every inode-targeted case must not inspect an absent slot. */
        switch (kind) {
        case COSMOFS_CORRUPT_LEAK:
        case COSMOFS_CORRUPT_ORPHAN:
        case COSMOFS_CORRUPT_DANGLING:
        case COSMOFS_CORRUPT_DIRENT:
        case COSMOFS_CORRUPT_COUNTER:
        case COSMOFS_CORRUPT_SNAP_MEMBERS:
        case COSMOFS_CORRUPT_SNAP_UUID:
        case COSMOFS_CORRUPT_ROLLBACK_CRC:
            memset(&in, 0, sizeof(in));
            break;
        default:
            return -EINVAL;
        }
    } else if (cfs_inode_read(fs, ino, &in) != 0) {
        return -ENOENT;
    }

    mutex_lock(&fs->lock);
    int rc = 0;
    uint64_t token = 0;
    switch (kind) {
    case COSMOFS_CORRUPT_LEAK: {
        uint64_t blk = 0, got = 0;
        rc = cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, &blk, &got);
        token = blk;
        break;
    }
    case COSMOFS_CORRUPT_FREE_IN_USE: {
        uint64_t lin = cfs_dva_lin(fs, in.direct[0].start);
        if (lin == CFS_DVA_NONE) {
            rc = -EINVAL;
            break;
        }
        cfs_bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->sb.free_blocks++;
        token = in.direct[0].start;
        break;
    }
    case COSMOFS_CORRUPT_CROSSLINK: {
        /* A second inode whose first extent is the named file's: two
         * live claims on one block, which is the cross-link. */
        uint64_t second = 0;
        rc = cfs_inode_alloc(fs, &second);
        if (rc)
            break;
        struct cfs_inode twin;
        memset(&twin, 0, sizeof(twin));
        twin.mode = CFS_MODE(CFS_TYPE_REG, 0644);
        twin.nlink = 1;
        twin.ino = second;
        twin.parent = CFS_ROOT_INO;
        twin.csum_algo = in.csum_algo;
        twin.size = CFS_BLOCK;
        twin.direct[0] = in.direct[0];
        rc = cfs_inode_write(fs, second, &twin);
        token = in.direct[0].start;
        break;
    }
    case COSMOFS_CORRUPT_ROOT_TYPE:
        in.mode = CFS_MODE(CFS_TYPE_REG, in.mode & 07777u);
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    case COSMOFS_CORRUPT_NLINK:
        in.nlink++;
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    case COSMOFS_CORRUPT_ORPHAN: {
        /* An inode with a block and no name: what a crash between an
         * unlink and the eviction that frees the blocks leaves. */
        uint64_t lost = 0;
        rc = cfs_inode_alloc(fs, &lost);
        if (rc)
            break;
        struct cfs_inode dead;
        memset(&dead, 0, sizeof(dead));
        dead.mode = CFS_MODE(CFS_TYPE_REG, 0644);
        dead.nlink = 0;
        dead.ino = lost;
        dead.parent = CFS_ROOT_INO;
        dead.csum_algo = CFS_CSUM_CRC32C;
        uint64_t blk = 0, got = 0;
        rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, 0, 1, &blk, &got);
        if (rc)
            break;
        dead.size = CFS_BLOCK;
        dead.direct[0].start = blk;
        dead.direct[0].count = 1;
        dead.direct[0].lblk = 0;
        rc = cfs_inode_write(fs, lost, &dead);
        token = lost;
        break;
    }
    case COSMOFS_CORRUPT_DANGLING:
    case COSMOFS_CORRUPT_DIRENT: {
        /* Both rewrite one entry of the root directory: the first to
         * name a slot nothing allocated, the second to claim a type its
         * inode does not have. */
        struct cfs_inode root;
        if (cfs_inode_read(fs, CFS_ROOT_INO, &root) != 0) {
            rc = -EIO;
            break;
        }
        uint8_t *block = kmalloc(CFS_BLOCK, 0);
        if (block == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = cfs_dir_read_block_at(fs, &root, 0, block);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)block;
            bool done = false;
            for (unsigned i = 0; i < CFS_DIRENTS_PER_BLOCK && !done; i++) {
                if (d[i].ino == 0)
                    continue;
                if (kind == COSMOFS_CORRUPT_DANGLING) {
                    token = fs->sb.next_ino + 1000;
                    d[i].ino = token;
                } else {
                    token = d[i].ino;
                    d[i].type = d[i].type == CFS_TYPE_DIR ? CFS_TYPE_REG : CFS_TYPE_DIR;
                }
                done = true;
            }
            rc = done ? cfs_dir_write_block_at(fs, &root, 0, block) : -ENOENT;
        }
        kfree(block);
        break;
    }
    case COSMOFS_CORRUPT_INO_SLOT:
        /* The slot keeps its position and loses its identity: the walk
         * reaches it by position and must not believe the number in it.
         * Nothing else changes, so a check that trusts the field sees an
         * ordinary inode at a number that does not exist. */
        in.ino = ino + 1000;
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    /*
     * Both of these add runs to a named file rather than editing the
     * one it has, because the fixture's file is a single block and
     * neither fault can be expressed in one run. The added runs point
     * at freshly ALLOCATED blocks, distinct from each other and from
     * the file's own: a run pointing at the file's block would be a
     * cross-link and one pointing at a free block would be
     * seen_not_alloc, and either would make the test pass for the
     * wrong reason (docs/audit/next-subsystem-fsck-unchecked.md).
     */
    case COSMOFS_CORRUPT_EXTENT_ORDER:
    case COSMOFS_CORRUPT_EXTENT_OVERLAP: {
        if (ino == 0 || cfs_ext_count(&in.direct[0]) == 0 ||
            cfs_ext_count(&in.direct[1]) != 0 || cfs_ext_count(&in.direct[2]) != 0) {
            /* Needs a file of exactly one run with room after it. Refused
             * rather than silently producing some other fault. */
            rc = -EINVAL;
            break;
        }
        /*
         * The overlap case needs its middle run to be TWO blocks wide,
         * so a third run can begin inside it. A one-block allocation
         * under a count of two would have the run claim its neighbour,
         * which the cross-link map catches -- the test would then pass
         * against the OLD code and prove nothing. (It did, on the
         * first build: `dup` fired and the vacuity guard caught it.)
         */
        uint32_t wide = (kind == COSMOFS_CORRUPT_EXTENT_OVERLAP) ? 2u : 1u;
        uint64_t a = 0, b = 0, got = 0;
        rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, 0, wide, &a, &got);
        if (rc)
            break;
        if (got < wide) {
            rc = -ENOSPC;     /* a short run would mean the same claim-the-neighbour bug */
            break;
        }
        rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, 0, 1, &b, &got);
        if (rc)
            break;
        if (got < 1) {
            rc = -ENOSPC;
            break;
        }
        uint32_t first = in.direct[0].lblk;      /* count 1: covers [first, first+1) */
        if (kind == COSMOFS_CORRUPT_EXTENT_ORDER) {
            /* Ascend, then DESCEND: 0, 5, 3. */
            in.direct[1].start = a; in.direct[1].count = 1; in.direct[1].lblk = first + 5;
            in.direct[2].start = b; in.direct[2].count = 1; in.direct[2].lblk = first + 3;
        } else {
            /* Ascend throughout, and the third begins inside the
             * second: 0/[1,3)/2. Strictly ascending, so the ordering
             * test passes and the overlap test is the one that fires. */
            in.direct[1].start = a; in.direct[1].count = wide; in.direct[1].lblk = first + 1;
            in.direct[2].start = b; in.direct[2].count = 1; in.direct[2].lblk = first + 2;
        }
        if (in.size < (uint64_t)(first + 8) * CFS_BLOCK)
            in.size = (uint64_t)(first + 8) * CFS_BLOCK;
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    }
    /*
     * Two of the four `dir_bad` sites that no test had ever reached
     * (docs/audit/next-subsystem-fsck-unchecked.md). A reporting path
     * with no test is a path that has never executed.
     */
    case COSMOFS_CORRUPT_BAD_PTR:
        /* A data pointer past the end of every member: `claim` maps it
         * to CFS_DVA_NONE and must say so rather than indexing a
         * bitmap with it. The run keeps count 1, so exactly one bad
         * pointer is claimed. */
        if (ino == 0 || cfs_ext_count(&in.direct[0]) == 0) {
            rc = -EINVAL;
            break;
        }
        in.direct[0].start = CFS_DVA_NONE - 1;
        rc = cfs_inode_write(fs, ino, &in);
        token = in.direct[0].start;
        break;
    case COSMOFS_CORRUPT_NAMELEN: {
        /* An entry claiming a name longer than its slot can hold. The
         * check must refuse it by length before reading the bytes. */
        uint8_t *blk = kmalloc(CFS_BLOCK, 0);
        if (blk == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = cfs_dir_read_block_at(fs, &in, 0, blk);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)blk;
            unsigned s2 = 0;
            while (s2 < CFS_DIRENTS_PER_BLOCK && d[s2].ino == 0)
                s2++;
            if (s2 == CFS_DIRENTS_PER_BLOCK)
                rc = -EINVAL;          /* an empty directory cannot carry this fault */
            else {
                /* Longer than any one slot holds at every version: below 15
                 * past CFS_NAME_MAX, from 15 a length needing continuations
                 * the entry does not have (design.md, "Format version 15"). */
                d[s2].namelen = (uint8_t)CFS_NAME_MAX_LONG;
                d[s2].extra = 0;
                rc = cfs_dir_write_block_at(fs, &in, 0, blk);
                token = ino;
            }
        }
        kfree(blk);
        break;
    }
    case COSMOFS_CORRUPT_TWO_PARENTS: {
        /* A second directory naming a directory that already has a
         * parent: a cycle in what must be a tree. The checker reaches
         * it twice and the second arrival is the finding. */
        uint8_t *blk = kmalloc(CFS_BLOCK, 0);
        if (blk == NULL) {
            rc = -ENOMEM;
            break;
        }
        struct cfs_inode root;
        rc = cfs_inode_read(fs, CFS_ROOT_INO, &root);
        if (rc == 0)
            rc = cfs_dir_read_block_at(fs, &root, 0, blk);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)blk;
            unsigned dst = 0;
            while (dst < CFS_DIRENTS_PER_BLOCK && d[dst].ino != 0)
                dst++;
            if (dst == CFS_DIRENTS_PER_BLOCK)
                rc = -ENOSPC;
            else {
                /* `ino` is the directory to name a second time. */
                d[dst].ino = ino;
                d[dst].type = CFS_TYPE_DIR;
                d[dst].namelen = 6;
                memcpy(d[dst].name, "second", 7);
                rc = cfs_dir_write_block_at(fs, &root, 0, blk);
                token = ino;
            }
        }
        kfree(blk);
        break;
    }
    case COSMOFS_CORRUPT_CHAIN_CYCLE: {
        /*
         * Two extent-chain blocks pointing at each other, hung off the
         * named inode. The walk must terminate on its own guard rather
         * than on the chain running out -- which is the whole content
         * of the class, and the reason the bug-proof for it removes
         * the guard and watches the boot time out.
         */
        struct cfs_buf *x = NULL, *y = NULL;
        rc = cfs_buf_new(fs, CFS_KIND_EXTENTS, &x);
        if (rc)
            break;
        rc = cfs_buf_new(fs, CFS_KIND_EXTENTS, &y);
        if (rc) {
            cfs_buf_put(fs, x);
            break;
        }
        struct cfs_extent_block *ebx = (struct cfs_extent_block *)(x->data + CFS_MHDR_SIZE);
        struct cfs_extent_block *eby = (struct cfs_extent_block *)(y->data + CFS_MHDR_SIZE);
        ebx->next = y->blkno;
        eby->next = x->blkno;          /* back to the first: the cycle */
        x->dirty = y->dirty = true;
        uint64_t head = x->blkno;
        cfs_buf_put(fs, x);
        cfs_buf_put(fs, y);
        in.indirect = head;
        rc = cfs_inode_write(fs, ino, &in);
        token = head;
        break;
    }
    case COSMOFS_CORRUPT_SNAP_MEMBERS: {
        /*
         * A snapshot's member table claiming more members than its
         * block can hold. The checker must say the TABLE is wrong and
         * still walk what the block does hold -- otherwise every
         * member's blocks are reported as leaked instead, which is the
         * comment beside that site and had never been exercised.
         *
         * Version 4 and later only: before it, a snapshot's alloc_root
         * is an index rather than a member table and there is no count
         * to be wrong.
         */
        if (fs->sb.version < 4) {
            rc = -ENOTSUP;
            break;
        }
        uint64_t list = fs->sb.snap_root, target = 0;
        unsigned guard = 0;
        while (list != 0 && target == 0 && guard++ < 64) {
            struct cfs_buf *b;
            if (cfs_buf_get(fs, list, cfs_snaplist_kind(fs), &b) != 0)
                break;
            unsigned slots = cfs_snaplist_slots(fs);
            for (unsigned i = 0; i < slots; i++) {
                struct cfs_snapshot s;
                if (cfs_snaplist_get(fs, b, i, &s) && s.id != 0 && s.alloc_root != 0) {
                    target = s.alloc_root;
                    break;
                }
            }
            list = cfs_snaplist_next(b);
            cfs_buf_put(fs, b);
        }
        if (target == 0) {
            rc = -ENOENT;          /* no snapshot: the caller must take one first */
            break;
        }
        struct cfs_buf *mb;
        rc = cfs_buf_get(fs, target, CFS_KIND_MEMBERS, &mb);
        if (rc)
            break;
        struct cfs_member_block *t = (struct cfs_member_block *)(mb->data + CFS_MHDR_SIZE);
        t->count = CFS_MEMBERS_PER_BLOCK + 1;
        mb->dirty = true;
        cfs_buf_put(fs, mb);
        token = target;
        break;
    }
    case COSMOFS_CORRUPT_DUP_NAME: {
        /*
         * One name twice in a directory, at two DIFFERENT inodes --
         * which is what makes it a fault rather than a hard link: two
         * entries resolve differently, unlink removes one and the name
         * survives.
         */
        uint8_t *blk = kmalloc(CFS_BLOCK, 0);
        if (blk == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = cfs_dir_read_block_at(fs, &in, 0, blk);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)blk;
            unsigned src = 0;
            while (src < CFS_DIRENTS_PER_BLOCK && d[src].ino == 0)
                src++;
            unsigned dst = 0;
            while (dst < CFS_DIRENTS_PER_BLOCK && d[dst].ino != 0)
                dst++;
            if (src == CFS_DIRENTS_PER_BLOCK || dst == CFS_DIRENTS_PER_BLOCK)
                rc = -EINVAL;
            else {
                uint64_t twin = 0;
                rc = cfs_inode_alloc(fs, &twin);
                if (rc == 0) {
                    struct cfs_inode ti;
                    memset(&ti, 0, sizeof(ti));
                    ti.mode = CFS_MODE(CFS_TYPE_REG, 0644);
                    ti.nlink = 1;
                    ti.ino = twin;
                    ti.parent = ino;
                    rc = cfs_inode_write(fs, twin, &ti);
                }
                if (rc == 0) {
                    /* The same name, however many slots it takes, at a
                     * different inode. */
                    char nm[CFS_NAME_MAX_LONG];
                    unsigned nlen = cfs_dirent_check(d, src, fs->sb.version) ? cfs_dirent_name(d, src, nm) : 0;
                    int run = nlen ? cfs_dirent_free_run(d, 1u + cfs_dirent_extra(nlen)) : -1;
                    if (run < 0) {
                        rc = -ENOSPC;
                    } else {
                        cfs_dirent_put(d, (unsigned)run, twin, CFS_TYPE_REG, nm, nlen);
                        rc = cfs_dir_write_block_at(fs, &in, 0, blk);
                        token = ino;
                    }
                }
            }
        }
        kfree(blk);
        break;
    }
    case COSMOFS_CORRUPT_SNAP_UUID: {
        /* Edited where it lies, as the other corruptions are: the fault
         * is in the committed list, not in a transaction's copy. */
        if (fs->sb.version < 12 || fs->sb.snap_root == 0) {
            rc = -ENOENT;
            break;
        }
        struct cfs_buf *b;
        rc = cfs_buf_get(fs, fs->sb.snap_root, CFS_KIND_SNAPLIST2, &b);
        if (rc)
            break;
        rc = -ENOENT;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK2; i++) {
            struct cfs_snapshot s;
            if (!cfs_snaplist_get(fs, b, i, &s))
                continue;
            memset(s.uuid, 0, sizeof(s.uuid));
            cfs_snaplist_set(fs, b, i, &s);
            cfs_buf_mark_dirty(fs, b);
            token = s.id;
            rc = 0;
            break;
        }
        cfs_buf_put(fs, b);
        break;
    }
    case COSMOFS_CORRUPT_ROLLBACK_CRC: {
        if (fs->sb.version < 12 || fs->sb.rollback_root == 0) {
            rc = -ENOENT;
            break;
        }
        struct cfs_buf *b;
        rc = cfs_buf_get(fs, fs->sb.rollback_root, CFS_KIND_ROLLBACK, &b);
        if (rc)
            break;
        /* The record's own CRC, not the block header's: the block still
         * verifies and what it says does not. */
        ((struct cfs_rollback *)(b->data + CFS_MHDR_SIZE))->crc ^= 0x5Au;
        cfs_buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
        token = fs->sb.rollback_root;
        break;
    }
    case COSMOFS_CORRUPT_COUNTER:
        /* Both of them, in opposite directions: the check must report one
         * finding per counter rather than one for "the superblock". */
        fs->free_blocks++;
        fs->sb.free_blocks++;
        if (fs->sb.inode_count > 0)
            fs->sb.inode_count--;
        token = fs->sb.free_blocks;
        break;
    case COSMOFS_CORRUPT_LONGNAME_EXTRA:
    case COSMOFS_CORRUPT_LONGNAME_CONT: {
        /* Version 15's two shapes the checker must refuse before reading a
         * name (CF13): a head whose `extra` is not what its length needs,
         * and continuations with no head. The named directory's first
         * long name, in its first block. */
        rc = cfs_inode_read(fs, ino, &in);
        if (rc == 0 && CFS_MODE_TYPE(in.mode) != CFS_TYPE_DIR)
            rc = -ENOTDIR;
        uint8_t *blk = rc == 0 ? kmalloc(CFS_BLOCK, 0) : NULL;
        if (rc == 0 && blk == NULL)
            rc = -ENOMEM;
        if (rc == 0)
            rc = cfs_dir_read_block_at(fs, &in, 0, blk);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)blk;
            unsigned s = 0;
            while (s < CFS_DIRENTS_PER_BLOCK && !(cfs_dirent_check(d, s, fs->sb.version) && d[s].extra > 0))
                s++;
            if (s == CFS_DIRENTS_PER_BLOCK) {
                rc = -ENOENT;   /* no long name to spoil */
            } else {
                token = d[s].ino;
                if (kind == COSMOFS_CORRUPT_LONGNAME_EXTRA)
                    d[s].extra--;
                else
                    memset(&d[s], 0, sizeof(d[s]));
                rc = cfs_dir_write_block_at(fs, &in, 0, blk);
            }
        }
        kfree(blk);
        break;
    }
    default:
        rc = -EINVAL;
        break;
    }
    mutex_unlock(&fs->lock);
    if (rc == 0 && what)
        *what = token;
    return rc;
}
