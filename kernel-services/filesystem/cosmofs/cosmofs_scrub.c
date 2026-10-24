/*
 * cosmofs_scrub.c - Read everything, before somebody needs it
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Scrub").
 *
 * Repair on read only finds what somebody reads. A scrub reads the whole
 * filesystem through exactly the same path -- metadata by its own
 * header, data by the checksum its inode records -- so a copy that has
 * rotted is found and put right while there is still a good one to put
 * it right from. Nothing here has its own idea of what a correct block
 * is; that would be a second implementation to keep in step.
 *
 * Roadmap 1.2 B2 made three changes. Every tree is read -- the live one,
 * each snapshot's and each clone's -- and each block once, however many
 * trees share it (`done`). A bad copy is rewritten only when the caller
 * asked for repair (COSMOFS_SCRUB_REPAIR); otherwise the scrub writes
 * nothing and says what a repair would do. And it is paced: at most
 * `g_cosmofs_scrub_rate` blocks a second, with the mount's lock let go
 * between windows, so a scrub of a large pool leaves the writers room.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "cosmofs_internal.h"

unsigned g_cosmofs_scrub_rate = COSMOFS_SCRUB_RATE_DEFAULT;

/* The pacing window: the budget is a rate, spent a tenth of a second at
 * a time, so a pause is never long enough to be a stall of its own --
 * and, below ten blocks a second, one block a window of 1/rate seconds
 * (Codex on #434: a 100 ms window's minimum of one block ran a rate of 1
 * at 10). The resilver paces itself the same way (roadmap 1.2 B3). */
#define SCRUB_WINDOW_NS 100000000ull
#define SCRUB_WINDOWS_PER_S 10u

void cfs_pace_init(struct cfs_pace *p, unsigned rate)
{
    p->quota = rate == 0 ? 0 : (rate >= SCRUB_WINDOWS_PER_S ? rate / SCRUB_WINDOWS_PER_S : 1);
    p->window_ns = rate == 0 || rate >= SCRUB_WINDOWS_PER_S ? SCRUB_WINDOW_NS : 1000000000ull / rate;
    p->used = 0;
    p->window = clock_now_ns();
}

/* Called with fs->lock held, and returns with it held: when this window's
 * share is spent it lets the caller's lock go for the rest of the window
 * and takes it again. True if it let go. */
bool cfs_pace(struct cfs *fs, struct cfs_pace *p)
{
    if (p->quota == 0 || p->used < p->quota)
        return false;
    uint64_t spent = clock_since_ns(p->window);
    mutex_unlock(&fs->lock);
    if (spent < p->window_ns)
        thread_sleep_ms((unsigned)((p->window_ns - spent + 999999ull) / 1000000ull));
    else
        sched_yield();
    mutex_lock(&fs->lock);
    p->used = 0;
    p->window = clock_now_ns();
    return true;
}

struct scrub {
    struct cfs *fs;
    uint8_t *page;
    uint8_t *inodes;          /* one inode block's slots, copied out of the cache */
    struct cosmofs_scrub_stats st;
    bool repair;
    struct cfs_pace pace;     /* fs.scrub_rate */
    uint64_t *slots;          /* an allocation index's chunk pointers, copied out */
    uint64_t commits;         /* fs->commits when the walk last looked */
    struct cfs_bits done;     /* blocks verified (linear numbers) */
    bool stop;                /* stop walking: `err` says why */
    int err;                  /* -EIO: the transaction was abandoned during a pause; -ENOMEM */
};

/*
 * The one place the lock is let go. Called only where the walk holds no
 * buffer, so nothing it holds can go stale; what it has *copied* -- an
 * inode, a snapshot's roots -- can, if a commit ran meanwhile, and the
 * answer says so: the caller reads it again. A block freed by that
 * commit is not handed out again before the commit after it (design.md,
 * "Commit"), so a copy taken in this generation never leads to a block
 * that has become something else.
 */
static bool pace(struct scrub *s)
{
    struct cfs *fs = s->fs;
    if (!cfs_pace(fs, &s->pace))
        return false;
    if (fs->failed) {
        s->stop = true;
        s->err = -EIO;
    }
    bool moved = fs->commits != s->commits;
    s->commits = fs->commits;
    return moved;
}

/* True the first time this scrub meets `dva`. */
static bool first_visit(struct scrub *s, uint64_t dva)
{
    uint64_t lin = cfs_dva_lin(s->fs, dva);
    if (lin == CFS_DVA_NONE)
        return true;   /* not a block of the pool: the verifier says so */
    return !cfs_bits_set(&s->done, lin);
}

static void tally(struct scrub *s, int rc, unsigned bad, unsigned fixed)
{
    if (rc == -ENOMEM) {
        /* Not a finding about the disk: the scrub could not look. It
         * stops and says so rather than call the block unrecoverable
         * (Codex on #433). */
        s->err = -ENOMEM;
        s->stop = true;
        return;
    }
    s->st.blocks_read++;
    s->pace.used++;
    if (rc)
        s->st.unrecoverable++;
    s->st.bad_copies += bad;
    s->st.repaired += fixed;
}

struct meta_want {
    const struct cfs_mac *mac;
    uint64_t dva;
    uint32_t kind;
    uint32_t alt;   /* a second kind the block may be, or 0 */
};

static bool meta_ok(const void *block, void *arg)
{
    const struct meta_want *w = arg;
    return cfs_mhdr_ok(w->mac, block, w->dva, w->kind) ||
           (w->alt != 0 && cfs_mhdr_ok(w->mac, block, w->dva, w->alt));
}

/* One metadata block, every copy of it: of kind `kind` or `alt`. */
static void scrub_meta_either(struct scrub *s, uint64_t dva, uint32_t kind, uint32_t alt)
{
    struct cfs *fs = s->fs;
    if (dva == 0 || !cfs_dva_valid(fs, dva))
        return;
    /* A block the open transaction wrote is in memory only: its device
     * copies are whatever the block held before, and checking them would
     * call a sound block unrecoverable -- or pass it by the luck of a
     * stale block with the same address and kind. The commit writes it;
     * the next scrub reads it. */
    if (cfs_buf_is_dirty(fs, dva)) {
        s->st.skipped++;
        return;
    }
    if (!first_visit(s, dva))
        return;
    struct meta_want w = { .mac = &fs->mac, .dva = dva, .kind = kind, .alt = alt };
    unsigned bad = 0, fixed = 0;
    int rc = cfs_verify_all(fs, dva, s->page, meta_ok, &w, s->repair, &bad, &fixed);
    tally(s, rc, bad, fixed);
}

static void scrub_meta(struct scrub *s, uint64_t dva, uint32_t kind)
{
    scrub_meta_either(s, dva, kind, 0);
}

/*
 * A checksum tree, one level or (version 14) two: every block's copies.
 * Each block's copies are verified before the cached read that finds its
 * children: on a writable mirror that read repairs a bad first copy, and
 * the scrub would neither count it nor say it was repaired (Codex on
 * #447). The root may be either shape.
 */
static void scrub_csum(struct scrub *s, uint64_t blk, unsigned depth)
{
    struct cfs *fs = s->fs;
    if (depth)
        scrub_meta(s, blk, CFS_KIND_CSUMIDX);
    else
        scrub_meta_either(s, blk, CFS_KIND_CSUMIDX, fs->sb.version >= 14 ? CFS_KIND_CSUMIDX2 : 0);
    struct cfs_buf *idx;
    if (s->stop || cfs_buf_get(fs, blk, depth ? CFS_KIND_CSUMIDX : 0, &idx) != 0)
        return;
    uint32_t kind = cfs_buf_hdr(idx)->kind;
    bool two = depth == 0 && kind == CFS_KIND_CSUMIDX2 && fs->sb.version >= 14;
    if (!two && kind != CFS_KIND_CSUMIDX) {
        cfs_buf_put(fs, idx);
        return;
    }
    uint64_t *slots = kmalloc(CFS_PTRS_PER_BLOCK * sizeof(*slots), 0);
    if (slots != NULL)
        memcpy(slots, idx->data + CFS_MHDR_SIZE, CFS_PTRS_PER_BLOCK * sizeof(*slots));
    cfs_buf_put(fs, idx);
    if (slots == NULL) {
        tally(s, -ENOMEM, 0, 0);   /* the tree's blocks unread: the scrub stops and says so (Codex on #447) */
        return;
    }
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK && !s->stop; i++) {
        if (slots[i] == 0)
            continue;
        if (two)
            scrub_csum(s, slots[i], 1);
        else
            scrub_meta(s, slots[i], CFS_KIND_CSUM);
    }
    kfree(slots);
}

static void scrub_data(struct scrub *s, struct cfs_inode *in, uint64_t lblk, uint64_t dva)
{
    if (!first_visit(s, dva))
        return;
    unsigned bad = 0, fixed = 0;
    int rc = cfs_data_scrub_block(s->fs, in, lblk, dva, s->page, s->repair, &bad, &fixed);
    tally(s, rc, bad, fixed);
}

/* Both superblock slots on every device of member 0. The root is the one
 * block whose loss costs everything, and the one block no mount can have
 * checked across copies -- the copies are not known until it has been
 * read. A slot no copy holds a superblock in is the unused slot of a
 * freshly formatted filesystem, not damage. */
static void scrub_supers(struct scrub *s)
{
    struct cfs *fs = s->fs;
    unsigned copies = pool_copies(fs->pool, CFS_DVA(0, 0));
    uint8_t *good = kmalloc(CFS_BLOCK, 0);
    if (good == NULL) {
        s->err = -ENOMEM;
        s->stop = true;
        return;
    }
    for (unsigned slot = 0; slot < 2; slot++) {
        uint64_t dva = CFS_DVA(0, slot);
        bool bad[POOL_MAX_COPIES] = { false };
        unsigned nbad = 0;
        int best = -1;
        uint64_t newest = 0;
        for (unsigned c = 0; c < copies; c++) {
            if (!pool_copy_readable(fs->pool, dva, c))
                continue;   /* not ONLINE: holds nothing yet, good or bad (CF26) */
            if (pool_read_copy(fs->pool, dva, c, s->page) == 0 && cfs_super_ok(s->page)) {
                uint64_t g = ((const struct cfs_super *)s->page)->generation;
                if (best < 0 || g > newest) {
                    newest = g;
                    best = (int)c;
                    memcpy(good, s->page, CFS_BLOCK);
                }
                continue;
            }
            bad[c] = true;
            nbad++;
        }
        if (best < 0) {
            /* No copy holds one. A never-written slot of a young pool;
             * but the slot this mount's root came from was written, and
             * then it is lost, not unused (Codex on #434). */
            if (slot == fs->sb_slot) {
                kerror("cosmofs: superblock slot %u, the mounted root's, verifies on no copy", slot);
                tally(s, -EIO, 0, 0);
            }
            continue;
        }
        (void)first_visit(s, dva);
        unsigned fixed = 0;
        if (nbad != 0 && s->repair) {
            for (unsigned c = 0; c < copies; c++)
                if (bad[c] && pool_write_copy(fs->pool, dva, c, good) == 0)
                    fixed++;
            fs->repairs += fixed;
        }
        if (nbad != 0)
            kwarn("cosmofs: superblock slot %u: %u of %u copies bad, %u rewritten from copy %d", slot, nbad,
                  copies, fixed, best);
        tally(s, 0, nbad, fixed);
    }
    kfree(good);
}

/* Every allocation index and bitmap block: the map of what is in use is
 * the one thing whose loss costs the whole filesystem. */
static void scrub_alloc(struct scrub *s)
{
    struct cfs *fs = s->fs;
    for (unsigned v = 0; v < fs->nmembers && !s->stop; v++) {
        /* The chunk pointers copied out, so the lock can go between
         * bitmap blocks (Codex on #434: a large member's whole map was
         * read in one hold); copied again after a commit, which moves
         * the chunks it rewrote. */
        uint64_t copied = 0;
        for (unsigned c = 0; c < fs->mem[v].nchunks && !s->stop; c++) {
            if (c == 0 || fs->commits != copied) {
                uint64_t root = fs->mem[v].alloc_root;
                scrub_meta(s, root, CFS_KIND_ALLOCIDX);
                struct cfs_buf *idx;
                if (cfs_buf_get(fs, root, CFS_KIND_ALLOCIDX, &idx))
                    break;
                memcpy(s->slots, idx->data + CFS_MHDR_SIZE, (size_t)CFS_PTRS_PER_BLOCK * sizeof(uint64_t));
                cfs_buf_put(fs, idx);
                copied = fs->commits;
            }
            scrub_meta(s, s->slots[c], CFS_KIND_BITMAP);
            (void)pace(s);
        }
        /* And the record of the member's copies (version 13), which a
         * mount reads before anything else of the member's. */
        if (!s->stop && fs->mem[v].state_root != 0)
            scrub_meta(s, fs->mem[v].state_root, CFS_KIND_COPYSTATE);
    }
}

/*
 * The snapshot with the lowest id above `after` (`want`: that one id
 * exactly), its blocks of the list scrubbed on the way. Found again from
 * the head every time, because the walk lets the lock go between
 * snapshots and the list may have changed -- and by id, not by place,
 * because a deletion meanwhile moves every later entry up one and a
 * walk by place skips the next (Codex on #434). Ids are unique and never
 * reused (CF18), so "the next id" is a stable cursor. A list is a few
 * blocks, so the walks cost little.
 */
static bool snapshot_find(struct scrub *s, uint64_t after, uint64_t want, struct cfs_snapshot *out)
{
    struct cfs *fs = s->fs;
    uint64_t blk = fs->sb.snap_root;
    unsigned guard = 0, slots = cfs_snaplist_slots(fs);
    bool found = false;
    while (blk != 0 && guard++ < CFS_SNAP_MAX_CHAIN) {
        scrub_meta(s, blk, cfs_snaplist_kind(fs));
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blk, cfs_snaplist_kind(fs), &b))
            return found;
        uint64_t next = cfs_snaplist_next(b);
        for (unsigned i = 0; i < slots; i++) {
            struct cfs_snapshot sn;
            if (!cfs_snaplist_get(fs, b, i, &sn) || sn.id == 0)
                continue;
            if (want != 0 ? sn.id == want : sn.id > after && (!found || sn.id < out->id)) {
                *out = sn;
                found = true;
            }
        }
        cfs_buf_put(fs, b);
        blk = next;
    }
    return found;
}

/* The roots of tree `id` (0: the live tree), now. */
static bool tree_roots(struct scrub *s, uint64_t id, uint64_t *imap_root, uint64_t *next_ino)
{
    struct cfs *fs = s->fs;
    if (id == 0) {
        *imap_root = fs->sb.imap_root;
        *next_ino = fs->sb.next_ino;
        return true;
    }
    struct cfs_snapshot sn;
    if (!snapshot_find(s, 0, id, &sn))
        return false;   /* deleted while the scrub paused */
    cfs_tree_roots(fs, &sn, imap_root, next_ino);   /* a clone's move with the open transaction */
    return true;
}

static bool inode_again(struct scrub *s, uint64_t id, uint64_t ino, struct cfs_inode *in)
{
    uint64_t root, next_ino;
    if (!tree_roots(s, id, &root, &next_ino))
        return false;
    return cfs_inode_read_at(s->fs, root, next_ino, ino, in) == 0;
}

/* One inode's own trees and its data blocks. */
static void scrub_inode(struct scrub *s, uint64_t id, const struct cfs_inode *inp)
{
    struct cfs *fs = s->fs;
    struct cfs_inode in = *inp;
    s->st.inodes++;
    uint64_t next = in.indirect;
    unsigned guard = 0;
    while (next && guard++ < CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2) {
        scrub_meta(s, next, CFS_KIND_EXTENTS);
        struct cfs_buf *b;
        if (cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &b))
            break;
        next = ((const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE))->next;
        cfs_buf_put(fs, b);
    }
    if (in.csum_root != 0)
        scrub_csum(s, in.csum_root, 0);
    uint64_t blocks = (in.size + CFS_BLOCK - 1) / CFS_BLOCK;
    for (uint64_t lblk = 0; lblk < blocks && !s->stop;) {
        struct cfs_extent e;
        int rc = cfs_map_ext(fs, &in, lblk, &e);
        if (rc != 1) {
            lblk++;      /* a hole, or a mapping that could not be read */
            continue;
        }
        if (cfs_ext_compressed(&e)) {
            /* A record's checksums cover its physical blocks, so those
             * are what is checked -- once for the record, not once per
             * logical block it holds. */
            for (uint32_t i = 0; i < cfs_ext_psize(&e); i++)
                scrub_data(s, &in, e.lblk + i, e.start + i);
            lblk = e.lblk + cfs_ext_count(&e);
        } else {
            scrub_data(s, &in, lblk, e.start + (lblk - e.lblk));
            lblk++;
        }
        if (pace(s) && !inode_again(s, id, in.ino, &in))
            return;   /* gone, or no longer what it was: its blocks were someone else's to scrub */
        blocks = (in.size + CFS_BLOCK - 1) / CFS_BLOCK;
    }
}

/*
 * Every block of one tree's inode map and every inode in it, one inode
 * block at a time: the lock may go between them, and the next is found
 * from the roots as they are then. Walking the map rather than reading
 * inodes by number is what lets each imap and inode block be checked on
 * *every* copy: an ordinary read stops at the first copy that verifies.
 */
static void scrub_tree(struct scrub *s, uint64_t id)
{
    struct cfs *fs = s->fs;
    for (uint64_t bi = 0; !s->stop; bi++) {
        uint64_t root, next_ino;
        if (!tree_roots(s, id, &root, &next_ino) || bi * CFS_INODES_PER_BLOCK >= next_ino || root == 0)
            return;
        scrub_meta(s, root, CFS_KIND_IMAP1);
        struct cfs_buf *b1;
        if (cfs_buf_get(fs, root, CFS_KIND_IMAP1, &b1))
            return;
        uint64_t l0 = ((const uint64_t *)(b1->data + CFS_MHDR_SIZE))[bi / CFS_PTRS_PER_BLOCK];
        cfs_buf_put(fs, b1);
        if (l0 == 0) {
            bi = (bi / CFS_PTRS_PER_BLOCK + 1) * CFS_PTRS_PER_BLOCK - 1;   /* the next l0's first */
            continue;
        }
        scrub_meta(s, l0, CFS_KIND_IMAP0);
        struct cfs_buf *b0;
        if (cfs_buf_get(fs, l0, CFS_KIND_IMAP0, &b0))
            continue;
        uint64_t ib = ((const uint64_t *)(b0->data + CFS_MHDR_SIZE))[bi % CFS_PTRS_PER_BLOCK];
        cfs_buf_put(fs, b0);
        if (ib == 0)
            continue;
        scrub_meta(s, ib, CFS_KIND_INODES);
        struct cfs_buf *bib;
        if (cfs_buf_get(fs, ib, CFS_KIND_INODES, &bib))
            continue;
        memcpy(s->inodes, bib->data + CFS_MHDR_SIZE, (size_t)CFS_INODES_PER_BLOCK * CFS_INODE_SIZE);
        cfs_buf_put(fs, bib);
        for (unsigned k = 0; k < CFS_INODES_PER_BLOCK && !s->stop; k++) {
            uint64_t copied = fs->commits;
            const struct cfs_inode *in = (const struct cfs_inode *)(s->inodes + (size_t)k * CFS_INODE_SIZE);
            if (in->nlink == 0 || in->ino == 0)
                continue;
            scrub_inode(s, id, in);
            /* The lock went during that inode and a commit came: the
             * rest of the block's copies may name what is now another
             * file's (Codex on #434). Each is read again by number. */
            if (fs->commits != copied && k + 1 < CFS_INODES_PER_BLOCK) {
                uint64_t r, ni;
                bool have = tree_roots(s, id, &r, &ni);
                for (unsigned j = k + 1; j < CFS_INODES_PER_BLOCK; j++) {
                    struct cfs_inode *slot = (struct cfs_inode *)(s->inodes + (size_t)j * CFS_INODE_SIZE);
                    uint64_t ino = bi * CFS_INODES_PER_BLOCK + j;
                    if (!have || cfs_inode_read_at(fs, r, ni, ino, slot) != 0)
                        memset(slot, 0, sizeof(*slot));   /* gone, or not readable now: skipped */
                }
            }
        }
        (void)pace(s);
    }
}

/* The snapshot list, every deadlist, and every snapshot's and clone's
 * tree. A snapshot shares most of its blocks with the live tree; `done`
 * reads each of those once. */
static void scrub_snapshots(struct scrub *s)
{
    struct cfs *fs = s->fs;
    struct cfs_snapshot sn;
    for (uint64_t last = 0; !s->stop && snapshot_find(s, last, 0, &sn); last = sn.id) {
        uint64_t dead = sn.deadlist;
        unsigned guard = 0;
        while (dead && guard++ < CFS_SNAP_MAX_CHAIN) {
            scrub_meta(s, dead, CFS_KIND_DEADLIST);
            struct cfs_buf *db;
            if (cfs_buf_get(fs, dead, CFS_KIND_DEADLIST, &db))
                break;
            dead = ((const struct cfs_dead_block *)(db->data + CFS_MHDR_SIZE))->next;
            cfs_buf_put(fs, db);
        }
        (void)pace(s);
        scrub_tree(s, sn.id);
    }
}

/*
 * The whole filesystem, in the order a mount would need it: the
 * allocation maps, the live tree, the snapshots and clones.
 */
int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out, unsigned flags)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    struct scrub s;
    memset(&s, 0, sizeof(s));
    s.fs = fs;
    s.repair = (flags & COSMOFS_SCRUB_REPAIR) != 0 && !fs->rdonly;
    unsigned rate = __atomic_load_n(&g_cosmofs_scrub_rate, __ATOMIC_RELAXED);
    s.st.rate = rate;
    cfs_pace_init(&s.pace, rate);
    s.page = kmalloc(CFS_BLOCK, 0);
    s.slots = kmalloc((size_t)CFS_PTRS_PER_BLOCK * sizeof(uint64_t), 0);
    s.inodes = kmalloc((size_t)CFS_INODES_PER_BLOCK * CFS_INODE_SIZE, 0);
    uint64_t t0 = clock_now_ns();

    mutex_lock(&fs->lock);
    int rc = fs->failed ? -EIO : 0;
    if (rc == 0 && (s.page == NULL || s.inodes == NULL || s.slots == NULL || cfs_bits_alloc(&s.done, fs->nblocks) != 0))
        rc = -ENOMEM;   /* before the walk, never half way through it */
    if (rc == 0) {
        s.pace.window = clock_now_ns();
        s.commits = fs->commits;
        scrub_supers(&s);
        scrub_alloc(&s);
        /* Version 12's rollback record: metadata like the list. */
        if (fs->sb.version >= 12 && fs->sb.rollback_root != 0)
            scrub_meta(&s, fs->sb.rollback_root, CFS_KIND_ROLLBACK);
        scrub_tree(&s, 0);
        scrub_snapshots(&s);
        if (s.stop)
            rc = s.err;   /* abandoned during a pause, or no memory to look */
    }
    mutex_unlock(&fs->lock);
    cfs_bits_free(&s.done);
    kfree(s.page);
    kfree(s.inodes);
    kfree(s.slots);
    if (rc)
        return rc;
    s.st.elapsed_ns = clock_since_ns(t0);
    kinfo("cosmofs: scrub read %llu blocks over %llu inodes: %llu bad copies, %llu repaired, %llu unrecoverable, "
          "%llu skipped (%llu ms, %u blocks/s bound)",
          (unsigned long long)s.st.blocks_read, (unsigned long long)s.st.inodes,
          (unsigned long long)s.st.bad_copies, (unsigned long long)s.st.repaired,
          (unsigned long long)s.st.unrecoverable, (unsigned long long)s.st.skipped,
          (unsigned long long)(s.st.elapsed_ns / 1000000), rate);
    if (out)
        *out = s.st;
    /* A block no copy could satisfy is a finding, as a check's are: the
     * pass ran and the counts say what it found (CFS-11). */
    return 0;
}
