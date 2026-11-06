/*
 * cosmofs_core.c - Buffers, allocation, the inode map, transactions,
 * commit, format, mount and unmount.
 *
 * Rules (docs/kernel-services/vfs/design.md): a metadata block from a
 * committed generation is never modified in place; the in-memory bitmap
 * is authoritative during a transaction and its chunks are written at
 * commit through a reserve-then-write fixpoint; blocks freed in a
 * transaction become allocatable only after that transaction's root is
 * on disk.
 */

#include <kernel/blake2b.h>
#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/lockdep.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/fwcfg.h>
#include <kernel/random.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <uapi/cosmo/fsctl.h>   /* COSMO_FSCTL_STAGE_*: where a mount stopped */

#include "cosmofs_internal.h"

/* --- the open transaction's first change ------------------------------- */

static void cfs_writeback_thread(void *arg);

/*
 * The open transaction just became (or stayed) non-empty. The writeback
 * thread starts here, on the first change, so a mount that only reads
 * (the replay harness mounts hundreds of prefix images) never has one to
 * join at unmount.
 *
 * Not before the mount is live, though. A mount's replay dirties buffers
 * (the freelog reclaim and the orphan replay both do) and it does that
 * with fs->lock unheld, because until cosmofs_mount returns nobody else
 * has the filesystem. A thread started here would falsify that: it takes
 * only the mount's sync lock, which the mount path does not hold, finds
 * mnt->unmounted false, and commits a half-built mount from the other
 * side of an unlocked fs->bufs. That is a second writer on an intrusive
 * list, and it showed up as a metadata block that would not verify and,
 * on x86_64, as a null dereference in list_remove under a commit the
 * writeback thread ran while the mount was still replaying. The same
 * window is why a failed mount could reach cfs_destroy, which frees
 * every buffer and fs itself, with that thread still running.
 *
 * So the rule is: no autonomous committer before the mount is live.
 * cosmofs_mount sets mount_done and calls back here, which starts the
 * thread then if the replay left anything dirty.
 */
void cfs_note_dirty(struct cfs *fs)
{
    if (fs->first_dirty_ns == 0)
        fs->first_dirty_ns = clock_now_ns();
    /* A read-only mount never commits, so it has nothing for a committer
     * to do: its replay's changes stay in memory (CFS-1). */
    if (fs->wb_thread == NULL && fs->wb_enabled && !fs->wb_stop && fs->mnt && fs->mount_done && !fs->rdonly) {
        fs->wb_thread = thread_create(cfs_writeback_thread, fs, "cfs-wb", SCHED_PRIO_DEFAULT);
        if (fs->wb_thread == NULL) {
            fs->wb_enabled = false;
            kwarn("cosmofs: no writeback thread; commits happen on sync, fsync and unmount only");
        }
    }
    /* The invariant, counted rather than asserted: whether such a thread
     * wins the race is timing, but whether it exists at all is not, and
     * that is what cosmofs-mount-no-early-writeback reads. */
    if (!fs->mount_done) {
        fs->mount_dirty_notes++;
        if (fs->wb_thread != NULL)
            fs->wb_early++;
    }
}

static uint64_t *ptrs(struct cfs_buf *b)
{
    return (uint64_t *)(b->data + CFS_MHDR_SIZE);
}

/* --- the page cache's write-back --------------------------------------------- */

/* Write back every dirty page of every cached regular file. Called with
 * no cfs lock held (writepage takes it). */
int cfs_sync_vnodes(struct cfs *fs)
{
    struct mount *mnt = fs->mnt;
    int rc = 0;
    for (unsigned b = 0; b < VNODE_HASH; b++) {
        for (;;) {
            struct vnode *vn = NULL;
            /* The hash lock is a spinlock; a hashed vnode always holds a
             * reference (vnode_put unhashes before the last drop). */
            arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
            struct vnode *it;
            list_for_each_entry(it, &mnt->vnodes[b], hash_link) {
                if (it->type == VNODE_REG && it->pc.nr_dirty) {
                    vn = it;
                    vnode_get(vn);
                    break;
                }
            }
            spin_unlock_irqrestore(&mnt->lock, s);
            if (vn == NULL)
                break;
            mutex_lock(&vn->lock);
            int r = pagecache_sync(vn);
            mutex_unlock(&vn->lock);
            vnode_put(vn);
            if (r) {
                rc = r;
                break;
            }
        }
    }
    return rc;
}

/* --- mount / unmount ------------------------------------------------------------ */

static int super_read(struct cfs *fs, unsigned slot, struct cfs_super *out)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, slot, block);
    if (rc == 0) {
        const struct cfs_super *sb = (const struct cfs_super *)block;
        /* Version 2 mounts unchanged: snapshots only use fields it
         * already reserved, and its snap_root is 0 (no snapshots). */
        bool version_ok = sb->version >= CFS_VERSION_MIN && sb->version <= CFS_VERSION;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && !version_ok && sb->version != 0)
            kerror("cosmofs: slot %u is format version %u; this kernel reads versions %u to %u (reformat)", slot,
                   sb->version, CFS_VERSION_MIN, CFS_VERSION);
        /* The member table is not read yet -- it is named by this very
         * block -- so a pointer can only be checked for shape here.
         * What it actually addresses is checked when it is read: every
         * metadata block carries its own DVA (mhdr_check). Before
         * version 4 there is one member and the old exact checks hold. */
        uint64_t dev0 = fs->pool->m[0].nblocks;
        /* The seal last: a slot that is not a superblock at all is -EIO
         * whether or not a key is loaded, and only one that is a
         * version-14 encrypted pool's says -ENOKEY (design.md, "The key
         * handle"). Its MAC key comes with it. */
        struct cfs_mac mac;
        if (!cfs_super_shape_ok(sb, dev0))   /* cfsparse: magic, version, geometry, pointers' shape */
            rc = -EIO;
        else
            rc = cfs_super_verify(block, &mac);
        /* Said once per slot read: the test of the MAC domains (make
         * test-encrypt) and an operator both need to know which slot a
         * mount refused and why. */
        if (rc == -EIO && cfs_super_is_mac(sb) && version_ok)
            kerror("cosmofs: superblock slot %u (generation %llu) does not authenticate", slot,
                   (unsigned long long)sb->generation);
        if (rc == 0) {
            *out = *sb;
            if (mac.on && !fs->mac.on)
                fs->mac = mac;
            else if (mac.on != fs->mac.on || (mac.on && memcmp(mac.key, fs->mac.key, sizeof(mac.key)) != 0))
                rc = -EIO;   /* the other slot was another pool's: slots_disagree's case, refused here */
        }
        memset(&mac, 0, sizeof(mac));
    }
    kfree(block);
    return rc;
}

static int load_bitmap(struct cfs *fs)
{
    /* fs->nblocks and fs->nr_chunks describe the linear space the
     * members were laid out in (cfs_members_load). */
    fs->bitmap = kzalloc((size_t)((fs->nblocks + 7) / 8));
    fs->bitmap_dirty = kzalloc(fs->nr_chunks);
    if (fs->bitmap == NULL || fs->bitmap_dirty == NULL)
        return -ENOMEM;
    int rc = 0;
    uint64_t free = 0;
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        struct cfs_buf *idx;
        rc = cfs_buf_get(fs, m->alloc_root, CFS_KIND_ALLOCIDX, &idx);
        if (rc)
            break;
        uint64_t *slots = ptrs(idx);
        for (unsigned c = 0; c < m->nchunks && rc == 0; c++) {
            struct cfs_buf *bm;
            rc = cfs_buf_get(fs, slots[c], CFS_KIND_BITMAP, &bm);
            if (rc)
                break;
            size_t bytes = CFS_BITS_PER_BITMAP / 8;
            size_t off = (size_t)(m->chunk0 + c) * bytes;
            size_t total = (size_t)((fs->nblocks + 7) / 8);
            size_t n = off + bytes <= total ? bytes : total - off;
            memcpy(fs->bitmap + off, bm->data + CFS_MHDR_SIZE, n);
            cfs_buf_put(fs, bm);
        }
        cfs_buf_put(fs, idx);
        if (rc)
            break;
        /* The tail of a member's last chunk addresses nothing: mark it
         * taken so the allocator can never reach it. */
        for (uint64_t b = m->nblocks; b < (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP; b++)
            cfs_bit_set(fs->bitmap, m->base + b);
        uint64_t mfree = 0;
        for (uint64_t b = m->first_usable; b < m->nblocks; b++)
            mfree += cfs_bit_test(fs->bitmap, m->base + b) ? 0 : 1;
        m->free_blocks = mfree;
        m->alloc_hint = m->first_usable;
        free += mfree;
    }
    if (rc)
        return rc;
    fs->free_blocks = free;
    /* The superblock's count is the bitmap's as that commit wrote it, so
     * the two are compared now, before the replay below frees the blocks
     * the commit released after its root was durable: compared after it,
     * every mount following an unmount warned by the last commit's frees
     * (4-7 blocks, roadmap 1.1 P3). */
    if (fs->free_blocks != fs->sb.free_blocks)
        kwarn("cosmofs: free block count %llu differs from the superblock's %llu; using the bitmap",
              (unsigned long long)fs->free_blocks, (unsigned long long)fs->sb.free_blocks);

    /*
     * Finish what the last commit started. It cleared these blocks' bits
     * in memory after its root was durable and marked the chunks for the
     * next commit, which at an unmount never came -- so on disk they are
     * still allocated and reachable from nothing. The root that freed
     * them says which they are, and this is where that is read
     * (docs/audit/next-subsystem-unmount-leak.md).
     *
     * Idempotent on purpose: the record is not cleared here. A mount
     * that replays and unmounts without committing changes nothing, and
     * the next mount replays the same list to the same effect. The
     * commit that supersedes the record is what retires it.
     */
    fs->load_stage = COSMO_FSCTL_STAGE_FREE_RECORD;
    rc = cfs_freelog_replay(fs);
    if (rc)
        return rc;
    return 0;
}

static void cfs_destroy(struct cfs *fs)
{
    /* Nothing may still be committing while this frees the buffers and
     * fs. cosmofs_unmount has already stopped the thread; a mount that
     * failed has none to stop, and this says so rather than trusting it. */
    cfs_resilver_stop(fs);
    if (fs->wb_thread) {
        __atomic_store_n(&fs->wb_stop, true, __ATOMIC_RELEASE);
        thread_join(fs->wb_thread);
        fs->wb_thread = NULL;
    }
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    kfree(fs->bitmap);
    kfree(fs->bitmap_dirty);
    kfree(fs->pending_free);
    kfree(fs->pending_exempt);
    kfree(fs->orphans);
    cfs_clones_free(fs);
    cfs_members_free(fs);
    if (fs->pool)
        pool_close(fs->pool);
    /* The key goes with the mount: its copy here is wiped, and the last
     * writable mount's release wipes the table's (design.md, "The key
     * handle"). */
    if (fs->key_held)
        cfs_keytab_release(fs->sb.uuid);
    memset(fs->master_key, 0, sizeof(fs->master_key));
    memset(&fs->mac, 0, sizeof(fs->mac));
    kfree(fs);
}

/* Drop every buffer and the bitmap so a different root can be loaded. */
static void cfs_reset_root(struct cfs *fs)
{
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    fs->nr_bufs = fs->nr_dirty = 0;
    kfree(fs->bitmap);
    kfree(fs->bitmap_dirty);
    fs->bitmap = NULL;
    fs->bitmap_dirty = NULL;
    /*
     * And everything the abandoned attempt accumulated in the open
     * transaction. Nothing of it belongs to the root about to be tried:
     * the orphan replay can queue deferred frees and then fail the
     * mount, and carrying those into the fallback would have its first
     * commit hand the allocator blocks the older root's inodes still
     * name -- live data, not a leak. The block numbers are the older
     * root's to decide about, and it has not been read yet.
     */
    fs->nr_pending = 0;
    fs->nr_exempt = 0;
    fs->nr_orphans = 0;
    fs->first_dirty_ns = 0;
    fs->failed = 0;   /* the attempt is abandoned, not the filesystem */
    cfs_clones_free(fs);   /* read again from the root about to be loaded */
    /* The other root may describe a different set of members; the pool
     * keeps its devices, the table is read again. */
    cfs_members_free(fs);
}

/* Load the allocator and the root directory of the root in fs->sb. */
static int load_root(struct cfs *fs, struct vnode **root)
{
    fs->gen = fs->sb.generation + 1;
    fs->alloc_hint = 2;
    fs->load_stage = COSMO_FSCTL_STAGE_MEMBERS;
    /* The root being loaded is the last durable one: what the copies
     * missed is counted from it (design.md, "Degraded operation"). */
    pool_commit_done(fs->pool, fs->sb.generation);
    int rc = cfs_members_load(fs);
    if (rc)
        return rc;
    fs->copystate_seq = fs->pool->state_seq;
    /* The key, if this filesystem has one. A mount without it still
     * works for everything that is not a file's contents: the metadata
     * is plaintext by design, and every data read answers -ENOKEY
     * (design.md, "Boot-time unlock"). */
    fs->encrypted = fs->sb.version >= 7 && fs->sb.key_root != 0;
    if (fs->encrypted && fs->mac.on && !fs->have_key) {
        /* Version 14: the superblock verified under the key table's MAC
         * key, so the master key is there; a writable mount holds it until
         * its unmount (design.md, "The key handle"). The key block is read
         * as any metadata is, so it authenticates too. */
        rc = cfs_keytab_take(fs->sb.uuid, !fs->maint, fs->master_key);
        if (rc)
            return rc;
        fs->have_key = true;
        fs->key_held = !fs->maint;
        struct cfs_buf *kb;
        rc = cfs_buf_get(fs, fs->sb.key_root, CFS_KIND_KEYS, &kb);
        if (rc)
            return rc;
        const struct cfs_keys2 *k = (const struct cfs_keys2 *)(kb->data + CFS_MHDR_SIZE);
        bool ok = k->version == 2 && memcmp(k->uuid, fs->sb.uuid, 16) == 0;
        cfs_buf_put(fs, kb);
        if (!ok) {
            kerror("cosmofs: %s: the key block is not this pool's version-2 key block", fs->pool_name);
            return -EIO;
        }
    } else if (fs->encrypted && !fs->have_key) {
        char key[128];
        if (fwcfg_get_string("fskey", key, sizeof(key)) && key[0]) {
            rc = cfs_keys_load(fs, key, strlen(key));
            memset(key, 0, sizeof(key));
            if (rc == -EKEYREJECTED)
                kerror("cosmofs: the key in opt/cosmo/fskey does not unwrap this filesystem");
            else if (rc && rc != -EAGAIN)   /* -EAGAIN: unseeded pool, mount locked (S18) */
                return rc;
        }
        if (!fs->have_key)
            kwarn("cosmofs: encrypted and locked; metadata only until a key arrives");
    }
    /* From the blocks that exist, not the linear span: that is rounded
     * up to whole bitmap chunks per member and is mostly padding on a
     * small device. */
    uint64_t tot = fs->sb.total_blocks;
    fs->reserve = tot / 32 > 32 ? tot / 32 : 32;
    fs->load_stage = COSMO_FSCTL_STAGE_BITMAP;
    rc = load_bitmap(fs);
    if (rc)
        return rc;
    fs->load_stage = COSMO_FSCTL_STAGE_SNAPSHOTS;
    /* How many snapshots exist decides whether a commit may free the
     * blocks it releases (design.md, "Format version 3"). */
    unsigned n = 0;
    if (cfs_snapshot_list(fs, NULL, 0, &n) == 0)
        fs->snap_count = n;
    if (n)
        kinfo("cosmofs: %u snapshot(s)", n);
    rc = cfs_clones_load(fs);
    if (rc)
        return rc;
    /*
     * And what the last root still owed: inodes unlinked while something
     * held them, whose eviction the crash or the forced unmount threw
     * away. After the bitmap and after the snapshot count, because the
     * frees this does go through both (docs/audit/next-subsystem-orphan.md).
     */
    /*
     * A pending rollback (design.md, "Rollback"): before the orphan
     * replay, because a switch makes another tree live and the old one's
     * orphans are the safety snapshot's to keep. A switch that fails is
     * reported, and the mount abandons it and loads this root again
     * without trying (cosmofs_mount).
     */
    bool switched = false;
    if (fs->sb.version >= 12 && fs->sb.rollback_root != 0 && fs->rdonly)
        kinfo("cosmofs: read-only: a rollback record is in place; a writable mount acts on it");
    fs->load_stage = COSMO_FSCTL_STAGE_ROLLBACK;
    if (fs->sb.version >= 12 && fs->sb.rollback_root != 0 && !fs->rollback_off && !fs->rdonly) {
        rc = cfs_rollback_at_mount(fs, &switched);
        if (rc) {
            fs->rollback_failed = true;
            return rc;
        }
    }
    fs->rollback_switched = switched;
    fs->load_stage = COSMO_FSCTL_STAGE_ROOT;
    /*
     * The root must be a directory -- the one the switch, if any, made
     * live: the VFS takes that as given (do_mount
     * asserts it), so a root inode that says otherwise is refused here,
     * by name, rather than met as a panic. A disk with one changed mode
     * used to stop the kernel.
     */
    struct cfs_inode ri;
    if (cfs_inode_read(fs, CFS_ROOT_INO, &ri) == 0 && CFS_MODE_TYPE(ri.mode) != CFS_TYPE_DIR) {
        kerror("cosmofs: generation %llu: the root inode is not a directory (mode 0%o); refusing it",
               (unsigned long long)fs->sb.generation, ri.mode);
        return -ENOTDIR;
    }
    /* After a switch the record is the target's own orphans, which the
     * switch's commit wrote (cosmofs_rollback.cee). */
    fs->load_stage = COSMO_FSCTL_STAGE_ORPHAN_RECORD;
    rc = cfs_orphan_replay(fs);
    if (rc)
        return rc;
    if (fs->rdonly && (fs->replay_freed || fs->replay_orphans))
        kinfo("cosmofs: read-only: %u block(s) of the free record and %u orphan inode(s) released in memory "
              "only; a writable mount writes them",
              fs->replay_freed, fs->replay_orphans);
    return cfs_vnode_get(fs, CFS_ROOT_INO, root);
}

static int cosmofs_sync(struct mount *mnt);
static int cosmofs_sync_counted(struct mount *mnt, bool writeback);

/*
 * The writeback thread (design.md): commits when the open transaction
 * has grown past a threshold or aged past the interval. It takes the
 * mount's sync lock with a trylock so it never waits on an unmount or a
 * vfs_sync in progress, and never commits when the test hook has turned
 * autonomous commits off.
 */
static bool wb_due(struct cfs *fs)
{
    if (fs->failed || fs->first_dirty_ns == 0)
        return false;
    if (fs->nr_dirty >= CFS_WB_DIRTY_BUFS || fs->nr_pending >= CFS_WB_PENDING)
        return true;
    if (__atomic_load_n(&fs->mnt->cache_dirty, __ATOMIC_RELAXED) >= CFS_WB_DIRTY_PAGES)
        return true;
    return clock_since_ns(fs->first_dirty_ns) >= (uint64_t)fs->wb_interval_ms * 1000000ull;
}

static void cfs_writeback_thread(void *arg)
{
    struct cfs *fs = arg;
    struct mount *mnt = fs->mnt;
    while (!__atomic_load_n(&fs->wb_stop, __ATOMIC_ACQUIRE)) {
        thread_sleep_ms(CFS_WB_POLL_MS);
        if (!fs->wb_enabled || !wb_due(fs))
            continue;
        if (!mutex_trylock(&mnt->sync_lock))
            continue;   /* an unmount or vfs_sync is at it: they commit */
        if (!mnt->unmounted && !__atomic_load_n(&fs->wb_stop, __ATOMIC_ACQUIRE)) {
            int rc = cosmofs_sync_counted(mnt, true);
            if (rc)
                kwarn("cosmofs: writeback commit failed (%d)", rc);
        }
        mutex_unlock(&mnt->sync_lock);
    }
}

/*
 * Failed mounts' stages, by device, for the check on an unmounted device,
 * which asks right after its own mount failed while it holds the
 * device's claim. One entry per device and a few of them, so two checks
 * of different devices failing at once each find their own (Codex on
 * #434: one shared record lost the first). An entry is taken by the
 * question, so a stale one cannot answer a later mount.
 */
#define CFS_FAIL_NOTES 8u
static spinlock_t g_fail_lock = SPINLOCK_INIT("cosmofs-mount-fail");
static struct {
    const struct blkdev *bd;
    unsigned stage;
    int rc;
} g_fail[CFS_FAIL_NOTES];
static unsigned g_fail_next;

static void mount_failure_note(const struct blkdev *bd, unsigned stage, int rc)
{
    spin_lock(&g_fail_lock);
    unsigned slot = CFS_FAIL_NOTES;
    for (unsigned i = 0; i < CFS_FAIL_NOTES && slot == CFS_FAIL_NOTES; i++)
        if (g_fail[i].bd == bd)
            slot = i;   /* this device's: replaced */
    if (slot == CFS_FAIL_NOTES)
        slot = g_fail_next++ % CFS_FAIL_NOTES;
    g_fail[slot].bd = bd;
    g_fail[slot].stage = stage;
    g_fail[slot].rc = rc;
    spin_unlock(&g_fail_lock);
}

bool cfs_mount_failure(const struct blkdev *bd, unsigned *stage, int *rc)
{
    bool mine = false;
    spin_lock(&g_fail_lock);
    for (unsigned i = 0; i < CFS_FAIL_NOTES && !mine; i++) {
        if (g_fail[i].bd != bd)
            continue;
        *stage = g_fail[i].stage;
        *rc = g_fail[i].rc;
        g_fail[i].bd = NULL;
        mine = true;
    }
    spin_unlock(&g_fail_lock);
    return mine;
}

/*
 * The newest superblock slot of `bd` that is either a CRC'd one that
 * verifies or a version-14 encrypted one, *unverified* -- which without
 * its key is all it can be. For the key operations alone (design.md, "The
 * key handle"): KEY_INFO hands out the slots, which authenticate
 * themselves, and KEY_LOAD verifies what it found under the key it was
 * given before it keeps anything. -ENOENT when neither slot is either.
 */
int cfs_super_peek(struct spool *p, struct cfs_super *out)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = -ENOENT;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (pool_read(p, slot, block) != 0)
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)block;
        if (!cfs_super_peek_ok(block))
            continue;
        if (rc == 0 && sb->generation <= out->generation)
            continue;
        *out = *sb;
        rc = 0;
    }
    kfree(block);
    return rc;
}

int cfs_super_slots(struct blkdev *bd, struct cfs_super *a, int *ra, struct cfs_super *b, int *rb)
{
    struct cfs *fs = kzalloc(sizeof(*fs));
    if (fs == NULL)
        return -ENOMEM;
    int rc = pool_open(bd, &fs->pool);
    if (rc == 0) {
        *ra = super_read(fs, CFS_SUPER_A, a);
        *rb = super_read(fs, CFS_SUPER_B, b);
        pool_close(fs->pool);
    }
    memset(&fs->mac, 0, sizeof(fs->mac));   /* a MAC key super_read took from the key table */
    kfree(fs);
    return rc;
}

void cfs_bitmap_claim(struct cfs *fs, uint64_t lin)
{
    if (lin >= fs->nblocks || cfs_bit_test(fs->bitmap, lin))
        return;
    cfs_bit_set(fs->bitmap, lin);
    fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
    if (fs->free_blocks > 0)
        fs->free_blocks--;
    uint64_t dva = cfs_lin_dva(fs, lin);
    unsigned v = CFS_DVA_VDEV(dva);
    if (v < fs->nmembers && fs->mem[v].free_blocks > 0)
        fs->mem[v].free_blocks--;
    cfs_note_dirty(fs);
}

/* A copy changed state (the pool's notify, under fs->lock or during the
 * mount): the change is a commit due, and the mount listing says whether
 * the pool runs degraded (design.md, "Degraded operation"). */
static void cfs_pool_changed(void *arg)
{
    struct cfs *fs = arg;
    if (fs->mnt == NULL)
        return;
    if (pool_degraded(fs->pool))
        __atomic_fetch_or(&fs->mnt->state, MOUNT_STATE_DEGRADED, __ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&fs->mnt->state, ~MOUNT_STATE_DEGRADED, __ATOMIC_RELAXED);
    if (fs->mount_done && !fs->rdonly)
        cfs_note_dirty(fs);
}

static int cosmofs_mount(struct fs_type *fst, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fst;
    if (bdev == NULL)
        return -EINVAL;
    struct cfs *fs = kzalloc(sizeof(*fs));
    if (fs == NULL)
        return -ENOMEM;
    fs->rdonly = (flags & MOUNT_RDONLY) != 0;
    /* A maintenance mount (cosmofs_check_dev) commits only when its
     * release decides to: no writeback thread may commit a replay or a
     * refused repair before then (Codex on #434). */
    bool detached = (flags & MOUNT_DETACHED) != 0;
    list_init(&fs->bufs);
    mutex_init(&fs->lock, "cosmofs");
    fs->mnt = mnt;
    fs->wb_enabled = !detached;
    fs->maint = detached;
    fs->wb_interval_ms = CFS_WB_INTERVAL_MS;
    int rc = pool_open(bdev, &fs->pool);
    if (rc) {
        kfree(fs);
        return rc;
    }
    /* fs->nblocks and the reserve follow from the member table, which
     * the root's own generation names (load_root). */

    struct cfs_super a, b;
    int ra = super_read(fs, CFS_SUPER_A, &a);
    int rb = super_read(fs, CFS_SUPER_B, &b);
    if (ra && rb) {
        int why = ra == -ENOKEY || rb == -ENOKEY ? -ENOKEY : -EIO;
        if (why == -ENOKEY)
            kerror("cosmofs: %s: encrypted and locked: its key is not loaded", bdev->name);
        else
            kerror("cosmofs: %s: no valid superblock", bdev->name);
        mount_failure_note(bdev, COSMO_FSCTL_STAGE_SUPER, why);
        cfs_destroy(fs);
        return why;
    }
    /* The newest root of the pool, whichever of member 0's devices this
     * one is (CF27): a copy that was away holds an older root that every
     * checksum on it still passes. */
    const struct cfs_super *mine = (ra == 0 && (rb != 0 || a.generation >= b.generation)) ? &a : &b;
    uint64_t newer_gen = 0;
    struct blkdev *newer = cfs_newer_member0(bdev, mine->uuid, mine->generation, &newer_gen);
    if (newer != NULL) {
        if (vfs_bdev_mounted(newer)) {
            kerror("cosmofs: %s is behind (generation %llu) and %s, newer, is mounted; not mounting", bdev->name,
                   (unsigned long long)mine->generation, newer->name);
            blkdev_put(newer);
            cfs_destroy(fs);
            return -EBUSY;
        }
        kwarn("cosmofs: %s is behind (generation %llu); mounting from %s (generation %llu)", bdev->name,
              (unsigned long long)mine->generation, newer->name, (unsigned long long)newer_gen);
        pool_close(fs->pool);
        fs->pool = NULL;
        rc = pool_open(newer, &fs->pool);
        blkdev_put(newer);
        if (rc == 0) {
            ra = super_read(fs, CFS_SUPER_A, &a);
            rb = super_read(fs, CFS_SUPER_B, &b);
            if (ra && rb)
                rc = -EIO;
        }
        if (rc) {
            mount_failure_note(bdev, COSMO_FSCTL_STAGE_SUPER, rc);
            cfs_destroy(fs);
            return rc;
        }
    }
    /* The newer valid root first; if its tree does not load, the older
     * one (design.md, "Older-slot fallback at mount"). */
    unsigned first = (ra == 0 && (rb != 0 || a.generation >= b.generation)) ? CFS_SUPER_A : CFS_SUPER_B;
    unsigned other = first == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    int rother = first == CFS_SUPER_A ? rb : ra;
    fs->sb = first == CFS_SUPER_A ? a : b;
    fs->sb_slot = first;
    mnt->fs_priv = fs;
    /* The pool's console lines name it by its label, or by this device;
     * and every change of a copy's state is a commit due and, while any
     * copy is out, a mount listed as degraded. */
    memcpy(fs->pool_name, fs->sb.label, CFS_LABEL_MAX);
    fs->pool_name[CFS_LABEL_MAX] = '\0';
    if (fs->pool_name[0] == '\0')
        strlcpy(fs->pool_name, bdev->name, sizeof(fs->pool_name));
    fs->pool->name = fs->pool_name;
    fs->pool->notify = cfs_pool_changed;
    fs->pool->notify_arg = fs;
    struct vnode *root = NULL;
    rc = load_root(fs, &root);
    if (rc && fs->rollback_failed) {
        /* The rollback's switch failed: nothing of it was published, so
         * the same root is loaded again with the switch off -- and why is
         * kept, for whoever asks why the rollback is still pending
         * (CFS-20). */
        int why = rc;
        cfs_reset_root(fs);
        fs->rollback_error = why;
        fs->sb = fs->sb_slot == CFS_SUPER_A ? a : b;   /* the switch changed the in-memory root */
        fs->rollback_failed = false;
        fs->rollback_off = true;
        rc = load_root(fs, &root);
    }
    if (rc && rother == 0) {
        kwarn("cosmofs: %s: generation %llu does not load (%d); falling back to generation %llu", bdev->name,
              (unsigned long long)fs->sb.generation, rc, (unsigned long long)(other == CFS_SUPER_A ? a : b).generation);
        cfs_reset_root(fs);
        fs->sb = other == CFS_SUPER_A ? a : b;
        fs->sb_slot = other;
        fs->fell_back = true;
        rc = load_root(fs, &root);
    }
    if (rc) {
        mount_failure_note(bdev, fs->load_stage, rc);
        mnt->fs_priv = NULL;
        cfs_destroy(fs);
        return rc;
    }
    mnt->root = root;
    cfs_pool_changed(fs);
    cfs_holds_sync(fs);
    kinfo("cosmofs: %s: generation %llu, %llu/%llu blocks free (%llu reserved), %llu inodes", bdev->name,
          (unsigned long long)fs->sb.generation, (unsigned long long)fs->free_blocks,
          (unsigned long long)fs->sb.total_blocks, (unsigned long long)fs->reserve,
          (unsigned long long)fs->sb.inode_count);
    /* Live now, so the writeback thread may exist. If the replay left an
     * open transaction, this is where its committer starts. */
    fs->mount_done = true;
    if (fs->first_dirty_ns != 0)
        cfs_note_dirty(fs);
    /* Spares, the pool's other devices held (CFS-5), and a resilver for
     * every copy that wants one -- after the mount, so a boot does not
     * wait for it (design.md, "Import"). */
    cfs_resilver_mount(fs);
    return 0;
}

/*
 * A commit, and whether it counts as the writeback thread's.
 *
 * The count is taken inside the same hold of fs->lock that publishes the
 * generation, because cosmofs_stats reads the two together under that
 * lock and a reader that sees one without the other sees a filesystem
 * that never existed. It used to be taken after cosmofs_sync returned,
 * which is after the lock was dropped: a window of a few instructions in
 * which sb.generation had advanced and wb_commits had not, and
 * cosmofs-writeback asserts exactly that pair
 * (docs/audit/2026-09-deferred-work-inventory.md). The increment was
 * also an unlocked read-modify-write on a field read under the lock.
 */
static int cosmofs_sync_counted(struct mount *mnt, bool writeback)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    if (fs->discard_on_unmount)
        return 0;   /* test hook: behave as if the root write never happened */
    int rc = cfs_sync_vnodes(fs);
    if (rc)
        return rc;
    mutex_lock(&fs->lock);
    /*
     * Counted only when this call actually published a root, which
     * cfs_commit_ex reports rather than leaving to be inferred.
     *
     * `rc == 0` is not the question: the early return means "there was
     * nothing to commit", and the writeback thread reaches it that way
     * whenever a foreground file_sync -- which does not take
     * mnt->sync_lock -- commits between wb_due and this call. Comparing
     * the generation before and after is not the question either: a
     * commit that failed after publishing the number leaves it advanced
     * and fs->gen where it was, so the retry that finally succeeds
     * republishes the same number and would go uncounted.
     */
    bool published = false;
    rc = cfs_commit_ex(fs, &published);
    if (rc == 0 && writeback && published) {
        /* Published with the generation, not after it. */
        lockdep_assert_held(&fs->lock, LOCKDEP_KIND_MUTEX);
        fs->wb_commits++;
    }
    mutex_unlock(&fs->lock);
    return rc;
}

/* statfs: the pool's blocks; what a file may still take is what is free
 * beyond the metadata reserve (cfs_alloc_run's rule for data). Read
 * without the filesystem's lock: a moment's figures, as df wants. */
static int cosmofs_statfs(struct mount *mnt, struct vfs_statfs *out)
{
    struct cfs *fs = mnt->fs_priv;
    uint64_t total = __atomic_load_n(&fs->nblocks, __ATOMIC_RELAXED);
    uint64_t free = __atomic_load_n(&fs->free_blocks, __ATOMIC_RELAXED);
    uint64_t reserve = __atomic_load_n(&fs->reserve, __ATOMIC_RELAXED);
    out->bsize = CFS_BLOCK;
    out->blocks = total;
    out->bfree = free;
    out->bavail = free > reserve ? free - reserve : 0;
    return 0;
}

static int cosmofs_sync(struct mount *mnt)
{
    return cosmofs_sync_counted(mnt, false);
}

/* A commit of this mount now, as the writeback thread makes one (under
 * the mount's sync lock): what a pool command changed is on disk when
 * the command returns (cosmofs_poolctl). */
int cosmofs_commit_mount(struct mount *mnt)
{
    mutex_lock(&mnt->sync_lock);
    int rc = mnt->unmounted ? 0 : cosmofs_sync_counted(mnt, false);
    mutex_unlock(&mnt->sync_lock);
    return rc;
}

/* The VFS committed through cosmofs_sync before calling this; what is
 * left is either nothing, a deliberately discarded transaction (test
 * hook) or an abandoned one (cfs_fail), and both are dropped here so the
 * on-disk state stays at the last committed root. The writeback thread
 * is stopped first (it skips a mount whose sync lock is held, which it
 * is here, so the join is short). */
static int cosmofs_unmount(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    /* The resilver first: its cursor is in the record the VFS's last
     * sync wrote, and the next mount goes on from there. */
    cfs_resilver_stop(fs);
    if (fs->wb_thread) {
        __atomic_store_n(&fs->wb_stop, true, __ATOMIC_RELEASE);
        thread_join(fs->wb_thread);
        fs->wb_thread = NULL;
    }
    if (fs->discard_on_unmount)
        kwarn("cosmofs: discarding the open transaction (test hook)");
    else if (fs->failed)
        kwarn("cosmofs: dropping the abandoned transaction (%d)", fs->failed);
    mnt->fs_priv = NULL;   /* the root's eviction sees no filesystem */
    cfs_destroy(fs);
    return 0;
}

struct fs_type cosmofs_fs_type = {
    .name = "cosmofs",
    .magic = 0x4d534643u,   /* "CFSM", the metadata header's magic: Linux has no number for this one */
    .statfs = cosmofs_statfs,
    .mount = cosmofs_mount,
    .unmount = cosmofs_unmount,
    .sync = cosmofs_sync,
    /* The maintenance passes, reachable from /dev/fsctl. Until this the
     * only caller of either was a self-test. */
    .check = cosmofs_check,
    .scrub = cosmofs_scrub,
    .check_dev = cosmofs_check_dev,
    .snapctl = cosmofs_snapctl,
    .poolctl = cosmofs_poolctl,
    /* A single-device pool, unencrypted: what the installer makes
     * through /dev/blkctl (roadmap M2), named or not (S6). */
    .format = cosmofs_format_label,
    .format_mirror = cfs_format_mirror_devs,
    /* An encrypted pool's key, and making one (roadmap 1.2 B5). */
    .keyctl = cosmofs_keyctl,
    .format_keyed = cosmofs_format_keys,
};

/*
 * The newest format this kernel reads, as text in its image: cosmo-upgrade
 * looks for it in a release's kernel.elf before switching to that release,
 * and refuses one that could not mount the root (a kernel without the
 * marker reads 11 at most; design.md, "Format version 12"). Printed at
 * boot, which also keeps it in every build.
 */
_Static_assert(CFS_VERSION == 15, "update the format marker with CFS_VERSION");
static const char cfs_format_marker[] = "cosmofs-format-max=15";

void cosmofs_init(void)
{
    if (vfs_register_fs(&cosmofs_fs_type))
        panic("cosmofs: cannot register");
    kinfo("cosmofs: reads format versions 2 to 15 (%s)", cfs_format_marker);
}
