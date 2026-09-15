/*
 * cosmofs_check.c - Do the blocks add up?
 * (docs/audit/next-subsystem-fsck.md; docs/kernel-services/filesystem/
 * cosmofs/design.md, "The structural check".)
 *
 * The scrub answers whether every block the filesystem can name is still
 * the block that was written. This answers the other question: whether
 * what the structures claim agrees with what the allocator believes,
 * whether an inode's link count matches the entries that name it, and
 * whether anything is reachable from nowhere.
 *
 * Two maps, and the difference between them is the whole design. `seen`
 * is a union over everything -- the live generation and every snapshot
 * -- and is what the allocation bitmap is compared against, because a
 * block a snapshot holds is not free. `live` is the live generation's
 * own claims, and is the only map in which a second claim means
 * corruption: a snapshot sharing a block with the live tree is the
 * normal case and must never be reported.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/pmm.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include "cosmofs_internal.h"


/* One page of bits covers this many blocks; the maps are arrays of those
 * pages, because a filesystem large enough to matter needs more bits
 * than KMALLOC_MAX_SIZE (4 MiB) can hold in one piece. */
#define BITS_PER_CHUNK (CFS_BLOCK * 8u)

/* No metadata chain in a sound filesystem is this long; one that is has
 * a cycle, and the walk says so rather than following it forever. */
#define CFS_CHECK_MAX_CHAIN  4096u
/* A directory tree deeper than this is a cycle the reachability map did
 * not catch, or a filesystem no path could name anyway (VFS_MAX_COMPONENTS). */
#define CFS_CHECK_MAX_DEPTH  64u

struct bitmap {
    uint8_t **chunk;
    unsigned nchunks;
};

static void bitmap_free(struct bitmap *m)
{
    if (m->chunk != NULL) {
        for (unsigned i = 0; i < m->nchunks; i++)
            if (m->chunk[i] != NULL)
                pmm_free_page(phys_to_page(virt_to_phys(m->chunk[i])));
        kfree(m->chunk);
    }
    m->chunk = NULL;
    m->nchunks = 0;
}

static int bitmap_alloc(struct bitmap *m, uint64_t bits)
{
    m->nchunks = (unsigned)((bits + BITS_PER_CHUNK - 1) / BITS_PER_CHUNK);
    if (m->nchunks == 0)
        m->nchunks = 1;
    m->chunk = kzalloc((size_t)m->nchunks * sizeof(*m->chunk));
    if (m->chunk == NULL)
        return -ENOMEM;
    for (unsigned i = 0; i < m->nchunks; i++) {
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (pg == NULL) {
            bitmap_free(m);
            return -ENOMEM;
        }
        m->chunk[i] = phys_to_virt(page_to_phys(pg));
    }
    return 0;
}

static bool bitmap_test(const struct bitmap *m, uint64_t bit)
{
    unsigned c = (unsigned)(bit / BITS_PER_CHUNK);
    if (c >= m->nchunks)
        return false;
    uint64_t i = bit % BITS_PER_CHUNK;
    return (m->chunk[c][i / 8] & (1u << (i % 8))) != 0;
}

/* Returns true if the bit was already set. */
static bool bitmap_set(struct bitmap *m, uint64_t bit)
{
    unsigned c = (unsigned)(bit / BITS_PER_CHUNK);
    if (c >= m->nchunks)
        return false;
    uint64_t i = bit % BITS_PER_CHUNK;
    uint8_t mask = (uint8_t)(1u << (i % 8));
    bool had = (m->chunk[c][i / 8] & mask) != 0;
    m->chunk[c][i / 8] |= mask;
    return had;
}

/* The counted link counts, chunked the same way: one 32-bit count per
 * inode number the map can reach. */
#define COUNTS_PER_CHUNK (CFS_BLOCK / sizeof(uint32_t))

struct counts {
    uint32_t **chunk;
    unsigned nchunks;
};

static void counts_free(struct counts *c)
{
    if (c->chunk != NULL) {
        for (unsigned i = 0; i < c->nchunks; i++)
            if (c->chunk[i] != NULL)
                pmm_free_page(phys_to_page(virt_to_phys(c->chunk[i])));
        kfree(c->chunk);
    }
    c->chunk = NULL;
    c->nchunks = 0;
}

static int counts_alloc(struct counts *c, uint64_t n)
{
    c->nchunks = (unsigned)((n + COUNTS_PER_CHUNK - 1) / COUNTS_PER_CHUNK);
    if (c->nchunks == 0)
        c->nchunks = 1;
    c->chunk = kzalloc((size_t)c->nchunks * sizeof(*c->chunk));
    if (c->chunk == NULL)
        return -ENOMEM;
    for (unsigned i = 0; i < c->nchunks; i++) {
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (pg == NULL) {
            counts_free(c);
            return -ENOMEM;
        }
        c->chunk[i] = phys_to_virt(page_to_phys(pg));
    }
    return 0;
}

static uint32_t counts_get(const struct counts *c, uint64_t i)
{
    unsigned k = (unsigned)(i / COUNTS_PER_CHUNK);
    return k < c->nchunks ? c->chunk[k][i % COUNTS_PER_CHUNK] : 0;
}

static void counts_add(struct counts *c, uint64_t i, uint32_t n)
{
    unsigned k = (unsigned)(i / COUNTS_PER_CHUNK);
    if (k < c->nchunks)
        c->chunk[k][i % COUNTS_PER_CHUNK] += n;
}

/* --- the walk's state ------------------------------------------------------ */

struct check {
    struct cfs *fs;
    struct cosmofs_check_report *rep;
    struct bitmap seen;    /* claimed by anything, live or snapshot */
    struct bitmap live;    /* claimed by the live generation */
    struct bitmap reach;   /* inode numbers reachable from the root */
    struct bitmap alive;   /* inode numbers the map holds, whatever their nlink */
    struct counts links;   /* directory entries naming each inode */
    struct counts nlink;   /* what each inode says its link count is */
    unsigned flags;
    /* Which counters the comparison found wrong, so that repair fixes
     * those and only those: the orphan repair changes `inode_count`
     * itself, and a blind write-back of the counted total afterwards
     * would put the stale number back. */
    bool free_wrong;
    bool inodes_wrong;
};

static void name_it(struct cosmofs_check_class *cl, uint64_t what)
{
    cl->count++;
    if (cl->named < CFS_CHECK_NAMES)
        cl->name[cl->named++] = what;
}

/* Claim `dva` for the live generation (live = true) or for a snapshot.
 * A second live claim is the cross-link this checker exists to find; a
 * snapshot claiming a live block is the normal case and says nothing. */
static void claim(struct check *ck, uint64_t dva, bool live)
{
    uint64_t lin = cfs_dva_lin(ck->fs, dva);
    if (lin == CFS_DVA_NONE) {
        name_it(&ck->rep->dir_bad, dva);   /* a pointer outside the pool */
        return;
    }
    bitmap_set(&ck->seen, lin);
    if (live && bitmap_set(&ck->live, lin))
        name_it(&ck->rep->dup, dva);
}

static int read_meta(struct check *ck, uint64_t dva, uint32_t kind, struct cfs_buf **out)
{
    int rc = cfs_buf_get(ck->fs, dva, kind, out);
    if (rc) {
        name_it(&ck->rep->unreadable, dva);
        ck->rep->partial = true;
    }
    return rc;
}

/* --- the structures -------------------------------------------------------- */

/* The allocation index and its chunks, for one member. */
static void walk_alloc(struct check *ck, uint64_t root, bool live)
{
    if (root == 0)
        return;
    claim(ck, root, live);
    struct cfs_buf *b;
    if (read_meta(ck, root, CFS_KIND_ALLOCIDX, &b))
        return;
    const uint64_t *slot = (const uint64_t *)(b->data + CFS_MHDR_SIZE);
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
        if (slot[i] != 0)
            claim(ck, slot[i], live);
    cfs_buf_put(ck->fs, b);
}

/* Every block one extent names. A physical size of zero is a hole. */
static void claim_extent(struct check *ck, const struct cfs_extent *e, bool live)
{
    if (cfs_ext_count(e) == 0)
        return;
    uint32_t psize = cfs_ext_psize(e);
    for (uint32_t k = 0; k < psize; k++)
        claim(ck, e->start + k, live);
}

/*
 * The chain of extent blocks hanging off an inode: each block is claimed,
 * the extents in it are claimed, and the chain is bounded. One pass, not
 * two -- an earlier version claimed the chain and then read it again for
 * its extents, which read every extent block twice and left the second
 * read reporting nothing when it failed.
 */
static void walk_extent_chain(struct check *ck, uint64_t head, bool live)
{
    uint64_t next = head;
    unsigned guard = 0;
    while (next != 0) {
        if (guard++ > CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2) {
            name_it(&ck->rep->chain_cycle, next);
            return;
        }
        claim(ck, next, live);
        struct cfs_buf *b;
        if (read_meta(ck, next, CFS_KIND_EXTENTS, &b))
            return;
        const struct cfs_extent_block *eb = (const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_EXTENTS_PER_BLOCK; i++)
            claim_extent(ck, &eb->ext[i], live);
        next = eb->next;
        cfs_buf_put(ck->fs, b);
    }
}

static void walk_csum_tree(struct check *ck, uint64_t root, bool live)
{
    if (root == 0)
        return;
    claim(ck, root, live);
    struct cfs_buf *idx;
    if (read_meta(ck, root, CFS_KIND_CSUMIDX, &idx))
        return;
    const uint64_t *slot = (const uint64_t *)(idx->data + CFS_MHDR_SIZE);
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
        if (slot[i] != 0)
            claim(ck, slot[i], live);
    cfs_buf_put(ck->fs, idx);
}

/* One inode's trees and every data block its extents name. */
static void walk_inode_blocks(struct check *ck, const struct cfs_inode *in, bool live)
{
    for (unsigned i = 0; i < CFS_DIRECT; i++)
        claim_extent(ck, &in->direct[i], live);
    walk_extent_chain(ck, in->indirect, live);
    walk_csum_tree(ck, in->csum_root, live);
}

/*
 * Every inode the map names, through one imap tree. `live` says whether
 * the claims belong to the live generation; `count_them` says whether
 * the inodes are this filesystem's own (a snapshot's inodes are not
 * counted toward inode_count or nlink).
 */
static void walk_imap(struct check *ck, uint64_t imap_root, bool live, bool count_them)
{
    if (imap_root == 0)
        return;
    claim(ck, imap_root, live);
    struct cfs_buf *b1;
    if (read_meta(ck, imap_root, CFS_KIND_IMAP1, &b1))
        return;
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++) {
        uint64_t l0 = ((const uint64_t *)(b1->data + CFS_MHDR_SIZE))[i];
        if (l0 == 0)
            continue;
        claim(ck, l0, live);
        struct cfs_buf *b0;
        if (read_meta(ck, l0, CFS_KIND_IMAP0, &b0))
            continue;
        for (unsigned j = 0; j < CFS_PTRS_PER_BLOCK; j++) {
            uint64_t ib = ((const uint64_t *)(b0->data + CFS_MHDR_SIZE))[j];
            if (ib == 0)
                continue;
            claim(ck, ib, live);
            struct cfs_buf *bi;
            if (read_meta(ck, ib, CFS_KIND_INODES, &bi))
                continue;
            for (unsigned k = 0; k < CFS_INODES_PER_BLOCK; k++) {
                const struct cfs_inode *in =
                    (const struct cfs_inode *)(bi->data + CFS_MHDR_SIZE + (size_t)k * CFS_INODE_SIZE);
                if (in->ino == 0)
                    continue;
                /*
                 * A slot's own number must be the number its position in
                 * the map means (`cfs_inode_block_index`, `cfs_imap_*`).
                 * One that disagrees, or that sits past the high-water
                 * mark, is a malformed inode: its blocks are not claimed
                 * from a structure the pass has just called untrustworthy,
                 * its accounting would be dropped or merged by maps sized
                 * on `next_ino`, and the answer is marked incomplete
                 * rather than acted on.
                 */
                uint64_t expect = ((uint64_t)i * CFS_PTRS_PER_BLOCK + j) * CFS_INODES_PER_BLOCK + k;
                if (in->ino != expect || expect >= ck->fs->sb.next_ino) {
                    name_it(&ck->rep->dir_bad, expect);
                    ck->rep->partial = true;
                    continue;
                }
                /* Unlike the scrub, an inode with no links is exactly
                 * what this pass is looking for: its blocks are still
                 * claimed and no name reaches it. */
                walk_inode_blocks(ck, in, live);
                if (count_them) {
                    ck->rep->inodes_seen++;
                    /* Recorded here rather than read back later:
                     * cfs_inode_read reports a slot with no links as
                     * absent, which is exactly the orphan this pass
                     * exists to find. */
                    bitmap_set(&ck->alive, in->ino);
                    counts_add(&ck->nlink, in->ino, in->nlink);
                }
            }
            cfs_buf_put(ck->fs, bi);
        }
        cfs_buf_put(ck->fs, b0);
    }
    cfs_buf_put(ck->fs, b1);
}

static void walk_snapshots(struct check *ck)
{
    uint64_t list = ck->fs->sb.snap_root;
    unsigned guard = 0;
    while (list != 0) {
        if (guard++ > CFS_CHECK_MAX_CHAIN) {
            name_it(&ck->rep->chain_cycle, list);
            return;
        }
        claim(ck, list, true);   /* the list itself belongs to the live generation */
        struct cfs_buf *b;
        if (read_meta(ck, list, CFS_KIND_SNAPLIST, &b))
            return;
        const struct cfs_snap_block *sb = (const struct cfs_snap_block *)(b->data + CFS_MHDR_SIZE);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            const struct cfs_snapshot *s = &sb->snap[i];
            if (s->id == 0)
                continue;
            ck->rep->snapshots_seen++;
            /* A snapshot's trees are seen, never live: it holds blocks
             * the live tree also holds, on purpose. */
            walk_imap(ck, s->imap_root, false, false);
            /* From version 4 a snapshot's alloc_root is the *member
             * table* it was taken with, and each member's own index
             * hangs off that (cosmofs_snap.c, snap_alloc_root); before
             * it, the single member's index directly. Reading it as an
             * index is how this walk first reported a snapshot's own
             * member table as unreadable. */
            if (ck->fs->sb.version < 4) {
                walk_alloc(ck, s->alloc_root, false);
            } else if (s->alloc_root != 0) {
                claim(ck, s->alloc_root, false);
                struct cfs_buf *mb;
                if (read_meta(ck, s->alloc_root, CFS_KIND_MEMBERS, &mb) == 0) {
                    const struct cfs_member_block *t =
                        (const struct cfs_member_block *)(mb->data + CFS_MHDR_SIZE);
                    /* A count past the block is the table being wrong,
                     * not the snapshot holding nothing: say so, and walk
                     * what the block can hold, or every member's blocks
                     * would be reported as leaked instead. */
                    unsigned n = t->count;
                    if (n > CFS_MEMBERS_PER_BLOCK) {
                        name_it(&ck->rep->dir_bad, s->alloc_root);
                        n = CFS_MEMBERS_PER_BLOCK;
                    }
                    for (unsigned v = 0; v < n; v++)
                        walk_alloc(ck, t->m[v].alloc_root, false);
                    cfs_buf_put(ck->fs, mb);
                }
            }
            uint64_t dl = s->deadlist;
            unsigned dguard = 0;
            while (dl != 0) {
                if (dguard++ > CFS_CHECK_MAX_CHAIN) {
                    name_it(&ck->rep->chain_cycle, dl);
                    break;
                }
                claim(ck, dl, false);
                struct cfs_buf *db;
                if (read_meta(ck, dl, CFS_KIND_DEADLIST, &db))
                    break;
                const struct cfs_dead_block *d = (const struct cfs_dead_block *)(db->data + CFS_MHDR_SIZE);
                for (uint32_t k = 0; k < d->count && k < CFS_DEAD_PER_BLOCK; k++)
                    claim(ck, d->blk[k], false);
                dl = d->next;
                cfs_buf_put(ck->fs, db);
            }
        }
        cfs_buf_put(ck->fs, b);
        list = next;
    }
}

/* --- the directory tree ---------------------------------------------------- */

static bool inode_exists(struct check *ck, uint64_t ino, struct cfs_inode *out)
{
    return ino != 0 && cfs_inode_read(ck->fs, ino, out) == 0;
}

/* Every entry of one directory, counting references and validating shape.
 * Recursion is bounded by the tree's own depth; a cycle is caught by the
 * reachability map, which refuses to walk an inode twice. */
static void walk_dir(struct check *ck, uint64_t ino, const struct cfs_inode *dir, unsigned depth)
{
    if (depth > CFS_CHECK_MAX_DEPTH) {
        name_it(&ck->rep->chain_cycle, ino);
        return;
    }
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL) {
        ck->rep->partial = true;
        return;
    }
    ck->rep->dirs_seen++;
    uint64_t blocks = (dir->size + CFS_BLOCK - 1) / CFS_BLOCK;
    for (uint64_t lblk = 0; lblk < blocks; lblk++) {
        if (cfs_dir_read_block_at(ck->fs, dir, lblk, block) != 0) {
            name_it(&ck->rep->unreadable, ino);
            ck->rep->partial = true;
            continue;
        }
        const struct cfs_dirent *d = (const struct cfs_dirent *)block;
        for (unsigned s = 0; s < CFS_DIRENTS_PER_BLOCK; s++) {
            if (d[s].ino == 0)
                continue;
            if (d[s].namelen == 0 || d[s].namelen > CFS_NAME_MAX) {
                name_it(&ck->rep->dir_bad, ino);
                continue;
            }
            struct cfs_inode child;
            if (!inode_exists(ck, d[s].ino, &child)) {
                name_it(&ck->rep->dangling_entry, d[s].ino);
                continue;
            }
            if (CFS_MODE_TYPE(child.mode) != d[s].type) {
                name_it(&ck->rep->dir_bad, d[s].ino);
                continue;
            }
            /*
             * A directory's link count is three things, and cosmofs
             * stores none of them as entries: the entry in its parent
             * (here), its own "." (below), and one per subdirectory's
             * ".." (credited to the parent, also below). A file's is
             * just the entries that name it.
             */
            counts_add(&ck->links, d[s].ino, 1);
            if (d[s].type == CFS_TYPE_DIR) {
                counts_add(&ck->links, d[s].ino, 1);   /* its own "." */
                counts_add(&ck->links, ino, 1);        /* its ".." names this directory */
                if (!bitmap_set(&ck->reach, d[s].ino))
                    walk_dir(ck, d[s].ino, &child, depth + 1);
                else
                    name_it(&ck->rep->dir_bad, d[s].ino);   /* a directory with two parents */
            } else {
                bitmap_set(&ck->reach, d[s].ino);
            }
        }
    }
    kfree(block);
}

/* --- the comparison -------------------------------------------------------- */

static void compare(struct check *ck)
{
    struct cfs *fs = ck->fs;
    uint64_t counted_free = 0;

    for (uint64_t lin = 0; lin < fs->nblocks; lin++) {
        uint64_t dva = cfs_lin_dva(fs, lin);
        if (dva == CFS_DVA_NONE)
            continue;                       /* padding between members */
        bool allocated = cfs_bitmap_test(fs, lin);
        bool seen = bitmap_test(&ck->seen, lin);
        if (allocated && !seen)
            name_it(&ck->rep->alloc_not_seen, dva);
        else if (!allocated && seen)
            name_it(&ck->rep->seen_not_alloc, dva);
        if (!allocated)
            counted_free++;
        if (seen)
            ck->rep->blocks_seen++;
    }

    /* Link counts and orphans, over the inodes the map itself holds. */
    uint64_t counted_inodes = 0;
    for (uint64_t ino = CFS_ROOT_INO; ino <= fs->sb.next_ino; ino++) {
        if (!bitmap_test(&ck->alive, ino))
            continue;                       /* no such inode: nothing to say */
        /*
         * Every slot the map holds, orphans included. The superblock's
         * count follows the slot and not the name: it is decremented
         * where the slot is zeroed, when the last handle on an unlinked
         * inode goes (`cosmofs.c`, the evict path), not at the unlink.
         * Counting links here instead would call every crash-stranded
         * orphan a wrong total as well, and report one fault as two.
         */
        counted_inodes++;
        uint32_t named = counts_get(&ck->links, ino);
        if (ino == CFS_ROOT_INO)
            named += 2;                     /* the root is its own parent, and nothing names it */
        if (named == 0)
            name_it(&ck->rep->orphan, ino);
        else if (counts_get(&ck->nlink, ino) != named)
            name_it(&ck->rep->nlink_wrong, ino);
    }

    /* One finding per counter that disagrees, named by what the walk
     * counted, so a report of two is two wrong totals and not one twice. */
    ck->free_wrong = counted_free != fs->free_blocks;
    ck->inodes_wrong = counted_inodes != fs->sb.inode_count;
    if (ck->free_wrong)
        name_it(&ck->rep->counter_wrong, counted_free);
    if (ck->inodes_wrong)
        name_it(&ck->rep->counter_wrong, counted_inodes);
    ck->rep->counted_free = counted_free;
    ck->rep->counted_inodes = counted_inodes;
}

/* --- repair ---------------------------------------------------------------- */

/*
 * Whether the walk's reachability is worth acting on. Every repair this
 * pass performs is an argument from absence: a block nothing claimed, an
 * inode no name reached, a link count no entry supported. If the walk
 * could not read a block, or met an entry it had to skip, then "nothing
 * reaches this" may only mean "this pass did not get there" -- and
 * freeing those blocks destroys a live file.
 *
 * `cosmofs-check-partial` is the proof: one unreadable directory block
 * turns every file named inside it into an orphan whose blocks look
 * leaked. A `dir_bad` entry does the same without being unreadable,
 * because the walk skips the entry and the inode it named loses its only
 * name; so does a `dangling_entry`, which is an entry whose inode number
 * was overwritten, leaving the inode it used to name unreferenced.
 *
 * So repair runs only on an answer the walk is sure of, and says when it
 * did not.
 */
static bool reachability_is_sound(const struct cosmofs_check_report *r)
{
    return !r->partial && r->unreadable.count == 0 && r->dir_bad.count == 0 &&
           r->dangling_entry.count == 0 && r->chain_cycle.count == 0;
}

static int repair(struct check *ck)
{
    struct cfs *fs = ck->fs;
    int rc = 0;

    if (!reachability_is_sound(ck->rep)) {
        ck->rep->repair_refused = true;
        return 0;   /* not an error: a refusal, and the report says why */
    }

    /* A block nothing reaches: give it back. */
    if (ck->rep->alloc_not_seen.count != 0) {
        for (uint64_t lin = 0; lin < fs->nblocks && rc == 0; lin++) {
            uint64_t dva = cfs_lin_dva(fs, lin);
            if (dva == CFS_DVA_NONE || bitmap_test(&ck->seen, lin) || !cfs_bitmap_test(fs, lin))
                continue;
            cfs_free_block_deferred(fs, dva);
            ck->rep->alloc_not_seen.repaired++;
        }
    }

    /*
     * An inode no name reaches: free what it holds and clear the slot.
     * Over the maps and not the report's names, which stop at
     * CFS_CHECK_NAMES: a filesystem with nine orphans must not be left
     * with one, and the eight names are a diagnosis for a reader, not a
     * work list.
     */
    for (uint64_t ino = CFS_ROOT_INO; ino <= fs->sb.next_ino && rc == 0; ino++) {
        if (!bitmap_test(&ck->alive, ino))
            continue;
        uint32_t named_by = counts_get(&ck->links, ino);
        if (ino == CFS_ROOT_INO)
            named_by += 2;
        if (named_by != 0)
            continue;
        struct cfs_inode in;
        if (cfs_inode_read_raw(fs, ino, &in) != 0)
            continue;   /* raw: an orphan has no links, which the ordinary read calls absent */
        rc = cfs_truncate_blocks(fs, &in, 0);
        if (rc == 0) {
            /* Every field, the number included: a slot whose `ino` still
             * matches reads as an inode, and the next pass would report
             * the same orphan again. */
            struct cfs_inode zero;
            memset(&zero, 0, sizeof(zero));
            rc = cfs_inode_write(fs, ino, &zero);
        }
        if (rc == 0) {
            if (fs->sb.inode_count > 0)
                fs->sb.inode_count--;
            ck->rep->orphan.repaired++;
        }
    }

    /* A link count the directory entries disagree with: the entries win.
     * Over the maps, for the same reason as the orphans above. */
    for (uint64_t ino = CFS_ROOT_INO; ino <= fs->sb.next_ino && rc == 0; ino++) {
        if (!bitmap_test(&ck->alive, ino))
            continue;
        uint32_t named = counts_get(&ck->links, ino);
        if (ino == CFS_ROOT_INO)
            named += 2;
        if (named == 0 || counts_get(&ck->nlink, ino) == named)
            continue;   /* no name at all is an orphan, repaired above */
        struct cfs_inode in;
        if (cfs_inode_read(fs, ino, &in) != 0)
            continue;
        in.nlink = named;
        rc = cfs_inode_write(fs, ino, &in);
        if (rc == 0)
            ck->rep->nlink_wrong.repaired++;
    }

    /*
     * The totals, and only the ones the comparison disagreed with. The
     * repairs above move both counters themselves -- freeing a leaked
     * block raises the free count, clearing an orphan lowers the inode
     * count -- so "write back what the walk counted" would undo the
     * repair that just ran. One repair per counter that was wrong when
     * it was read, so `repaired` can be compared with `count`.
     */
    if (rc == 0 && ck->free_wrong) {
        fs->free_blocks = ck->rep->counted_free;
        fs->sb.free_blocks = ck->rep->counted_free;
        ck->rep->counter_wrong.repaired++;
    }
    if (rc == 0 && ck->inodes_wrong) {
        fs->sb.inode_count = ck->rep->counted_inodes;
        ck->rep->counter_wrong.repaired++;
    }
    return rc;
}

/* --- the pass -------------------------------------------------------------- */

static bool report_clean(const struct cosmofs_check_report *r)
{
    return r->alloc_not_seen.count == 0 && r->seen_not_alloc.count == 0 && r->dup.count == 0 &&
           r->nlink_wrong.count == 0 && r->orphan.count == 0 && r->dangling_entry.count == 0 &&
           r->dir_bad.count == 0 && r->counter_wrong.count == 0 && r->chain_cycle.count == 0 &&
           r->unreadable.count == 0;
}

int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;

    struct cosmofs_check_report rep;
    memset(&rep, 0, sizeof(rep));
    struct check ck = { .fs = fs, .rep = &rep, .flags = flags };
    uint64_t t0 = clock_now_ns();

    mutex_lock(&fs->lock);
    if (fs->failed) {
        mutex_unlock(&fs->lock);
        return -EIO;
    }
    int rc = bitmap_alloc(&ck.seen, fs->nblocks);
    if (rc == 0)
        rc = bitmap_alloc(&ck.live, fs->nblocks);
    if (rc == 0)
        rc = bitmap_alloc(&ck.reach, fs->sb.next_ino + 2);
    if (rc == 0)
        rc = bitmap_alloc(&ck.alive, fs->sb.next_ino + 2);
    if (rc == 0)
        rc = counts_alloc(&ck.links, fs->sb.next_ino + 2);
    if (rc == 0)
        rc = counts_alloc(&ck.nlink, fs->sb.next_ino + 2);
    if (rc) {
        mutex_unlock(&fs->lock);
        bitmap_free(&ck.seen);
        bitmap_free(&ck.live);
        bitmap_free(&ck.reach);
        bitmap_free(&ck.alive);
        counts_free(&ck.links);
        counts_free(&ck.nlink);
        return rc;   /* -ENOMEM before the walk, never half way through it */
    }
    rep.bytes_allocated = (uint64_t)(ck.seen.nchunks + ck.live.nchunks + ck.reach.nchunks + ck.alive.nchunks +
                                     ck.links.nchunks + ck.nlink.nchunks) * CFS_BLOCK;

    /*
     * Blocks this transaction released are still marked allocated until
     * the next commit makes them reusable (cosmofs_core.c, the deferred
     * free list): allocated, reachable from nothing, and correct. They
     * are claimed here so the walk does not call the filesystem's own
     * copy-on-write history a leak -- which is what the first run of
     * cosmofs-check-faults did, three blocks at a time.
     */
    for (unsigned i = 0; i < fs->nr_pending; i++)
        claim(&ck, fs->pending_free[i], true);

    /* The blocks no structure points at but every mount needs: the
     * superblock slots and, on each member, the label. */
    for (unsigned v = 0; v < fs->nmembers; v++)
        for (uint64_t b = 0; b < fs->mem[v].first_usable; b++)
            claim(&ck, CFS_DVA(v, b), true);

    walk_imap(&ck, fs->sb.imap_root, true, true);
    for (unsigned v = 0; v < fs->nmembers; v++)
        walk_alloc(&ck, fs->mem[v].alloc_root, true);
    if (fs->sb.members != 0)
        claim(&ck, fs->sb.members, true);
    if (fs->sb.key_root != 0)
        claim(&ck, fs->sb.key_root, true);
    walk_snapshots(&ck);

    struct cfs_inode root;
    if (cfs_inode_read(fs, CFS_ROOT_INO, &root) == 0) {
        bitmap_set(&ck.reach, CFS_ROOT_INO);
        walk_dir(&ck, CFS_ROOT_INO, &root, 0);
    } else {
        name_it(&rep.unreadable, CFS_ROOT_INO);
        rep.partial = true;
    }

    compare(&ck);
    if ((flags & COSMOFS_CHECK_REPAIR) != 0)
        rc = repair(&ck);

    mutex_unlock(&fs->lock);
    bitmap_free(&ck.seen);
    bitmap_free(&ck.live);
    bitmap_free(&ck.reach);
    bitmap_free(&ck.alive);
    counts_free(&ck.links);
    counts_free(&ck.nlink);

    rep.clean = report_clean(&rep);
    rep.elapsed_ns = clock_now_ns() - t0;
    if (out)
        *out = rep;
    if (!rep.clean && rep.unreadable.count != 0)
        kwarn("cosmofs: check: first unreadable block 0x%llx",
              (unsigned long long)rep.unreadable.name[0]);
    if (!rep.clean)
        kwarn("cosmofs: check: %llu leaked, %llu free-in-use, %llu cross-linked, %llu bad nlink, %llu orphan, "
              "%llu dangling, %llu bad entries, %llu counters, %llu cycles, %llu unreadable",
              (unsigned long long)rep.alloc_not_seen.count, (unsigned long long)rep.seen_not_alloc.count,
              (unsigned long long)rep.dup.count, (unsigned long long)rep.nlink_wrong.count,
              (unsigned long long)rep.orphan.count, (unsigned long long)rep.dangling_entry.count,
              (unsigned long long)rep.dir_bad.count, (unsigned long long)rep.counter_wrong.count,
              (unsigned long long)rep.chain_cycle.count, (unsigned long long)rep.unreadable.count);
    return rc;
}

