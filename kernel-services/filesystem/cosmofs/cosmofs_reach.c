/*
 * cosmofs_reach.c - Every block one tree reaches
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 12").
 *
 * The question a snapshot's allocation bitmap answers only approximately:
 * that bitmap is the pool's, as of the snapshot's commit, so it also marks
 * blocks other trees held then and blocks in flight in that commit's free
 * record. What a snapshot is worth, what a clone owns and what a rollback
 * leaves behind are questions about one tree, and this is the walk that
 * answers them exactly -- the structural check's walk, without its
 * accounting.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

struct reach {
    struct cfs *fs;
    void (*mark)(void *arg, uint64_t lin);
    void *arg;
    int rc;
    uint64_t first_bad;   /* the first block the walk could not use, for the log */
    const char *why;
};

static void reach_fail(struct reach *r, uint64_t dva, const char *why)
{
    if (r->rc == 0) {
        r->first_bad = dva;
        r->why = why;
    }
    r->rc = -EIO;
}

static void reach_mark(struct reach *r, uint64_t dva)
{
    uint64_t lin = cfs_dva_lin(r->fs, dva);
    if (lin == CFS_DVA_NONE) {
        reach_fail(r, dva, "outside the pool");   /* the tree is not sound */
        return;
    }
    r->mark(r->arg, lin);
}

static int reach_get(struct reach *r, uint64_t dva, uint32_t kind, struct cfs_buf **out)
{
    int rc = cfs_buf_get(r->fs, dva, kind, out);
    if (rc)
        reach_fail(r, dva, "unreadable");
    return rc;
}

static void reach_extent(struct reach *r, const struct cfs_extent *e)
{
    if (cfs_ext_count(e) == 0)
        return;
    uint32_t psize = cfs_ext_psize(e);
    for (uint32_t k = 0; k < psize; k++)
        reach_mark(r, e->start + k);
}

static void reach_inode(struct reach *r, const struct cfs_inode *in)
{
    for (unsigned i = 0; i < CFS_DIRECT; i++)
        reach_extent(r, &in->direct[i]);
    uint64_t next = in->indirect;
    unsigned guard = 0;
    while (next != 0) {
        if (guard++ > CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2) {
            reach_fail(r, next, "an extent chain too long");
            break;
        }
        reach_mark(r, next);
        struct cfs_buf *b;
        if (reach_get(r, next, CFS_KIND_EXTENTS, &b))
            break;
        const struct cfs_extent_block *eb = (const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_EXTENTS_PER_BLOCK; i++)
            reach_extent(r, &eb->ext[i]);
        next = eb->next;
        cfs_buf_put(r->fs, b);
    }
    if (in->csum_root != 0) {
        reach_mark(r, in->csum_root);
        struct cfs_buf *idx;
        if (reach_get(r, in->csum_root, CFS_KIND_CSUMIDX, &idx) == 0) {
            /* Copied out (on the heap: a block's worth): reading a leaf may
             * evict the index's buffer. */
            uint64_t *slots = kmalloc(CFS_PTRS_PER_BLOCK * sizeof(*slots), 0);
            if (slots == NULL) {
                cfs_buf_put(r->fs, idx);
                reach_fail(r, in->csum_root, "out of memory");
                return;
            }
            memcpy(slots, idx->data + CFS_MHDR_SIZE, CFS_PTRS_PER_BLOCK * sizeof(*slots));
            cfs_buf_put(r->fs, idx);
            for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++) {
                if (slots[i] == 0)
                    continue;
                reach_mark(r, slots[i]);
                /* Read, not only counted: a walk that says it reached a
                 * leaf has read it, or it says it could not. */
                struct cfs_buf *leaf;
                if (reach_get(r, slots[i], CFS_KIND_CSUM, &leaf) == 0)
                    cfs_buf_put(r->fs, leaf);
            }
            kfree(slots);
        }
    }
}

int cfs_tree_walk(struct cfs *fs, uint64_t imap_root, uint64_t next_ino,
                  void (*mark)(void *arg, uint64_t lin), void *arg)
{
    struct reach r = { .fs = fs, .mark = mark, .arg = arg, .rc = 0 };
    if (imap_root == 0)
        return 0;
    reach_mark(&r, imap_root);
    struct cfs_buf *b1;
    if (reach_get(&r, imap_root, CFS_KIND_IMAP1, &b1)) {
        kwarn("cosmofs: tree walk from 0x%llx: its inode map is unreadable", (unsigned long long)imap_root);
        return r.rc;
    }
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++) {
        uint64_t l0 = ((const uint64_t *)(b1->data + CFS_MHDR_SIZE))[i];
        if (l0 == 0)
            continue;
        reach_mark(&r, l0);
        struct cfs_buf *b0;
        if (reach_get(&r, l0, CFS_KIND_IMAP0, &b0))
            continue;
        for (unsigned j = 0; j < CFS_PTRS_PER_BLOCK; j++) {
            uint64_t ib = ((const uint64_t *)(b0->data + CFS_MHDR_SIZE))[j];
            if (ib == 0)
                continue;
            reach_mark(&r, ib);
            struct cfs_buf *bi;
            if (reach_get(&r, ib, CFS_KIND_INODES, &bi))
                continue;
            for (unsigned k = 0; k < CFS_INODES_PER_BLOCK; k++) {
                const struct cfs_inode *in =
                    (const struct cfs_inode *)(bi->data + CFS_MHDR_SIZE + (size_t)k * CFS_INODE_SIZE);
                if (in->ino == 0)
                    continue;
                /* A slot whose number is not its position, or past the
                 * tree's high-water mark, is not trusted for its blocks
                 * (the checker's rule, cosmofs_check.c walk_imap). */
                uint64_t expect = ((uint64_t)i * CFS_PTRS_PER_BLOCK + j) * CFS_INODES_PER_BLOCK + k;
                if (in->ino != expect || expect >= next_ino) {
                    reach_fail(&r, expect, "an inode slot out of place");
                    continue;
                }
                reach_inode(&r, in);
            }
            cfs_buf_put(fs, bi);
        }
        cfs_buf_put(fs, b0);
    }
    cfs_buf_put(fs, b1);
    if (r.rc)
        kwarn("cosmofs: tree walk from 0x%llx: %s at 0x%llx", (unsigned long long)imap_root, r.why,
              (unsigned long long)r.first_bad);
    return r.rc;
}
