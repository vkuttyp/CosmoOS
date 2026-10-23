/*
 * cosmofs_member.c - The pool's members, and the addresses that name them
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 4:
 * many members").
 *
 * Every pointer on disk is a DVA: a member and a block on it. This file
 * owns the three things that follow from that -- the table that says
 * which members exist, the labels that let a mount find their devices,
 * and the conversion between a DVA and the linear index the allocator's
 * one flat bitmap is built on.
 */

#include <kernel/blk.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- addresses ----------------------------------------------------------- */

bool cfs_dva_valid(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return false;
    uint64_t b = CFS_DVA_BLK(dva);
    return b >= fs->mem[v].first_usable && b < fs->mem[v].nblocks;
}

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return CFS_DVA_NONE;
    uint64_t b = CFS_DVA_BLK(dva);
    if (b >= fs->mem[v].nblocks)
        return CFS_DVA_NONE;
    return fs->mem[v].base + b;
}

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        const struct cfs_memstate *m = &fs->mem[v];
        if (lin < m->base)
            break;
        if (lin < m->base + m->nblocks)
            return CFS_DVA(v, lin - m->base);
        /* Between a member's last block and the end of its last bitmap
         * chunk is padding: an address with no block behind it. */
        if (lin < m->base + (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP)
            return CFS_DVA_NONE;
    }
    return CFS_DVA_NONE;
}

/* --- labels -------------------------------------------------------------- */

static uint32_t label_crc(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_label, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

/* A label's seal: its CRC32C, or on a version-14 encrypted pool the tag
 * in the CRC and the word after it (design.md, "The metadata MAC"). */
static void label_seal(const struct cfs_mac *mac, uint8_t *block)
{
    struct cfs_label *l = (struct cfs_label *)block;
    if (mac != NULL && mac->on) {
        cfs_mac_seal(mac, CFS_MAC_LABEL, block, offsetof(struct cfs_label, crc));
        return;
    }
    l->crc = label_crc(block);
}

static bool label_sealed(const struct cfs_mac *mac, const uint8_t *block)
{
    const struct cfs_label *l = (const struct cfs_label *)block;
    if (mac != NULL && mac->on)
        return cfs_mac_ok(mac, CFS_MAC_LABEL, block, offsetof(struct cfs_label, crc));
    return label_crc(block) == l->crc;
}

int cfs_label_write(struct spool *pool, const struct cfs_mac *mac, unsigned vdev, unsigned copy, uint64_t generation,
                    const uint8_t uuid[16], uint64_t nblocks)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_label *l = (struct cfs_label *)block;
    memcpy(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic));
    l->version = CFS_FORMAT_VERSION;   /* what a format writes; the label does not change with a raise */
    l->index = vdev;
    l->copy = copy;
    l->generation = generation;
    memcpy(l->uuid, uuid, 16);
    l->nblocks = nblocks;
    label_seal(mac, block);
    /* To that copy alone: every device of a group carries its own. */
    int rc = pool_write_copy(pool, CFS_DVA(vdev, 0), copy, block);
    kfree(block);
    return rc;
}

/*
 * A label written straight to a device that is not (yet) one of the
 * pool's copies, or is being rebuilt as one: a resilver's target
 * (CFS_LABEL_RESILVER) or a spare (CFS_LABEL_SPARE), found again after a
 * reboot by `tag` (design.md, "Resilvering"). A member-0 target's block 1
 * is cleared too: what it holds is an older life's superblock, and until
 * the resilver ends it must not look like a copy of anything.
 */
int cfs_label_write_dev(struct cfs *fs, struct blkdev *bd, unsigned vdev, uint64_t flags, uint64_t tag,
                        uint64_t nblocks)
{
    struct spool *p;
    int rc = pool_open(bd, &p);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(p);
        return -ENOMEM;
    }
    struct cfs_label *l = (struct cfs_label *)block;
    memcpy(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic));
    l->version = 13;
    l->index = vdev;
    memcpy(l->uuid, fs->sb.uuid, 16);
    l->nblocks = nblocks;
    l->generation = fs->sb.generation;
    l->flags = flags;
    l->tag = tag;
    label_seal(&fs->mac, block);
    rc = pool_write(p, CFS_DVA(0, 0), block);
    if (rc == 0 && (flags & CFS_LABEL_RESILVER) && vdev == 0) {
        memset(block, 0, CFS_BLOCK);
        rc = pool_write(p, CFS_DVA(0, 1), block);
    }
    if (rc == 0)
        rc = pool_flush(p);
    kfree(block);
    pool_close(p);
    return rc;
}

/* A device that stops being the pool's (detached, replaced, a spare
 * removed): its label and superblock slots are cleared, so no later
 * mount takes it for a copy. Best effort: a device that is gone is gone. */
void cfs_label_clear_dev(struct blkdev *bd)
{
    struct spool *p;
    if (bd == NULL || bd->gone || pool_open(bd, &p))
        return;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block) {
        (void)pool_write(p, CFS_DVA(0, 0), block);
        (void)pool_write(p, CFS_DVA(0, 1), block);
        (void)pool_flush(p);
        kfree(block);
    }
    pool_close(p);
}

/* The label at block 0 of `bd`, or -ENOENT when there is none. A device
 * that is not a member of anything reads as random data here, so every
 * field is checked before it is believed. */
static int label_read(const struct cfs_mac *mac, struct blkdev *bd, struct cfs_label *out)
{
    struct spool *p;
    int rc = pool_open(bd, &p);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(p);
        return -ENOMEM;
    }
    rc = pool_read(p, CFS_DVA(0, 0), block);
    if (rc == 0) {
        const struct cfs_label *l = (const struct cfs_label *)block;
        if (memcmp(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic)) != 0 || !label_sealed(mac, block) ||
            l->index >= CFS_MAX_MEMBERS || l->copy >= CFS_MAX_COPIES)
            rc = -ENOENT;
        else
            *out = *l;
    }
    kfree(block);
    pool_close(p);
    return rc;
}

/*
 * A device is current if its label names the generation being mounted --
 * or the one after it. Labels are stamped after a commit's blocks are
 * stable and before the root that publishes them, so a commit
 * interrupted between the two leaves labels one ahead of the durable
 * root. Those devices did take part in everything that root names; a
 * device that *missed* a commit is the one that is behind, and treating
 * an interrupted commit as staleness would degrade a healthy mirror for
 * nothing.
 */
static bool label_current(const struct cfs *fs, const struct cfs_label *l)
{
    return l->generation >= fs->sb.generation;
}

/* True if `bd` carries a current copy of member `vdev`. `stale` says it
 * is ours and behind; `wrong_size` says it is this pool's member and the
 * wrong disk, which is a different thing from the member being absent
 * and is worth a different answer. */
static bool copy_here(struct cfs *fs, struct blkdev *bd, unsigned vdev, const struct cfs_member *want, bool *stale,
                      bool *wrong_size)
{
    struct cfs_label l;
    *stale = false;
    *wrong_size = false;
    if (label_read(&fs->mac, bd, &l) != 0 || l.index != vdev || memcmp(l.uuid, fs->sb.uuid, 16) != 0)
        return false;
    /* A resilver's target or a spare is labelled for this pool and is no
     * copy of anything yet (design.md, "Resilvering"). */
    /* At any version: the words were reserved zero before 13, and a
     * resilver may have labelled a device of an older pool (Codex on #440). */
    if (l.flags & (CFS_LABEL_RESILVER | CFS_LABEL_SPARE))
        return false;
    if (l.nblocks != want->nblocks) {
        kerror("cosmofs: %s carries member %u but %llu blocks, not %llu", bd->name, vdev,
               (unsigned long long)l.nblocks, (unsigned long long)want->nblocks);
        *wrong_size = true;
        return false;
    }
    if (!label_current(fs, &l)) {
        kwarn("cosmofs: %s last took part in generation %llu, not %llu; not mirroring it", bd->name,
              (unsigned long long)l.generation, (unsigned long long)fs->sb.generation);
        *stale = true;
        return false;
    }
    return true;
}

/*
 * Assemble member `vdev`: the first *current* device found becomes the
 * pool's next member (members are added in order, so the pool's
 * numbering is the table's). Which copy it was labelled is not the
 * question -- copies of a mirror are interchangeable, and preferring
 * the one labelled 0 would serve a stale disk in preference to a good
 * one, which is precisely what the generation is recorded to prevent.
 */
static int assemble_member(struct cfs *fs, unsigned vdev, const struct cfs_member *want)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        bool stale, wrong_size;
        if (!copy_here(fs, bd, vdev, want, &stale, &wrong_size)) {
            blkdev_put(bd);
            if (wrong_size)
                return -EIO;   /* this pool's member, and the wrong disk */
            (void)stale;       /* counted once at the end, from what the pool has */
            continue;
        }
        unsigned got;
        int rc = pool_add_member(fs->pool, bd, &got);
        blkdev_put(bd);
        if (rc)
            return rc;
        if (got != vdev)
            return -EIO;   /* the caller assembles in order; this cannot happen */
        return 0;
    }
    kerror("cosmofs: member %u of the pool has no current device", vdev);
    return -ENODEV;
}

/*
 * The generation `bd` last took part in, if it carries a copy of member
 * `vdev` of this pool: its label's, or -- member 0's devices carry the
 * superblock rather than a label, block 0 being the superblock -- the
 * newer of its two superblocks'. Only a superblock that verifies counts
 * (cfs_super_slots: magic, version, CRC), so a torn slot cannot pass
 * for a newer root.
 */
static bool device_gen(struct cfs *fs, struct blkdev *bd, unsigned vdev, const struct cfs_member *want, uint64_t *gen)
{
    if (vdev != 0) {
        struct cfs_label l;
        if (label_read(&fs->mac, bd, &l) != 0 || l.index != vdev || memcmp(l.uuid, fs->sb.uuid, 16) != 0 ||
            l.nblocks != want->nblocks || (l.flags & (CFS_LABEL_RESILVER | CFS_LABEL_SPARE)))
            return false;
        *gen = l.generation;
        return true;
    }
    struct cfs_super a, b;
    int ra = -EIO, rb = -EIO;
    if (cfs_super_slots(bd, &a, &ra, &b, &rb) != 0)
        return false;
    uint64_t newest = 0;
    bool ours = false;
    if (ra == 0 && memcmp(a.uuid, fs->sb.uuid, 16) == 0) {
        ours = true;
        newest = a.generation;
    }
    if (rb == 0 && memcmp(b.uuid, fs->sb.uuid, 16) == 0) {
        ours = true;
        if (b.generation > newest)
            newest = b.generation;
    }
    *gen = newest;
    return ours;
}

/*
 * A mount stands on the newest root of the pool, whichever of member 0's
 * devices it was handed (CF27): the device it was given may be the copy
 * that was away. Only a superblock that verifies counts.
 */
struct blkdev *cfs_newer_member0(struct blkdev *bd, const uint8_t uuid[16], uint64_t gen, uint64_t *newer)
{
    static const uint8_t zero[16] = { 0 };
    if (memcmp(uuid, zero, 16) == 0)
        return NULL;   /* before version 4 a pool has no uuid, and no copies */
    struct blkdev *best = NULL;
    uint64_t best_gen = gen;
    for (unsigned i = 0;; i++) {
        struct blkdev *cand = blk_nth(i);
        if (cand == NULL)
            break;
        struct cfs_super a, b;
        int ra = -EIO, rb = -EIO;
        uint64_t g = 0;
        if (cand != bd && cfs_super_slots(cand, &a, &ra, &b, &rb) == 0) {
            if (ra == 0 && memcmp(a.uuid, uuid, 16) == 0 && a.generation > g)
                g = a.generation;
            if (rb == 0 && memcmp(b.uuid, uuid, 16) == 0 && b.generation > g)
                g = b.generation;
        }
        if (g > best_gen) {
            if (best)
                blkdev_put(best);
            best = cand;
            best_gen = g;
        } else {
            blkdev_put(cand);
        }
    }
    *newer = best_gen;
    return best;
}

static bool in_pool(struct cfs *fs, unsigned vdev, const struct blkdev *bd)
{
    const struct spool_member *m = &fs->pool->m[vdev];
    for (unsigned k = 0; k < m->ncopies; k++)
        if (m->copy[k].dev == bd)
            return true;
    return false;
}

/*
 * A device carrying member `vdev` that the pool does not hold yet, and
 * whose generation is `current` (not older than the root being mounted)
 * or, when `current` is false, exactly `gen`. Referenced, or NULL. The
 * label's copy number is how devices of a member are told apart, not an
 * order to be honoured: copies are interchangeable.
 */
static struct blkdev *find_device(struct cfs *fs, unsigned vdev, const struct cfs_member *want, bool current,
                                  uint64_t gen)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *cand = blk_nth(i);
        if (cand == NULL)
            return NULL;
        uint64_t g;
        if (!in_pool(fs, vdev, cand) && device_gen(fs, cand, vdev, want, &g) &&
            (current ? g >= fs->sb.generation : g == gen))
            return cand;   /* keep the reference */
        blkdev_put(cand);
    }
}

/* A device labelled `flags` (CFS_LABEL_RESILVER or CFS_LABEL_SPARE) for
 * this pool with `tag`, the pool not holding it: referenced, or NULL. */
struct blkdev *cfs_find_tagged(struct cfs *fs, uint64_t flags, uint64_t tag)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *cand = blk_nth(i);
        if (cand == NULL)
            return NULL;
        struct cfs_label l;
        unsigned v, c;
        if (!pool_find_dev(fs->pool, cand, &v, &c) && label_read(&fs->mac, cand, &l) == 0 &&
            memcmp(l.uuid, fs->sb.uuid, 16) == 0 && (l.flags & flags) && (tag == 0 || l.tag == tag))
            return cand;
        blkdev_put(cand);
    }
}

/* Every spare of this pool among the registered devices, into the pool's
 * list: a spare is its label (design.md, "Spares"). */
void cfs_spares_find(struct cfs *fs)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *cand = blk_nth(i);
        if (cand == NULL)
            return;
        struct cfs_label l;
        unsigned v, c;
        if (!pool_find_dev(fs->pool, cand, &v, &c) && label_read(&fs->mac, cand, &l) == 0 &&
            memcmp(l.uuid, fs->sb.uuid, 16) == 0 && (l.flags & CFS_LABEL_SPARE) &&
            pool_add_spare(fs->pool, cand, l.tag) == 0)
            kinfo("cosmofs: %s is a spare of this pool", cand->name);
        blkdev_put(cand);
    }
}

/*
 * A device of member `vdev` this pool does not hold whose generation is
 * behind the root: the copy that was away. `since` it is the copy a
 * record says missed what its bitmap says (*exact); any other is a whole
 * rebuild. Referenced, or NULL.
 */
struct blkdev *cfs_find_lagging(struct cfs *fs, unsigned vdev, uint64_t since, bool *exact)
{
    struct cfs_member want = { .nblocks = fs->mem[vdev].nblocks };
    struct blkdev *any = NULL;
    for (unsigned i = 0;; i++) {
        struct blkdev *cand = blk_nth(i);
        if (cand == NULL)
            break;
        uint64_t g;
        if (in_pool(fs, vdev, cand) || !device_gen(fs, cand, vdev, &want, &g) || g >= fs->sb.generation) {
            blkdev_put(cand);
            continue;
        }
        if (since != 0 && g == since) {
            if (any)
                blkdev_put(any);
            *exact = true;
            return cand;
        }
        if (any == NULL)
            any = cand;
        else
            blkdev_put(cand);
    }
    *exact = false;
    return any;
}

/* --- copy state (version 13) ---------------------------------------------
 *
 * design.md, "Format version 13". The pool keeps each copy's state and
 * what it missed; this is where that is read from and written into the
 * member's CFS_KIND_COPYSTATE block.
 */

_Static_assert(CFS_COPY_DIRTY_BYTES == POOL_DIRTY_BYTES, "the record holds the pool's bitmap");
_Static_assert(sizeof(struct cfs_copy_rec) == 1000, "struct cfs_copy_rec is 1000 bytes");
_Static_assert(sizeof(struct cfs_copystate) <= CFS_BLOCK - CFS_MHDR_SIZE, "a copy state fits one block");
_Static_assert(CFS_MAX_COPIES == 4 && POOL_MAX_COPIES == 4, "struct cfs_copystate holds four copies");
_Static_assert(CFS_COPY_ONLINE == POOL_COPY_ONLINE && CFS_COPY_MISSING == POOL_COPY_MISSING &&
                   CFS_COPY_OFFLINE == POOL_COPY_OFFLINE && CFS_COPY_FAULTED == POOL_COPY_FAULTED &&
                   CFS_COPY_RESILVERING == POOL_COPY_RESILVERING,
               "the record's states are the pool's");

static void rec_from_copy(struct cfs_copy_rec *r, const struct pool_copy *cp)
{
    memset(r, 0, sizeof(*r));
    r->state = (uint32_t)cp->state;
    r->flags = cp->whole ? CFS_COPY_WHOLE : 0;
    r->since = cp->since;
    r->cursor = cp->cursor;
    r->read_errors = cp->read_errors;
    r->write_errors = cp->write_errors;
    r->csum_errors = cp->csum_errors;
    r->tag = cp->tag;
    r->peer = cp->peer;
    if (cp->peer)
        r->flags |= CFS_COPY_DETACH;
    if (cp->dirty)
        memcpy(r->dirty, cp->dirty, CFS_COPY_DIRTY_BYTES);
}

static void copy_from_rec(struct pool_copy *cp, const struct cfs_copy_rec *r)
{
    cp->state = (enum pool_copy_state)r->state;
    cp->whole = (r->flags & CFS_COPY_WHOLE) != 0;
    cp->since = r->since;
    cp->cursor = r->cursor;
    cp->read_errors = r->read_errors;
    cp->write_errors = r->write_errors;
    cp->csum_errors = r->csum_errors;
    cp->tag = r->tag;
    cp->peer = (r->flags & CFS_COPY_DETACH) ? r->peer : 0;
    if (cp->dirty)
        memcpy(cp->dirty, r->dirty, CFS_COPY_DIRTY_BYTES);
}

/* The member's records, ONLINE first: copies are interchangeable, and the
 * device the pool already holds in slot 0 is a current one. A record set
 * that is not this member's is the block being wrong, which the mount
 * refuses as it refuses a wrong member table. */
static int copystate_read(struct cfs *fs, unsigned v, struct cfs_copy_rec *recs, unsigned n)
{
    struct cfs_buf *b;
    int rc = cfs_buf_get(fs, fs->mem[v].state_root, CFS_KIND_COPYSTATE, &b);
    if (rc)
        return rc;
    const struct cfs_copystate *cs = (const struct cfs_copystate *)(b->data + CFS_MHDR_SIZE);
    if (cs->ncopies != n || cs->region_blocks != fs->pool->m[v].region_blocks) {
        kerror("cosmofs: member %u's copy state describes %u copies of %u blocks a region, not %u of %u", v,
               cs->ncopies, cs->region_blocks, n, fs->pool->m[v].region_blocks);
        cfs_buf_put(fs, b);
        return -EIO;
    }
    unsigned k = 0;
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned c = 0; c < n; c++)
            if ((cs->copy[c].state == CFS_COPY_ONLINE) == (pass == 0))
                recs[k++] = cs->copy[c];
    cfs_buf_put(fs, b);
    for (unsigned c = 0; c < n; c++)
        if (recs[c].state >= POOL_COPY_NSTATES)
            return -EIO;
    return 0;
}

/*
 * Give member `vdev` its copies: the slots the table promises, the
 * records of what each holds, and the devices that fill them. A current
 * device fills an ONLINE record; an ONLINE record left without one is
 * MISSING and, nobody knowing since when, a whole rebuild. A device
 * behind the root is never attached for reads (CF27): one whose
 * generation is a MISSING record's `since` is that copy, and the record
 * says what it lacks; the resilver brings it back.
 */
static int setup_copies(struct cfs *fs, unsigned vdev, const struct cfs_member *want)
{
    struct cfs_memstate *m = &fs->mem[vdev];
    unsigned n = m->copies;
    int rc = pool_set_copies(fs->pool, vdev, n);
    if (rc)
        return rc;
    struct cfs_copy_rec *recs = kzalloc((size_t)n * sizeof(*recs));
    if (recs == NULL)
        return -ENOMEM;
    if (m->state_root != 0) {
        rc = copystate_read(fs, vdev, recs, n);
        if (rc) {
            kfree(recs);
            return rc;
        }
    }
    struct spool_member *pm = &fs->pool->m[vdev];
    for (unsigned c = 0; c < n; c++) {
        struct pool_copy *cp = &pm->copy[c];
        if (c == 0 && recs[0].state != CFS_COPY_ONLINE) {
            /* The device the mount stands on is current, whatever the
             * record thought: it took this root. */
            kwarn("cosmofs: member %u: no copy is recorded ONLINE; %s holds the root", vdev, cp->dev->name);
            recs[0].state = CFS_COPY_ONLINE;
        }
        copy_from_rec(cp, &recs[c]);
        if (c == 0 || recs[c].state != CFS_COPY_ONLINE)
            continue;
        struct blkdev *bd = find_device(fs, vdev, want, true, 0);
        if (bd != NULL) {
            rc = pool_attach_copy(fs->pool, vdev, c, bd, POOL_COPY_ONLINE);
            blkdev_put(bd);
            if (rc == 0)
                continue;
            kwarn("cosmofs: member %u copy %u could not be attached (%d)", vdev, c, rc);
        }
        cp->state = POOL_COPY_MISSING;
        cp->whole = true;
        cp->since = 0;
        kwarn("cosmofs: member %u copy %u is missing; the pool is degraded", vdev, c);
    }
    /* A resilver that a reboot interrupted: its target carries the
     * record's tag, and goes on from the record's cursor. */
    for (unsigned c = 0; c < n; c++) {
        struct pool_copy *cp = &pm->copy[c];
        if (cp->dev != NULL || cp->state != POOL_COPY_RESILVERING)
            continue;
        struct blkdev *bd = cp->tag ? cfs_find_tagged(fs, CFS_LABEL_RESILVER, cp->tag) : NULL;
        struct blkdev *done = bd == NULL ? find_device(fs, vdev, want, true, 0) : NULL;
        if (bd != NULL && pool_attach_copy(fs->pool, vdev, c, bd, POOL_COPY_RESILVERING) == 0) {
            kinfo("cosmofs: %s: member %u copy %u resilvering, from block %llu", bd->name, vdev, c,
                  (unsigned long long)cp->cursor);
        } else if (done != NULL && pool_attach_copy(fs->pool, vdev, c, done, POOL_COPY_ONLINE) == 0) {
            /* The resilver finished -- the target carries the pool's own
             * superblocks or label, as current as the root -- and the
             * machine stopped before the commit that records it ONLINE
             * (Codex on #440). Only a complete copy is current: a target
             * gets them last, and nothing else of the pool's is written
             * the root. */
            kinfo("cosmofs: %s: member %u copy %u was resilvered just before the last stop; ONLINE", done->name,
                  vdev, c);
            cp->whole = false;
            cp->cursor = 0;
            cp->tag = 0;
            if (cp->dirty)
                memset(cp->dirty, 0, CFS_COPY_DIRTY_BYTES);
        } else {
            kwarn("cosmofs: member %u copy %u was resilvering and its device is not here", vdev, c);
            cp->state = POOL_COPY_MISSING;
            cp->whole = true;
            cp->since = 0;
        }
        if (bd)
            blkdev_put(bd);
        if (done)
            blkdev_put(done);
    }
    /* What a lagging device is, said once: it stays out until resilvered. */
    for (unsigned c = 0; c < n; c++) {
        struct pool_copy *cp = &pm->copy[c];
        if (cp->dev != NULL || cp->state == POOL_COPY_ONLINE)
            continue;
        struct blkdev *bd = cp->whole ? NULL : find_device(fs, vdev, want, false, cp->since);
        if (bd != NULL) {
            kwarn("cosmofs: %s is member %u copy %u, %s since generation %llu; not read until resilvered",
                  bd->name, vdev, c, pool_copy_state_name(cp->state), (unsigned long long)cp->since);
            blkdev_put(bd);
        }
    }
    kfree(recs);
    return 0;
}

/*
 * Before a commit allocates anything: the state block each mirrored
 * member needs -- copied on write, made, or given back when every copy
 * is ONLINE again (state_root 0 is that record). The first record of a
 * degraded member raises the pool to version 13, through 12 at 11; a
 * pool below 11 is not raised and keeps its copy state in memory only
 * (design.md, "The record, written by the commit").
 */
int cfs_copystate_prepare(struct cfs *fs)
{
    if (fs->rdonly)
        return 0;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        bool need = m->copies > 1 && pool_member_degraded(fs->pool, v);
        if (!need) {
            if (m->state_root != 0) {
                cfs_free_block_deferred(fs, m->state_root);
                m->state_root = 0;
            }
            continue;
        }
        if (fs->sb.version < 13) {
            if (fs->sb.version < 11)
                continue;
            unsigned was = fs->sb.version;
            int rc = cfs_snap_raise(fs);
            if (rc)
                return rc;
            fs->sb.version = 13;
            kinfo("cosmofs: format version %u -> 13 (copy state): member %u is degraded; a kernel that reads "
                  "format 12 at most (CosmoOS 1.0.10 and older) cannot mount this pool",
                  was, v);
        }
        struct cfs_buf *b;
        int rc;
        if (m->state_root == 0) {
            /* On the member it describes, when it has room (as a member's
             * allocation index is); the mount reads records only once
             * every member is assembled, so elsewhere is not fatal. */
            uint64_t blk, got;
            rc = cfs_alloc_run(fs, CFS_ALLOC_META, CFS_DVA(v, m->first_usable), 1, &blk, &got);
            if (rc)
                return rc;
            rc = cfs_buf_new_at(fs, CFS_KIND_COPYSTATE, blk, &b);
            if (rc) {
                cfs_free_block_deferred(fs, blk);
                return rc;
            }
            m->state_root = b->blkno;
        } else {
            rc = cfs_buf_get(fs, m->state_root, CFS_KIND_COPYSTATE, &b);
            if (rc)
                return rc;
            rc = cfs_buf_cow(fs, &b, &m->state_root);
            if (rc) {
                cfs_buf_put(fs, b);
                return rc;
            }
        }
        cfs_buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
    }
    return 0;
}

/* True when every degraded mirrored member that can have a record has
 * one. A copy lost after cfs_copystate_prepare, on a member that had no
 * record, is recorded by the next commit, which this keeps due. */
bool cfs_copystate_recorded(struct cfs *fs)
{
    if (fs->sb.version < 11)
        return true;   /* not recordable here: kept in memory (design.md) */
    for (unsigned v = 0; v < fs->nmembers; v++)
        if (fs->mem[v].copies > 1 && fs->mem[v].state_root == 0 && pool_member_degraded(fs->pool, v))
            return false;
    return true;
}

/*
 * Just before the dirty metadata is written: every dirty block's region
 * and region 0 (the superblocks, or the label) are set for the copies
 * that will miss them -- this commit's own writes -- and then the pool's
 * state is copied into each state block. Without the first step the
 * record would describe everything but the commit that wrote it (CF25).
 * Allocates nothing; the blocks are this generation's.
 */
int cfs_copystate_fill(struct cfs *fs)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        if (m->state_root == 0)
            continue;
        struct cfs_buf *it;
        list_for_each_entry(it, &fs->bufs, link)
            if (it->dirty && CFS_DVA_VDEV(it->blkno) == v)
                pool_mark_missed(fs->pool, it->blkno);
        for (uint64_t blk = 0; blk < m->first_usable; blk++)
            pool_mark_missed(fs->pool, CFS_DVA(v, blk));
    }
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        if (m->state_root == 0)
            continue;
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, m->state_root, CFS_KIND_COPYSTATE, &b);
        if (rc)
            return rc;
        const struct spool_member *pm = &fs->pool->m[v];
        struct cfs_copystate *cs = (struct cfs_copystate *)(b->data + CFS_MHDR_SIZE);
        memset(cs, 0, sizeof(*cs));
        cs->ncopies = pm->ncopies;
        cs->region_blocks = pm->region_blocks;
        for (unsigned c = 0; c < pm->ncopies; c++)
            rec_from_copy(&cs->copy[c], &pm->copy[c]);
        cfs_buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
    }
    return 0;
}

/* --- the member table ---------------------------------------------------- */

/*
 * A member table is disk data. Its geometry decides how many bitmap
 * chunks are read and how far the allocator may reach, so every field
 * is checked against what the format can express before any of it is
 * believed -- the formatter's limits are not evidence about a disk this
 * kernel did not write.
 */
static bool member_sane(const struct cfs_member *m, unsigned v, unsigned nmembers)
{
    if (m->nblocks < CFS_MIN_BLOCKS || m->nblocks > CFS_MAX_BLOCKS)
        return false;
    /* One allocation index holds CFS_PTRS_PER_BLOCK chunk pointers, so a
     * member that claims more chunks than that would be read past the
     * end of its index block. */
    uint64_t chunks = (m->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP;
    if (chunks > CFS_PTRS_PER_BLOCK)
        return false;
    uint64_t first = v == 0 ? 2 : 1;   /* the superblocks, or the label */
    if (m->first_usable < first || m->first_usable >= m->nblocks)
        return false;
    /* A member's allocation index is normally on that member (that is
     * what the copy-on-write hint arranges), but a member with no free
     * block at all leaves its replacement elsewhere rather than wedging
     * the filesystem, so what is checked here is only that the address
     * could name a block of this pool. */
    if (CFS_DVA_VDEV(m->alloc_root) >= nmembers)
        return false;
    if (m->copies > CFS_MAX_COPIES)
        return false;
    return m->free_blocks <= m->nblocks;
}

/* Lay the members out in the linear space the bitmap is indexed by:
 * each member starts at a whole chunk, so no chunk straddles two. */
static void layout(struct cfs *fs)
{
    uint64_t base = 0;
    unsigned chunk = 0;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        m->base = base;
        m->chunk0 = chunk;
        m->nchunks = (unsigned)((m->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        base += (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP;
        chunk += m->nchunks;
    }
    fs->nblocks = base;
    fs->nr_chunks = chunk;
}

int cfs_members_load(struct cfs *fs)
{
    /* Member 0's first device is the one the mount was given. */
    if (fs->sb.version < 4) {
        /* One member, and its allocation root is where it always was. */
        fs->mem = kzalloc(sizeof(*fs->mem));
        if (fs->mem == NULL)
            return -ENOMEM;
        fs->nmembers = 1;
        fs->mem[0].nblocks = fs->sb.total_blocks;
        fs->mem[0].first_usable = 2;
        fs->mem[0].alloc_root = fs->sb.alloc_root;
        layout(fs);
        return 0;
    }

    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, fs->sb.members, block);
    if (rc == 0) {
        const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
        if (h->magic != CFS_MHDR_MAGIC || h->kind != CFS_KIND_MEMBERS || h->blkno != fs->sb.members)
            rc = -EIO;
    }
    if (rc) {
        kfree(block);
        kerror("cosmofs: the member table is unreadable");
        return rc;
    }
    const struct cfs_member_block *mb = (const struct cfs_member_block *)(block + CFS_MHDR_SIZE);
    uint64_t count = mb->count;
    if (count == 0 || count > CFS_MEMBERS_PER_BLOCK || count > CFS_MAX_MEMBERS) {
        kfree(block);
        return -EIO;
    }
    fs->mem = kzalloc((size_t)count * sizeof(*fs->mem));
    if (fs->mem == NULL) {
        kfree(block);
        return -ENOMEM;
    }
    fs->nmembers = (unsigned)count;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        const struct cfs_member *src = &mb->m[v];
        struct cfs_memstate *m = &fs->mem[v];
        if (!member_sane(src, v, fs->nmembers)) {
            kerror("cosmofs: member %u of the table is not a member this format can describe", v);
            rc = -EIO;
            break;
        }
        m->nblocks = src->nblocks;
        m->first_usable = src->first_usable;
        m->alloc_root = src->alloc_root;
        m->free_blocks = src->free_blocks;
        m->copies = src->copies ? src->copies : 1;   /* version 4 wrote 0 and meant one */
        m->state_root = fs->sb.version >= 13 ? src->state_root : 0;
        memcpy(m->uuid, src->uuid, 16);
        /* A second load of the same pool (the older root, after the newer
         * one did not load) finds the member already assembled. */
        if (v > 0 && v >= fs->pool->nmembers) {
            rc = assemble_member(fs, v, src);
            if (rc)
                break;
        }
        /* The table says how big the member is; the device says how big
         * it really is, and the smaller of those is not negotiable. */
        if (rc == 0 && m->nblocks > fs->pool->m[v].nblocks) {
            kerror("cosmofs: member %u claims %llu blocks, the device has %llu", v,
                   (unsigned long long)m->nblocks, (unsigned long long)fs->pool->m[v].nblocks);
            rc = -EIO;
        }
        if (rc)
            break;
        /* And the member is the table's size, however big its device:
         * the copy record's regions are sized from it, and a larger copy
         * mounting the pool must size them the same. */
        pool_set_member_blocks(fs->pool, v, m->nblocks);
    }
    /* The copies, once every member is assembled: a member's record may
     * lie on another member (the allocator put it where there was room),
     * and a record is read like any metadata block -- only once the
     * member it lies on is there. */
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++)
        if (fs->mem[v].copies > 1 && fs->pool->m[v].ncopies == 1)
            rc = setup_copies(fs, v, &mb->m[v]);
    kfree(block);
    if (rc) {
        kfree(fs->mem);
        fs->mem = NULL;
        fs->nmembers = 0;
        return rc;
    }
    layout(fs);
    /* What the table promised, less what the pool actually has: counting
     * as devices are passed over instead would count the same missing
     * copy twice, once where it was rejected and once where its slot
     * went unfilled. */
    fs->degraded = cfs_copies_out(fs);
    if (fs->nmembers > 1 || fs->mem[0].copies > 1) {
        unsigned mirrored = 0;
        for (unsigned v = 0; v < fs->nmembers; v++)
            for (unsigned c = 0; c < fs->pool->m[v].ncopies; c++)
                mirrored += pool_copy_online(fs->pool, v, c) ? 1u : 0u;
        kinfo("cosmofs: pool of %u members over %u device(s), %llu blocks%s", fs->nmembers, mirrored,
              (unsigned long long)fs->sb.total_blocks, fs->degraded ? " (degraded)" : "");
    }
    return 0;
}

int cfs_members_store(struct cfs *fs)
{
    if (fs->sb.version < 4) {
        /* Nothing to store: the one member's root is the superblock's,
         * which the commit writes anyway. */
        fs->sb.alloc_root = fs->mem[0].alloc_root;
        return 0;
    }
    struct cfs_buf *b;
    int rc = cfs_buf_get(fs, fs->sb.members, CFS_KIND_MEMBERS, &b);
    if (rc)
        return rc;
    rc = cfs_buf_cow(fs, &b, &fs->sb.members);
    if (rc) {
        cfs_buf_put(fs, b);
        return rc;
    }
    struct cfs_member_block *mb = (struct cfs_member_block *)(b->data + CFS_MHDR_SIZE);
    mb->count = fs->nmembers;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_member *dst = &mb->m[v];
        const struct cfs_memstate *m = &fs->mem[v];
        memcpy(dst->uuid, m->uuid, 16);
        dst->nblocks = m->nblocks;
        dst->first_usable = m->first_usable;
        dst->alloc_root = m->alloc_root;
        dst->free_blocks = m->free_blocks;
        dst->copies = m->copies;
        dst->state_root = fs->sb.version >= 13 ? m->state_root : 0;
    }
    cfs_buf_mark_dirty(fs, b);
    cfs_buf_put(fs, b);
    return 0;
}

/*
 * Stamp every attached copy of every member past the first with this
 * generation. Called from the commit after every block of the
 * transaction is stable and before the root is written, so a label is
 * never newer than the root it belongs to: a device that misses a
 * commit keeps the older number and is refused at the next mount.
 *
 * Member 0 needs none of this -- its devices carry the superblock
 * itself, whose generation is the same evidence.
 *
 * Only the ONLINE copies: one that is not keeps the generation it last
 * held whole, which is how a mount knows it for what it is. A label
 * that fails to write is the pool's to count, like any write; the
 * commit fails only when a member has no ONLINE copy left, and the
 * flush after this says so.
 */
int cfs_labels_update(struct cfs *fs)
{
    if (fs->sb.version < 5)
        return 0;
    for (unsigned v = 1; v < fs->nmembers; v++) {
        unsigned n = pool_copies(fs->pool, CFS_DVA(v, 0));
        for (unsigned c = 0; c < n; c++) {
            if (!pool_copy_online(fs->pool, v, c))
                continue;
            int rc = cfs_label_write(fs->pool, &fs->mac, v, c, fs->gen, fs->sb.uuid, fs->mem[v].nblocks);
            if (rc == -ENOMEM)
                return rc;
        }
    }
    return 0;
}

/*
 * The devices the pool uses besides the one the mount was given -- its
 * other copies and resilver targets, its spares, and after a mount that
 * moved to a newer member-0 device that device too -- held by the mount, so /dev/blkctl and a second
 * mount see them as in use (CFS-5; Codex on #439: a redirected mount
 * left the device it actually used unclaimed).
 */
void cfs_holds_sync(struct cfs *fs)
{
    (void)cfs_holds_sync_with(fs, NULL);
}

/* The same with `extra` held too: a device about to join the pool is
 * claimed before anything is written to it, and -EBUSY when another mount
 * has it (Codex on #440: checked first and claimed after, a mount could
 * take it in between). */
int cfs_holds_sync_with(struct cfs *fs, struct blkdev *extra)
{
    if (fs->mnt == NULL)
        return 0;
    struct blkdev *held[MOUNT_HELD_MAX];
    unsigned n = 0;
    const struct spool *p = fs->pool;
    for (unsigned v = 0; v < p->nmembers; v++)
        for (unsigned c = 0; c < p->m[v].ncopies; c++) {
            struct blkdev *bd = p->m[v].copy[c].dev;
            if (bd != NULL && bd != fs->mnt->bdev && n < MOUNT_HELD_MAX)
                held[n++] = bd;
        }
    for (unsigned i = 0; i < p->nspares && n < MOUNT_HELD_MAX; i++)
        held[n++] = p->spare[i];   /* and its spares (roadmap 1.2 B3) */
    if (extra != NULL) {
        if (n == MOUNT_HELD_MAX)
            return -ENOSPC;
        held[n++] = extra;
    }
    int rc = vfs_mount_set_held(fs->mnt, held, n);
    if (rc && extra == NULL)
        kwarn("cosmofs: %s: the pool's devices could not all be held (%d)", fs->pool_name, rc);
    return rc;
}

/* Copies the table promises that are not ONLINE with a device. */
unsigned cfs_copies_out(struct cfs *fs)
{
    unsigned out = 0;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        unsigned n = fs->mem[v].copies;
        for (unsigned c = 0; c < n; c++)
            out += pool_copy_online(fs->pool, v, c) ? 0u : 1u;
    }
    return out;
}

/*
 * One block, from whichever copy of its member can satisfy `verify`.
 * Copy 0 first, because that is where a healthy pool answers from; then
 * the rest, and the first that verifies is written back over the ones
 * that did not. A mirror that is read without checking only doubles the
 * chance of returning something wrong.
 */
int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired, bool *read_ok)
{
    if (repaired)
        *repaired = false;
    if (read_ok)
        *read_ok = false;
    unsigned copies = pool_copies(fs->pool, dva);
    if (copies == 0)
        return -EINVAL;
    bool bad[POOL_MAX_COPIES] = { false };
    int first_err = 0;
    bool any_bad = false;
    for (unsigned c = 0; c < copies; c++) {
        /* A copy that is not ONLINE is not read, and is not a bad copy
         * either: it is known not to hold the block (CF26). */
        if (!pool_copy_readable(fs->pool, dva, c))
            continue;
        int rc = pool_read_copy(fs->pool, dva, c, buf);
        /* Read and rejected is not read and failed: the failure below is
         * -EIO either way, and a caller that reports on the contents of
         * `buf` must know which it has. This tracks the copy just
         * attempted rather than whether any copy succeeded, because
         * every copy reads into the same buffer: when the loop ends it
         * is the last attempt's bytes that are in there, and a copy that
         * failed mid-transfer leaves a partial block behind that no
         * verifier ever looked at. */
        if (read_ok)
            *read_ok = (rc == 0);
        if (rc == 0 && verify(buf, arg)) {
            if (!any_bad)
                return 0;   /* the common case: nothing to repair */
            if (fs->rdonly) {
                /* A read-only mount reads from the good copy and writes
                 * nothing back (CFS-1): the bad one is said, not fixed. */
                kwarn("cosmofs: block %llu:%llu read from copy %u; %u bad copy(s) not repaired (read-only)",
                      (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), c, c);
                return 0;
            }
            /* Write this copy back over every copy that failed. */
            unsigned fixed = 0;
            for (unsigned b = 0; b < c; b++) {
                if (!bad[b])
                    continue;
                if (pool_write_copy(fs->pool, dva, b, buf) == 0)
                    fixed++;
            }
            kwarn("cosmofs: block %llu:%llu recovered from copy %u (%u repaired)",
                  (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), c, fixed);
            fs->repairs += fixed;
            if (repaired)
                *repaired = fixed > 0;
            return 0;
        }
        if (rc == 0)
            pool_copy_csum_error(fs->pool, dva, c);   /* read, and not what it should be */
        bad[c] = true;
        any_bad = true;
        if (first_err == 0)
            first_err = rc ? rc : -EIO;
    }
    return first_err ? first_err : -EIO;
}

/*
 * Every copy, checked. A read stops at the first copy that verifies,
 * because that is the answer; a scrub cannot, because the copy that
 * verifies may be hiding a rotted one behind it that nobody will notice
 * until it is the one answering.
 */
int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   bool repair, unsigned *bad_out, unsigned *repaired)
{
    if (repaired)
        *repaired = 0;
    if (bad_out)
        *bad_out = 0;
    unsigned copies = pool_copies(fs->pool, dva);
    if (copies == 0)
        return -EINVAL;
    bool bad[POOL_MAX_COPIES] = { false };
    unsigned nbad = 0;
    int good = -1;
    uint8_t *scratch = kmalloc(CFS_BLOCK, 0);
    if (scratch == NULL)
        return -ENOMEM;
    unsigned present = 0;
    for (unsigned c = 0; c < copies; c++) {
        if (!pool_copy_readable(fs->pool, dva, c))
            continue;   /* not ONLINE: neither good nor bad, it holds nothing yet (CF26) */
        present++;
        int rc = pool_read_copy(fs->pool, dva, c, scratch);
        if (rc == 0 && verify(scratch, arg)) {
            if (good < 0) {
                good = (int)c;
                memcpy(buf, scratch, CFS_BLOCK);
            }
            continue;
        }
        if (rc == 0)
            pool_copy_csum_error(fs->pool, dva, c);
        bad[c] = true;
        nbad++;
    }
    int rc = 0;
    if (good < 0) {
        rc = -EIO;
    } else if (nbad && (!repair || fs->rdonly)) {
        /* Found and left: a scrub that was not asked to repair writes
         * nothing, and a read-only mount writes nothing (CFS-1). */
        kwarn("cosmofs: block %llu:%llu: %u of %u copies rotted; not repaired%s",
              (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), nbad, present,
              fs->rdonly ? " (read-only)" : "");
        if (bad_out)
            *bad_out = nbad;
    } else if (nbad) {
        unsigned fixed = 0;
        for (unsigned c = 0; c < copies; c++)
            if (bad[c] && pool_write_copy(fs->pool, dva, c, buf) == 0)
                fixed++;
        kwarn("cosmofs: block %llu:%llu: %u of %u copies rotted, %u repaired from copy %d",
              (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), nbad, present, fixed, good);
        fs->repairs += fixed;
        if (repaired)
            *repaired = fixed;
        if (bad_out)
            *bad_out = nbad;
    }
    kfree(scratch);
    return rc;
}

void cfs_members_free(struct cfs *fs)
{
    kfree(fs->mem);
    fs->mem = NULL;
    fs->nmembers = 0;
}
