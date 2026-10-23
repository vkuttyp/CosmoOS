/*
 * cosmofs_resilver.c - Rebuilding a mirror's copy: the resilver, the
 * operator's commands on copies and spares, and what a mount starts
 * (roadmap 1.2 B3; docs/kernel-services/filesystem/cosmofs/design.md,
 * "Resilvering").
 *
 * One thread a mount does the copying, under fs->lock a window at a time
 * (fs.resilver_rate, paced as the scrub is), so the pool stays live: a
 * write that lands meanwhile goes to every writable copy, the one being
 * rebuilt included, and the cursor's walk skips nothing it needs to copy
 * again because nothing it has copied can change without that write.
 */

#include <kernel/blk.h>
#include <kernel/timer.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/string.h>
#include <kernel/thread.h>

#include "cosmofs_internal.h"

unsigned g_cosmofs_resilver_rate = COSMOFS_RESILVER_RATE_DEFAULT;
#if CONFIG_DEBUG
/* Test seam: every mount's resilver thread copies nothing while set; the
 * self-tests drive it with cosmofs_test_resilver_run instead. */
static bool g_rs_hold;
#endif

#define RS_IDLE_MS 100u
/* The cursor reaches the record every this many blocks copied (a commit
 * is made due), so a reboot repeats at most this much. */
#define RS_RECORD_BLOCKS 2048u

static uint64_t new_tag(void)
{
    uint64_t t;
    do
        t = random_u64();
    while (t == 0);
    return t;
}

/* --- starting one -------------------------------------------------------- */

static void rs_wake(struct cfs *fs);

/*
 * Copy `c` of member `v` becomes RESILVERING: its device is labelled as
 * the target with a new tag (found again by it after a reboot), and from
 * this moment every write reaches it. What it is missing is what its
 * record says -- the regions it missed, or everything (`whole`).
 */
static int rs_start(struct cfs *fs, unsigned v, unsigned c)
{
    struct pool_copy *cp = &fs->pool->m[v].copy[c];
    if (cp->dev == NULL)
        return -ENODEV;
    if (fs->sb.version < 11)
        return -EOPNOTSUPP;   /* the record that finds the target again cannot be kept (design.md) */
    uint64_t tag = cp->tag ? cp->tag : new_tag();
    int rc = cfs_label_write_dev(fs, cp->dev, v, CFS_LABEL_RESILVER, tag, fs->mem[v].nblocks);
    if (rc)
        return rc;
    cp->tag = tag;
    cp->cursor = 0;
    if (cp->state != POOL_COPY_RESILVERING) {
        /* Not through pool_copy_set_state: a copy that was ONLINE keeps
         * nothing to start from, and one that was out keeps its bitmap. */
        enum pool_copy_state was = cp->state;
        cp->state = POOL_COPY_RESILVERING;
        fs->pool->state_seq++;
        if (fs->pool->notify)
            fs->pool->notify(fs->pool->notify_arg);
        kwarn("cosmofs: %s: member %u copy %u (%s) RESILVERING, was %s: %s", fs->pool_name, v, c, cp->dev->name,
              pool_copy_state_name(was), cp->whole ? "every block" : "the regions it missed");
    }
    rs_wake(fs);
    return 0;
}

/* A spare takes the place of copy `c`, which is FAULTED or MISSING: the
 * takeover is logged, and the spare is a member once it is resilvered. */
static bool rs_takeover(struct cfs *fs, unsigned v, unsigned c)
{
    struct pool_copy *cp = &fs->pool->m[v].copy[c];
    uint64_t tag = 0;
    struct blkdev *spare = pool_take_spare(fs->pool, v, &tag);
    if (spare == NULL)
        return false;
    char was[BLKDEV_NAME_MAX];
    strlcpy(was, cp->dev ? cp->dev->name : "no device", sizeof(was));
    enum pool_copy_state state = cp->state;
    int rc = pool_copy_replace_dev(fs->pool, v, c, spare, POOL_COPY_RESILVERING);
    if (rc) {
        kerror("cosmofs: %s: spare %s could not take member %u copy %u (%d)", fs->pool_name, spare->name, v, c, rc);
        (void)pool_add_spare(fs->pool, spare, tag);   /* the slot never had it */
        blkdev_put(spare);
        return false;
    }
    cp->tag = tag;
    rc = rs_start(fs, v, c);
    if (rc) {
        /* The slot has the spare now and it cannot be labelled: a spare
         * that does not take a label is no spare, and the copy is out
         * either way (Codex on #440: back on the spare list it would be a
         * copy and a spare at once). */
        kerror("cosmofs: %s: spare %s took member %u copy %u and cannot be written (%d); FAULTED", fs->pool_name,
               spare->name, v, c, rc);
        cp->state = POOL_COPY_FAULTED;
        fs->pool->state_seq++;
        blkdev_put(spare);
        cfs_holds_sync(fs);
        return false;
    }
    kwarn("cosmofs: %s: spare %s takes over member %u copy %u (was %s, %s)", fs->pool_name, spare->name, v, c, was,
          pool_copy_state_name(state));
    blkdev_put(spare);   /* the pool holds it now */
    cfs_holds_sync(fs);
    return true;
}

/* --- the copying --------------------------------------------------------- */

/*
 * The walk copies blocks as they are, without knowing what each one is, so
 * it cannot check one against its checksum. What it can check: when a
 * second ONLINE copy has the block too, that the two agree. A copy being
 * replaced while it is still good is such a second source; if the first
 * one rotted, the walk has just copied the rot, and detaching the good
 * copy at the end would leave none (Codex on #440). So a disagreement is
 * counted, and rs_finish keeps the copy it would have detached until a
 * scrub, which does know what each block is, has put the copies right.
 */
static void rs_check_sources(struct cfs *fs, unsigned v, unsigned target, uint64_t dva, const uint8_t *got)
{
    struct spool *p = fs->pool;
    unsigned first = POOL_MAX_COPIES;
    for (unsigned k = 0; k < p->m[v].ncopies; k++)
        if (k != target && pool_copy_readable(p, dva, k)) {
            if (first == POOL_MAX_COPIES) {
                first = k;   /* the one pool_read took */
                continue;
            }
            uint8_t *other = kmalloc(CFS_BLOCK, 0);
            if (other == NULL)
                return;
            if (pool_read_copy(p, dva, k, other) == 0 && memcmp(other, got, CFS_BLOCK) != 0) {
                if (fs->rs_disagree++ < 8)
                    kwarn("cosmofs: %s: block %u:%llu differs between member %u copies %u and %u", fs->pool_name, v,
                          (unsigned long long)POOL_DVA_BLK(dva), v, first, k);
            }
            kfree(other);
            return;
        }
}

/* The copy being rebuilt, if any: the first RESILVERING copy with a device
 * whose member has a copy to read from. */
static bool rs_target(struct cfs *fs, unsigned *vp, unsigned *cp)
{
    struct spool *p = fs->pool;
    for (unsigned v = 0; v < p->nmembers; v++) {
        unsigned online = 0;
        for (unsigned c = 0; c < p->m[v].ncopies; c++)
            online += pool_copy_online(p, v, c) ? 1u : 0u;
        if (online == 0)
            continue;
        for (unsigned c = 0; c < p->m[v].ncopies; c++) {
            const struct pool_copy *k = &p->m[v].copy[c];
            if (k->state == POOL_COPY_RESILVERING && k->dev != NULL) {
                *vp = v;
                *cp = c;
                return true;
            }
        }
        /* Then the holes of an ONLINE copy (a write it failed while
         * another copy took it), with another copy to fill them from. */
        for (unsigned c = 0; c < p->m[v].ncopies && online > 1; c++) {
            const struct pool_copy *k = &p->m[v].copy[c];
            if (k->state == POOL_COPY_ONLINE && k->dev != NULL && pool_copy_any_missed(p, v, c)) {
                *vp = v;
                *cp = c;
                return true;
            }
        }
    }
    return false;
}

/*
 * The end of a pass: the target holds every allocated block it missed,
 * and everything written since its resilver began. Last come the blocks
 * below first_usable -- member 0's superblocks, another member's label --
 * which turn the device from a target into a copy; they are copied (or
 * written) only now, with no commit running (fs->lock), so the target's
 * root is exactly the one its blocks belong to. Then ONLINE, and the
 * copies it was replacing are detached.
 */
static int rs_finish(struct cfs *fs, unsigned v, unsigned c, uint8_t *buf)
{
    struct spool *p = fs->pool;
    struct pool_copy *cp = &p->m[v].copy[c];
    int rc = 0;
    if (v == 0) {
        for (uint64_t blk = 0; blk < fs->mem[0].first_usable && rc == 0; blk++) {
            rc = pool_read(p, CFS_DVA(0, blk), buf);
            if (rc == 0)
                rc = pool_write_copy(p, CFS_DVA(0, blk), c, buf);
        }
    } else {
        rc = cfs_label_write(p, v, c, fs->sb.generation, fs->sb.uuid, fs->mem[v].nblocks);
    }
    if (rc == 0)
        rc = blk_flush(cp->dev);
    if (rc)
        return rc;
    uint64_t tag = cp->tag;
    char name[BLKDEV_NAME_MAX];
    strlcpy(name, cp->dev->name, sizeof(name));
    cp->tag = 0;   /* a copy now: its superblock or label says what it is */
    rc = pool_copy_resilvered(p, v, c);
    if (rc)
        return rc;
    kinfo("cosmofs: %s: %s resilvered as member %u copy %u: %llu blocks copied in %llu ms", fs->pool_name, name, v, c,
          (unsigned long long)fs->rs_copied, (unsigned long long)(clock_since_ns(fs->rs_started_ns) / 1000000ull));
    fs->rs_copied = 0;
    fs->rs_started_ns = 0;
    /* A replace: what this copy was replacing goes now (design.md,
     * "Replacing a copy") -- unless two of the copies it was built from
     * disagreed, when the one being replaced may be the only good one. */
    if (fs->rs_disagree) {
        kwarn("cosmofs: %s: %llu block(s) differed between the copies %s was built from; the copy it replaces "
              "stays until `fsctl scrub --repair` and a `cosmo-pool detach`", fs->pool_name,
              (unsigned long long)fs->rs_disagree, name);
        for (unsigned k = 0; k < p->m[v].ncopies; k++)
            if (p->m[v].copy[k].peer == tag)
                p->m[v].copy[k].peer = 0;
        fs->rs_disagree = 0;
    }
    for (unsigned k = 0; k < p->m[v].ncopies;) {
        struct pool_copy *old = &p->m[v].copy[k];
        if (old->peer == 0 || old->peer != tag) {
            k++;
            continue;
        }
        struct blkdev *gone = old->dev;
        if (gone)
            blkdev_get(gone);
        if (pool_detach_copy(p, v, k) == 0) {
            fs->mem[v].copies = p->m[v].ncopies;
            if (gone) {
                cfs_label_clear_dev(gone);
                kinfo("cosmofs: %s: %s replaced and detached", fs->pool_name, gone->name);
            }
        } else {
            k++;
        }
        if (gone)
            blkdev_put(gone);
    }
    cfs_holds_sync(fs);
    return 0;
}

/*
 * One window's worth of the resilver, under fs->lock. Returns false when
 * there was nothing to do. The cursor walks the member's blocks; a region
 * is copied when its bit says the copy missed it, or when the cursor is
 * already inside it (the bit is cleared as the region is begun, so a
 * write that fails on the target during it sets it again, and a reboot
 * that stops the walk inside it finishes the region before trusting the
 * bits again).
 */
static bool rs_step(struct cfs *fs, uint8_t *buf, struct cfs_pace *pace, uint64_t *budget)
{
    struct spool *p = fs->pool;
    if (fs->failed || fs->rdonly)
        return false;
    /* A copy whose device was unregistered (removed: the block layer
     * answers -ENODEV from now on) is MISSING now, not at its next I/O:
     * an idle pool learns it too, and a spare can take over. */
    for (unsigned v = 0; v < p->nmembers; v++)
        for (unsigned c = 0; c < p->m[v].ncopies; c++) {
            struct pool_copy *k = &p->m[v].copy[c];
            if (k->dev != NULL && k->dev->gone &&
                (k->state == POOL_COPY_ONLINE || k->state == POOL_COPY_RESILVERING))
                (void)pool_copy_set_state(p, v, c, POOL_COPY_MISSING, -ENODEV);
        }
    /* A spare for every copy that is out. */
    bool took = false;
    for (unsigned v = 0; v < p->nmembers && p->nspares > 0; v++)
        for (unsigned c = 0; c < p->m[v].ncopies && p->nspares > 0; c++) {
            enum pool_copy_state s = p->m[v].copy[c].state;
            if (s == POOL_COPY_FAULTED || s == POOL_COPY_MISSING)
                took = rs_takeover(fs, v, c) || took;
        }
    unsigned v, c;
    if (!rs_target(fs, &v, &c))
        return took;
    struct spool_member *m = &p->m[v];
    struct pool_copy *cp = &m->copy[c];
    if (fs->rs_started_ns == 0)
        fs->rs_started_ns = clock_now_ns();
    uint64_t rb = m->region_blocks;
    uint64_t end = fs->mem[v].nblocks;
    while (cp->cursor < end) {
        if ((cp->state != POOL_COPY_RESILVERING && cp->state != POOL_COPY_ONLINE) || cp->dev == NULL)
            return true;   /* the target failed or went: its state says so */
        uint64_t r = cp->cursor / rb;
        bool inside = cp->cursor % rb != 0;
        if (!inside && !pool_region_missed(p, v, c, r)) {
            cp->cursor = (r + 1) * rb;
            continue;
        }
        bool online = cp->state == POOL_COPY_ONLINE;
        if (!inside && online)
            fs->rs_region_errors = cp->write_errors;
        else if (!inside && !cp->whole)
            pool_region_clear(p, v, c, r);
        uint64_t blk = cp->cursor;
        cp->cursor++;
        /* An ONLINE copy's hole is read from no more than the rest of
         * it is: its bit goes when the whole region is filled, and only
         * if no write to the copy failed meanwhile (which would be a new
         * hole in it). A rebuilding copy is read from nowhere, so its bit
         * goes as the region begins (above). */
        if (online && (cp->cursor % rb == 0 || cp->cursor >= end) && cp->write_errors == fs->rs_region_errors)
            pool_region_clear(p, v, c, r);
        bool fixed = blk < fs->mem[v].first_usable;   /* the superblocks or the label */
        if (fixed && !online)
            continue;   /* a rebuilding copy takes them last (rs_finish); an ONLINE one's are as any hole */
        uint64_t dva = CFS_DVA(v, blk);
        if (!fixed && !cfs_bitmap_test(fs, cfs_dva_lin(fs, dva)))
            continue;   /* free: nothing to copy */
        int rc = pool_read(p, dva, buf);
        if (rc == 0)
            rs_check_sources(fs, v, c, dva, buf);
        if (rc == 0)
            (void)pool_write_copy(p, dva, c, buf);   /* a failure is the pool's: the bit set again, or FAULTED */
        else
            kwarn("cosmofs: %s: block %u:%llu reads from no copy (%d); member %u copy %u goes without it",
                  fs->pool_name, v, (unsigned long long)blk, rc, v, c);
        fs->rs_copied++;
        pace->used++;
        if (budget && --*budget == 0)
            return true;
        if (++fs->rs_since_record >= RS_RECORD_BLOCKS) {
            /* The cursor into the record: a commit due. */
            fs->rs_since_record = 0;
            p->state_seq++;
            if (p->notify)
                p->notify(p->notify_arg);
        }
        if (cfs_pace(fs, pace))
            return true;   /* the lock was let go: start again from the state as it is */
    }
    /* The pass is over. A whole rebuild has done everything; failures
     * on the target during it set bits again, and those go round. */
    cp->whole = false;
    if (pool_copy_any_missed(p, v, c)) {
        cp->cursor = 0;
        return true;
    }
    if (cp->state == POOL_COPY_ONLINE) {
        /* Holes filled: an ONLINE copy has no superblocks to take and no
         * state to change; its bits were cleared as its regions began. */
        cp->cursor = 0;
        kinfo("cosmofs: %s: member %u copy %u (%s): its holes are filled", fs->pool_name, v, c, cp->dev->name);
        p->state_seq++;
        if (p->notify)
            p->notify(p->notify_arg);
        return true;
    }
    int rc = rs_finish(fs, v, c, buf);
    if (rc) {
        kerror("cosmofs: %s: member %u copy %u: the resilver could not finish (%d)", fs->pool_name, v, c, rc);
        (void)pool_copy_set_state(p, v, c, POOL_COPY_FAULTED, rc);
    }
    return true;
}

static void rs_thread(void *arg)
{
    struct cfs *fs = arg;
    uint8_t *buf = fs->rs_buf;   /* rs_wake's: the thread exists only with it (Codex on #440) */
    struct cfs_pace pace;
    cfs_pace_init(&pace, __atomic_load_n(&g_cosmofs_resilver_rate, __ATOMIC_RELAXED));
    while (!__atomic_load_n(&fs->rs_stop, __ATOMIC_ACQUIRE)) {
        mutex_lock(&fs->lock);
#if CONFIG_DEBUG
        bool worked = !__atomic_load_n(&g_rs_hold, __ATOMIC_RELAXED) && rs_step(fs, buf, &pace, NULL);
#else
        bool worked = rs_step(fs, buf, &pace, NULL);
#endif
        mutex_unlock(&fs->lock);
        if (!worked) {
            thread_sleep_ms(RS_IDLE_MS);
            cfs_pace_init(&pace, __atomic_load_n(&g_cosmofs_resilver_rate, __ATOMIC_RELAXED));
        }
    }
}

/* With fs->lock held: the thread exists while the mount does (it sleeps
 * when there is nothing to rebuild). Not on a read-only or maintenance
 * mount: those write nothing. */
static void rs_wake(struct cfs *fs)
{
    if (fs->rs_thread != NULL || fs->rs_stop || fs->rdonly || fs->maint || !fs->mount_done)
        return;
    if (fs->rs_buf == NULL)
        fs->rs_buf = kmalloc(CFS_BLOCK, 0);
    if (fs->rs_buf == NULL) {
        kwarn("cosmofs: %s: no memory for the resilver; tried again at its next wake", fs->pool_name);
        return;
    }
    fs->rs_thread = thread_create(rs_thread, fs, "cfs-resilver", SCHED_PRIO_DEFAULT);
    if (fs->rs_thread == NULL)
        kwarn("cosmofs: %s: no resilver thread; tried again at its next wake", fs->pool_name);
}

void cfs_resilver_stop(struct cfs *fs)
{
    if (fs->rs_thread == NULL)
        return;
    __atomic_store_n(&fs->rs_stop, true, __ATOMIC_RELEASE);
    thread_join(fs->rs_thread);
    fs->rs_thread = NULL;
    kfree(fs->rs_buf);
    fs->rs_buf = NULL;
}

/*
 * After a mount: the pool's spares, the holds, and a resilver for every
 * copy that wants one -- a target a reboot interrupted, and the copy that
 * was away when its device is back (incremental when a record says what
 * it missed, else whole). "When the others are consistent": only into a
 * member with an ONLINE copy to read from. Boot is not held up: the
 * thread does it after the mount returns (design.md, "Import").
 */
void cfs_resilver_mount(struct cfs *fs)
{
    mutex_lock(&fs->lock);
    cfs_spares_find(fs);
    struct spool *p = fs->pool;
    bool writable = !fs->rdonly && !fs->maint && fs->sb.version >= 11;
    for (unsigned v = 0; v < p->nmembers && writable; v++) {
        if (!pool_member_degraded(p, v))
            continue;
        for (unsigned c = 0; c < p->m[v].ncopies; c++) {
            struct pool_copy *cp = &p->m[v].copy[c];
            if (cp->state == POOL_COPY_RESILVERING && cp->dev != NULL) {
                rs_wake(fs);
                continue;
            }
            if (cp->state != POOL_COPY_MISSING || cp->dev != NULL)
                continue;
            bool exact = false;
            struct blkdev *bd = cfs_find_lagging(fs, v, cp->whole ? 0 : cp->since, &exact);
            if (bd == NULL)
                continue;
            if (!exact) {
                cp->whole = true;   /* no record covers what this device missed */
                cp->since = 0;
            }
            int rc = pool_attach_copy(p, v, c, bd, POOL_COPY_MISSING);
            if (rc == 0)
                rc = rs_start(fs, v, c);
            kinfo("cosmofs: %s: %s is back as member %u copy %u: %s (%d)", fs->pool_name, bd->name, v, c,
                  exact ? "resilvering what it missed" : "resilvering it whole", rc);
            blkdev_put(bd);
        }
    }
    /* A mirrored pool or one with spares keeps the thread: it is what
     * notices a removed device while nothing is written (rs_step). */
    bool mirrored = p->nspares > 0;
    for (unsigned v = 0; v < p->nmembers; v++)
        mirrored = mirrored || p->m[v].ncopies > 1;
    if (writable && mirrored)
        rs_wake(fs);
    cfs_holds_sync(fs);
    mutex_unlock(&fs->lock);
}

#if CONFIG_DEBUG
/* Test seams: hold the resilver thread, and run the resilver here instead
 * for at most `blocks` blocks copied (0: until there is nothing left),
 * unpaced -- what a self-test needs to stop it at a known place. */
void cosmofs_test_resilver_hold(bool hold)
{
    __atomic_store_n(&g_rs_hold, hold, __ATOMIC_RELAXED);
}

int cosmofs_test_resilver_run(struct mount *mnt, uint64_t blocks)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    uint8_t *buf = kmalloc(CFS_BLOCK, 0);
    if (buf == NULL)
        return -ENOMEM;
    struct cfs_pace pace;
    cfs_pace_init(&pace, 0);
    mutex_lock(&fs->lock);
    uint64_t budget = blocks;
    for (unsigned guard = 0; guard < 1000000; guard++) {
        uint64_t before = budget;
        if (!rs_step(fs, buf, &pace, blocks ? &budget : NULL))
            break;
        if (blocks && budget == 0)
            break;
        (void)before;
    }
    mutex_unlock(&fs->lock);
    kfree(buf);
    return 0;
}
#endif

/* --- the operator's commands --------------------------------------------- */

/* "member:copy", or a device the pool holds as a copy. */
static int find_copy(struct cfs *fs, const char *name, unsigned *v, unsigned *c)
{
    if (name == NULL || name[0] == '\0')
        return -EINVAL;
    const char *colon = strchr(name, ':');
    if (colon != NULL) {
        unsigned a = 0, b = 0;
        const char *s = name;
        if (s == colon)
            return -EINVAL;
        for (; s < colon; s++) {
            if (*s < '0' || *s > '9')
                return -EINVAL;
            a = a * 10 + (unsigned)(*s - '0');
        }
        if (colon[1] == '\0')
            return -EINVAL;
        for (s = colon + 1; *s; s++) {
            if (*s < '0' || *s > '9')
                return -EINVAL;
            b = b * 10 + (unsigned)(*s - '0');
        }
        if (a >= fs->pool->nmembers || b >= fs->pool->m[a].ncopies)
            return -ENOENT;
        *v = a;
        *c = b;
        return 0;
    }
    struct blkdev *bd = blk_find(name);
    if (bd == NULL)
        return -ENODEV;
    bool found = pool_find_dev(fs->pool, bd, v, c);
    blkdev_put(bd);
    return found ? 0 : -ENOENT;
}

/* A device that is to join the pool: there, not the pool's already, not
 * another mount's, and big enough for member `v` (or, for a spare, `v`
 * is ignored). Referenced. */
static int new_device(struct cfs *fs, const char *name, int v, struct blkdev **out)
{
    if (name == NULL || name[0] == '\0')
        return -EINVAL;
    struct blkdev *bd = blk_find(name);
    if (bd == NULL)
        return -ENODEV;
    unsigned a, b;
    int rc = 0;
    if (pool_find_dev(fs->pool, bd, &a, &b) || bd == fs->mnt->bdev)
        rc = -EEXIST;
    for (unsigned i = 0; rc == 0 && i < fs->pool->nspares; i++)
        if (fs->pool->spare[i] == bd)
            rc = -EEXIST;
    if (rc == 0 && vfs_bdev_mounted(bd))
        rc = -EBUSY;
    if (rc == 0 && bd->read_only)
        rc = -EROFS;
    /* A sector the pool's block is a whole number of, before anything is
     * divided by it (Codex on #440: an 8 KiB sector divided by zero). */
    if (rc == 0 && (bd->sector_size == 0 || bd->sector_size > POOL_BLOCK || POOL_BLOCK % bd->sector_size != 0))
        rc = -EINVAL;
    if (rc == 0 && v >= 0 && bd->capacity / (POOL_BLOCK / bd->sector_size) < fs->mem[v].nblocks)
        rc = -ENOSPC;
    /* Claimed before anything is written to it: a mount that took it
     * after the check above would otherwise share it (Codex on #440). */
    if (rc == 0)
        rc = cfs_holds_sync_with(fs, bd);
    if (rc) {
        blkdev_put(bd);
        return rc;
    }
    *out = bd;
    return 0;
}

/* One more copy slot for member `v` (attach, or a replace beside a copy
 * still good): the member table grows, which is adding a copy and
 * nothing else. */
static int add_slot(struct cfs *fs, unsigned v, unsigned *c)
{
    if (fs->sb.version < 11)
        return -EOPNOTSUPP;   /* a copy state could not be recorded (design.md) */
    unsigned n = fs->pool->m[v].ncopies;
    if (n >= POOL_MAX_COPIES)
        return -ENOSPC;
    int rc = pool_set_copies(fs->pool, v, n + 1);
    if (rc)
        return rc;
    fs->mem[v].copies = n + 1;
    *c = n;
    return 0;
}

static int op_online(struct cfs *fs, const char *name)
{
    unsigned v, c;
    int rc = find_copy(fs, name, &v, &c);
    if (rc == -ENOENT && strchr(name, ':') == NULL) {
        /* A device that is back: a lagging copy of this pool, into a
         * MISSING slot of its member. */
        struct blkdev *bd = blk_find(name);
        if (bd == NULL)
            return -ENODEV;
        rc = -ENOENT;
        for (unsigned m = 0; m < fs->pool->nmembers && rc == -ENOENT; m++)
            for (unsigned k = 0; k < fs->pool->m[m].ncopies && rc == -ENOENT; k++) {
                struct pool_copy *cp = &fs->pool->m[m].copy[k];
                if (cp->state != POOL_COPY_MISSING || cp->dev != NULL)
                    continue;
                bool exact = false;
                struct blkdev *lag = cfs_find_lagging(fs, m, cp->whole ? 0 : cp->since, &exact);
                if (lag == bd) {
                    if (!exact) {
                        cp->whole = true;
                        cp->since = 0;
                    }
                    rc = pool_attach_copy(fs->pool, m, k, bd, POOL_COPY_MISSING);
                    if (rc == 0)
                        rc = rs_start(fs, m, k);
                }
                if (lag)
                    blkdev_put(lag);
            }
        blkdev_put(bd);
        return rc;
    }
    if (rc)
        return rc;
    struct pool_copy *cp = &fs->pool->m[v].copy[c];
    if (cp->state == POOL_COPY_ONLINE || cp->state == POOL_COPY_RESILVERING)
        return 0;
    if (cp->dev == NULL)
        return -ENODEV;
    /* Missed nothing: ONLINE at once. Otherwise what it missed, rebuilt. */
    rc = pool_copy_set_state(fs->pool, v, c, POOL_COPY_ONLINE, 0);
    if (rc == -EAGAIN) {
        cp->read_errors = cp->write_errors = cp->csum_errors = 0;
        rc = rs_start(fs, v, c);
    }
    return rc;
}

static int op_replace(struct cfs *fs, const char *old, const char *new)
{
    unsigned v, c;
    int rc = find_copy(fs, old, &v, &c);
    if (rc)
        return rc;
    struct blkdev *bd;
    rc = new_device(fs, new, (int)v, &bd);
    if (rc)
        return rc;
    struct pool_copy *cp = &fs->pool->m[v].copy[c];
    if (pool_copy_online(fs->pool, v, c) || cp->state == POOL_COPY_RESILVERING) {
        /* The old copy still serves until the new one holds everything:
         * the new one gets a slot of its own, and the old one is detached
         * when it is done (design.md, "Replacing a copy"). */
        unsigned n;
        rc = add_slot(fs, v, &n);
        if (rc == 0)
            rc = pool_copy_replace_dev(fs->pool, v, n, bd, POOL_COPY_RESILVERING);
        if (rc == 0) {
            struct pool_copy *nc = &fs->pool->m[v].copy[n];
            nc->tag = new_tag();
            fs->pool->m[v].copy[c].peer = nc->tag;
            rc = rs_start(fs, v, n);
        }
    } else {
        /* The old copy is out: the new device takes its slot. */
        struct blkdev *gone = cp->dev;
        if (gone)
            blkdev_get(gone);
        rc = pool_copy_replace_dev(fs->pool, v, c, bd, POOL_COPY_RESILVERING);
        if (rc == 0)
            rc = rs_start(fs, v, c);
        if (rc == 0 && gone)
            cfs_label_clear_dev(gone);
        if (gone)
            blkdev_put(gone);
    }
    if (rc == 0)
        kinfo("cosmofs: %s: replacing member %u copy %u with %s", fs->pool_name, v, c, bd->name);
    blkdev_put(bd);
    cfs_holds_sync(fs);
    return rc;
}

static int op_attach(struct cfs *fs, const char *existing, const char *new)
{
    unsigned v, c;
    int rc = find_copy(fs, existing, &v, &c);
    if (rc == -ENOENT && fs->mnt->bdev != NULL && strcmp(existing, fs->mnt->bdev->name) == 0) {
        v = 0;
        rc = 0;
    }
    if (rc)
        return rc;
    struct blkdev *bd;
    rc = new_device(fs, new, (int)v, &bd);
    if (rc)
        return rc;
    unsigned n;
    rc = add_slot(fs, v, &n);
    if (rc == 0)
        rc = pool_copy_replace_dev(fs->pool, v, n, bd, POOL_COPY_RESILVERING);
    if (rc == 0)
        rc = rs_start(fs, v, n);
    if (rc == 0)
        kinfo("cosmofs: %s: %s attached as member %u copy %u", fs->pool_name, bd->name, v, n);
    blkdev_put(bd);
    cfs_holds_sync(fs);
    return rc;
}

static int op_detach(struct cfs *fs, const char *name)
{
    unsigned v, c;
    int rc = find_copy(fs, name, &v, &c);
    if (rc)
        return rc;
    struct blkdev *gone = fs->pool->m[v].copy[c].dev;
    if (gone)
        blkdev_get(gone);
    rc = pool_detach_copy(fs->pool, v, c);
    if (rc == 0) {
        fs->mem[v].copies = fs->pool->m[v].ncopies;
        /* A detached copy no longer names this pool. */
        if (gone)
            cfs_label_clear_dev(gone);
        cfs_holds_sync(fs);
    }
    if (gone)
        blkdev_put(gone);
    return rc;
}

static int op_spare_add(struct cfs *fs, const char *name)
{
    if (fs->sb.version < 4)
        return -EOPNOTSUPP;   /* no pool uuid to label it with */
    struct blkdev *bd;
    int rc = new_device(fs, name, -1, &bd);
    if (rc)
        return rc;
    uint64_t tag = new_tag();
    uint64_t blocks = bd->capacity / (POOL_BLOCK / bd->sector_size);
    rc = cfs_label_write_dev(fs, bd, 0, CFS_LABEL_SPARE, tag, blocks);
    if (rc == 0)
        rc = pool_add_spare(fs->pool, bd, tag);
    if (rc == 0) {
        kinfo("cosmofs: %s: %s is a spare", fs->pool_name, bd->name);
        cfs_holds_sync(fs);
        rs_wake(fs);   /* a copy that is out already is taken over now */
    }
    blkdev_put(bd);
    return rc;
}

static int op_spare_remove(struct cfs *fs, const char *name)
{
    struct blkdev *bd = blk_find(name ? name : "");
    if (bd == NULL)
        return -ENODEV;
    int rc = pool_remove_spare(fs->pool, bd);
    if (rc == 0) {
        cfs_label_clear_dev(bd);
        cfs_holds_sync(fs);
    }
    blkdev_put(bd);
    return rc;
}

static void status(struct cfs *fs, struct cosmofs_pool_req *req)
{
    const struct spool *p = fs->pool;
    req->generation = fs->sb.generation;
    req->members = fs->nmembers;
    req->format = fs->sb.version;
    req->rate = __atomic_load_n(&g_cosmofs_resilver_rate, __ATOMIC_RELAXED);
    req->degraded = pool_degraded(p);
    req->resilvering = false;
    memcpy(req->label, fs->sb.label, sizeof(req->label) < CFS_LABEL_MAX ? sizeof(req->label) : CFS_LABEL_MAX);
    req->count = 0;
    req->total = 0;
    for (unsigned v = 0; v < p->nmembers; v++) {
        const struct spool_member *m = &p->m[v];
        for (unsigned c = 0; c < m->ncopies; c++) {
            const struct pool_copy *cp = &m->copy[c];
            if (cp->state == POOL_COPY_RESILVERING)
                req->resilvering = true;
            req->total++;
            if (req->count >= req->max)
                continue;
            struct cosmofs_copy_info *o = &req->out[req->count++];
            memset(o, 0, sizeof(*o));
            o->member = v;
            o->copy = c;
            o->state = (unsigned)cp->state;
            o->flags = (cp->whole ? COSMOFS_COPY_I_WHOLE : 0u) | (cp->peer ? COSMOFS_COPY_I_DETACH : 0u);
            o->since = cp->state == POOL_COPY_ONLINE ? fs->sb.generation : cp->since;
            o->cursor = cp->cursor;
            o->blocks = fs->mem[v].nblocks;
            if (cp->state == POOL_COPY_RESILVERING || cp->state != POOL_COPY_ONLINE) {
                uint64_t todo = 0;
                for (uint64_t r = 0; r * m->region_blocks < o->blocks; r++) {
                    uint64_t first = r * m->region_blocks, last = first + m->region_blocks;
                    if (last > o->blocks)
                        last = o->blocks;
                    if (last <= cp->cursor && cp->state == POOL_COPY_RESILVERING)
                        continue;
                    if (pool_region_missed(p, v, c, r))
                        todo += last - (first > cp->cursor || cp->state != POOL_COPY_RESILVERING ? first : cp->cursor);
                }
                o->to_do = todo;
            }
            o->read_errors = cp->read_errors;
            o->write_errors = cp->write_errors;
            o->csum_errors = cp->csum_errors;
            if (cp->dev)
                strlcpy(o->dev, cp->dev->name, sizeof(o->dev));
        }
    }
    req->total += p->nspares;
    for (unsigned i = 0; i < p->nspares && req->count < req->max; i++) {
        struct cosmofs_copy_info *o = &req->out[req->count++];
        memset(o, 0, sizeof(*o));
        o->member = 0xffff;
        o->copy = 0xffff;
        o->state = COSMOFS_COPY_SPARE;
        o->blocks = p->spare[i]->capacity / (POOL_BLOCK / p->spare[i]->sector_size);
        strlcpy(o->dev, p->spare[i]->name, sizeof(o->dev));
    }
}

int cosmofs_poolctl(struct mount *mnt, struct cosmofs_pool_req *req)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    bool changes = req->op != COSMOFS_POOL_STATUS;
    if (changes && (fs->rdonly || fs->maint))
        return -EROFS;
    mutex_lock(&fs->lock);
    int rc = fs->failed && changes ? -EIO : 0;
    if (rc == 0) {
        switch (req->op) {
        case COSMOFS_POOL_STATUS:
            status(fs, req);
            break;
        case COSMOFS_POOL_OFFLINE: {
            unsigned v, c;
            rc = find_copy(fs, req->name, &v, &c);
            if (rc == 0)
                rc = pool_copy_set_state(fs->pool, v, c, POOL_COPY_OFFLINE, 0);
            break;
        }
        case COSMOFS_POOL_ONLINE:
            rc = op_online(fs, req->name);
            break;
        case COSMOFS_POOL_REPLACE:
            rc = op_replace(fs, req->name, req->name2);
            break;
        case COSMOFS_POOL_ATTACH:
            rc = op_attach(fs, req->name, req->name2);
            break;
        case COSMOFS_POOL_DETACH:
            rc = op_detach(fs, req->name);
            break;
        case COSMOFS_POOL_SPARE_ADD:
            rc = op_spare_add(fs, req->name);
            break;
        case COSMOFS_POOL_SPARE_REMOVE:
            rc = op_spare_remove(fs, req->name);
            break;
        default:
            rc = -EINVAL;
            break;
        }
    }
    mutex_unlock(&fs->lock);
    /* A change of copies is a change of the pool: committed now, so the
     * command's answer is what a reboot finds. */
    if (rc == 0 && changes)
        rc = cosmofs_commit_mount(mnt);
    return rc;
}
