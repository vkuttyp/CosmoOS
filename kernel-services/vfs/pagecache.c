/*
 * pagecache.c - Per-vnode page cache.
 */

#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/lockdep.h>
#include <kernel/panic.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

static struct pagecache_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("pagecache-stats");

static void stat_add(uint64_t *field, int64_t delta);

/* The global LRU of clean, reclaimable pages: head = most recent. */
static LIST_HEAD(g_lru);
static spinlock_t g_lru_lock = SPINLOCK_INIT("pagecache-lru");
static uint64_t g_limit_pages;   /* 0: no limit */

static bool entry_reclaimable(const struct pc_entry *e)
{
    return !(e->vn->mnt->flags & MOUNT_CACHE_IS_STORE);
}

/* Both take the LRU lock; the caller holds the owning cache's mutex. */
static void lru_add(struct pc_entry *e)
{
    if (!entry_reclaimable(e) || e->on_lru)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lru_lock);
    list_push_front(&g_lru, &e->lru);
    e->on_lru = true;
    spin_unlock_irqrestore(&g_lru_lock, s);
}

static void lru_remove(struct pc_entry *e)
{
    if (!e->on_lru)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lru_lock);
    if (e->on_lru) {
        list_remove(&e->lru);
        e->on_lru = false;
    }
    spin_unlock_irqrestore(&g_lru_lock, s);
}

void pagecache_set_limit(uint64_t pages)
{
    __atomic_store_n(&g_limit_pages, pages, __ATOMIC_RELAXED);
}

uint64_t pagecache_limit(void)
{
    return __atomic_load_n(&g_limit_pages, __ATOMIC_RELAXED);
}

static uint64_t cached_pages(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    uint64_t n = g_stats.pages;
    spin_unlock_irqrestore(&g_stats_lock, s);
    return n;
}

static void remove_entry(struct pagecache *pc, struct pc_entry *e);

/*
 * Evict from the LRU tail. Runs with no cache mutex held: the victim's
 * vnode is pinned with a tryget (one already on its way out is skipped)
 * and its cache locked with a trylock (a busy one is skipped), so no
 * thread ever waits on a cache lock while holding another of the same
 * class. Under the victim's lock the entry is re-checked: still clean,
 * still on the LRU, still this vnode's.
 */
unsigned pagecache_reclaim(unsigned max)
{
    unsigned done = 0, skipped = 0;
    while (done < max && skipped < 16) {
        arch_irq_state_t s = spin_lock_irqsave(&g_lru_lock);
        if (list_empty(&g_lru)) {
            spin_unlock_irqrestore(&g_lru_lock, s);
            break;
        }
        struct pc_entry *e = list_entry(g_lru.prev, struct pc_entry, lru);
        struct vnode *vn = e->vn;
        if (!kobject_tryget(&vn->obj)) {
            /* Being released: its pagecache_drop will take the entry off. */
            list_remove(&e->lru);
            list_push_front(&g_lru, &e->lru);
            spin_unlock_irqrestore(&g_lru_lock, s);
            skipped++;
            continue;
        }
        spin_unlock_irqrestore(&g_lru_lock, s);

        if (!mutex_trylock(&vn->pc.lock)) {
            /* Busy: move it to the head so the next candidate differs. */
            s = spin_lock_irqsave(&g_lru_lock);
            if (e->on_lru && e->vn == vn) {
                list_remove(&e->lru);
                list_push_front(&g_lru, &e->lru);
            }
            spin_unlock_irqrestore(&g_lru_lock, s);
            vnode_put(vn);
            skipped++;
            continue;
        }
        /* The entry may have been dirtied, freed or reused since. */
        bool valid = false, pinned = false;
        s = spin_lock_irqsave(&g_lru_lock);
        struct pc_entry *cur;
        list_for_each_entry(cur, &g_lru, lru) {
            if (cur == e) {
                valid = e->vn == vn && !e->dirty;
                /* A mapping holds the frame (a reference per PTE, taken
                 * under the mutex this holds): left alone, and moved to
                 * the head so the next candidate differs. */
                if (valid && __atomic_load_n(&e->page->refcount, __ATOMIC_ACQUIRE) != 1) {
                    valid = false;
                    pinned = true;
                    list_remove(&e->lru);
                    list_push_front(&g_lru, &e->lru);
                }
                break;
            }
        }
        spin_unlock_irqrestore(&g_lru_lock, s);
        if (pinned) {
            stat_add(&g_stats.pinned_skips, 1);
            skipped++;
        }
        if (valid) {
            remove_entry(&vn->pc, e);
            stat_add(&g_stats.reclaimed, 1);
            done++;
        }
        mutex_unlock(&vn->pc.lock);
        vnode_put(vn);
    }
    return done;
}

/* Called before a cache lock is taken: keep the total under the limit. */
static void reclaim_if_needed(void)
{
    uint64_t limit = pagecache_limit();
    if (limit == 0)
        return;
    for (unsigned rounds = 0; rounds < 4 && cached_pages() >= limit; rounds++)
        if (pagecache_reclaim(32) == 0)
            break;   /* nothing clean to evict: a soft cap until writeback exists */
}

static void stat_add(uint64_t *field, int64_t delta)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *field = (uint64_t)((int64_t)*field + delta);
    spin_unlock_irqrestore(&g_stats_lock, s);
}

void pagecache_init(struct pagecache *pc)
{
    memset(pc, 0, sizeof(*pc));
    mutex_init(&pc->lock, "pagecache");
    list_init(&pc->mappings);
    pc->trim_bound = UINT64_MAX;
}

static struct pc_entry *find(struct pagecache *pc, uint64_t index)
{
    for (struct pc_entry *e = pc->buckets[index % PC_HASH]; e; e = e->next) {
        if (e->index == index)
            return e;
    }
    return NULL;
}

/* Lock held. Returns the entry for `index`, reading it in on a miss
 * (readpage for pages inside the file, zeros beyond). */
static struct pc_entry *get(struct vnode *vn, uint64_t index, int *err)
{
    struct pagecache *pc = &vn->pc;
    struct pc_entry *e = find(pc, index);
    if (e) {
        stat_add(&g_stats.hits, 1);
        return e;
    }
    stat_add(&g_stats.misses, 1);
    /* Reserve the mount's page first: the increment is the admission, so
     * concurrent misses on different vnodes of one mount cannot both pass
     * a stale read of the count. Every failure below gives it back. */
    struct mount *mnt = vn->mnt;
    for (;;) {
        uint64_t cur = __atomic_load_n(&mnt->cache_pages, __ATOMIC_RELAXED);
        if (mnt->cache_limit_pages && cur >= mnt->cache_limit_pages) {
            stat_add(&g_stats.budget_refusals, 1);
            *err = -ENOSPC;   /* the mount's page budget (ramfs) */
            return NULL;
        }
        /* The compare-and-swap is the admission: the count never exceeds
         * the budget, not even transiently. */
        if (__atomic_compare_exchange_n(&mnt->cache_pages, &cur, cur + 1, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            break;
    }
    e = kzalloc(sizeof(*e));
    if (e == NULL) {
        __atomic_fetch_sub(&mnt->cache_pages, 1u, __ATOMIC_RELAXED);
        *err = -ENOMEM;
        return NULL;
    }
    e->vn = vn;
    list_init(&e->lru);
    e->page = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (e->page == NULL) {
        kfree(e);
        __atomic_fetch_sub(&mnt->cache_pages, 1u, __ATOMIC_RELAXED);
        *err = -ENOMEM;
        return NULL;
    }
    e->index = index;
    e->page->flags |= PG_PAGECACHE;   /* the frame is the cache's: a mapping references, never owns, it */
    if (index * PAGE_SIZE < vn->size && vn->ops->readpage) {
        int rc = faultinject_should_fail(FI_FILE_READPAGE) ? -EIO
                                                             : vn->ops->readpage(vn, index, page_to_virt(e->page));
        if (rc) {
            e->page->flags &= ~PG_PAGECACHE;
            pmm_free_page(e->page);
            kfree(e);
            __atomic_fetch_sub(&mnt->cache_pages, 1u, __ATOMIC_RELAXED);
            *err = rc;
            return NULL;
        }
    }
    e->next = pc->buckets[index % PC_HASH];
    pc->buckets[index % PC_HASH] = e;
    pc->nr_pages++;
    stat_add(&g_stats.pages, 1);
    lru_add(e);   /* clean until written */
    return e;
}

static void remove_entry(struct pagecache *pc, struct pc_entry *e)
{
    struct pc_entry **pp = &pc->buckets[e->index % PC_HASH];
    while (*pp && *pp != e)
        pp = &(*pp)->next;
    if (*pp)
        *pp = e->next;
    lru_remove(e);
    if (e->dirty) {
        pc->nr_dirty--;
        __atomic_fetch_sub(&e->vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
    }
    pc->nr_pages--;
    __atomic_fetch_sub(&e->vn->mnt->cache_pages, 1u, __ATOMIC_RELAXED);
    stat_add(&g_stats.pages, -1);
    /* The cache's reference is the last: truncate unmapped first, reclaim
     * skipped a mapped frame, and a released vnode has no mappings.
     * pmm_free_page panics on any other count, which is the check. */
    e->page->flags &= ~PG_PAGECACHE;
    pmm_free_page(e->page);
    kfree(e);
}

/* A write dirties the page: off the LRU until pagecache_sync cleans it. */
static void mark_dirty(struct pagecache *pc, struct pc_entry *e)
{
    if (!e->dirty) {
        e->dirty = true;
        pc->nr_dirty++;
        __atomic_fetch_add(&e->vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
        lru_remove(e);
    }
}

int64_t pagecache_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    if (off >= vn->size)
        return 0;
    if (len > vn->size - off)
        len = (size_t)(vn->size - off);
    uint8_t *out = buf;
    size_t done = 0;
    reclaim_if_needed();
    mutex_lock(&vn->pc.lock);
    while (done < len) {
        uint64_t index = (off + done) / PAGE_SIZE;
        size_t in_page = (size_t)((off + done) % PAGE_SIZE);
        size_t n = PAGE_SIZE - in_page;
        if (n > len - done)
            n = len - done;
        int err = 0;
        struct pc_entry *e = get(vn, index, &err);
        if (e == NULL) {
            mutex_unlock(&vn->pc.lock);
            return done ? (int64_t)done : err;
        }
        memcpy(out + done, (uint8_t *)page_to_virt(e->page) + in_page, n);
        done += n;
    }
    mutex_unlock(&vn->pc.lock);
    return (int64_t)done;
}

int64_t pagecache_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    if (len == 0)
        return 0;
    if (off + len < off)
        return -EFBIG;
    const uint8_t *in = buf;
    size_t done = 0;
    int err = 0;
    reclaim_if_needed();
    mutex_lock(&vn->pc.lock);
    while (done < len) {
        uint64_t index = (off + done) / PAGE_SIZE;
        size_t in_page = (size_t)((off + done) % PAGE_SIZE);
        size_t n = PAGE_SIZE - in_page;
        if (n > len - done)
            n = len - done;
        struct pc_entry *e = get(vn, index, &err);
        if (e == NULL)
            break;
        memcpy((uint8_t *)page_to_virt(e->page) + in_page, in + done, n);
        mark_dirty(&vn->pc, e);
        done += n;
        if (off + done > vn->size) {
            vn->size = off + done;
            vn->pc.trim_bound = UINT64_MAX;   /* the file grew: the size governs again */
        }
    }
    mutex_unlock(&vn->pc.lock);
    return done ? (int64_t)done : err;
}

int pagecache_sync(struct vnode *vn)
{
    struct pagecache *pc = &vn->pc;
    int rc = 0;
    mutex_lock(&pc->lock);
    if (pc->nr_dirty) {
        /* In ascending page order, so a filesystem that allocates at a
         * moving hint lays a sequentially written file out contiguously
         * (hash-bucket order scattered the blocks and hit cosmofs's
         * extent cap on files of a few hundred pages). Dirty pages lie
         * below the size: writes grow it and truncate drops what is above. */
        uint64_t npages = (vn->size + PAGE_SIZE - 1) / PAGE_SIZE;
        unsigned left = pc->nr_dirty;
        struct pc_entry *run[PAGECACHE_WRITE_RUN];
        void *bufs[PAGECACHE_WRITE_RUN];
        for (uint64_t idx = 0; idx < npages && left > 0 && rc == 0; idx++) {
            struct pc_entry *e = find(pc, idx);
            if (e == NULL || !e->dirty)
                continue;

            /* Gather the consecutive dirty pages that follow, so a
             * filesystem with writepages can write them as one object.
             * Every page of the run is dirty and below the size, which
             * is what the filesystem is promised. */
            unsigned n = 0;
            run[n] = e;
            bufs[n] = page_to_virt(e->page);
            n++;
            if (vn->ops->writepages) {
                for (uint64_t k = idx + 1; k < npages && n < PAGECACHE_WRITE_RUN; k++) {
                    struct pc_entry *next = find(pc, k);
                    if (next == NULL || !next->dirty)
                        break;
                    run[n] = next;
                    bufs[n] = page_to_virt(next->page);
                    n++;
                }
            }

            /* Before the bytes are read for the disk, every writable PTE
             * of these pages is lowered (and shot down), so a write that
             * lands after this faults, waits for this mutex, and dirties
             * the page again; one that landed before is in what is
             * written. The other order -- write, then lower -- would
             * mark clean a page written between the two. */
            struct vm_file_map *m;
            list_for_each_entry(m, &pc->mappings, link)
                vm_file_map_writeprotect(m, e->index, n);

            unsigned done = 0;
            if (n > 1 && vn->ops->writepages) {
                rc = vn->ops->writepages(vn, e->index, bufs, n, &done);
                if (rc == 0 && (done == 0 || done > n))
                    rc = -EIO;   /* a filesystem that reports nonsense */
            } else if (vn->ops->writepage) {
                rc = vn->ops->writepage(vn, e->index, bufs[0]);
                done = 1;
            } else {
                done = 1;
            }
            if (rc == 0) {
                for (unsigned i = 0; i < done; i++) {
                    run[i]->dirty = false;
                    pc->nr_dirty--;
                    __atomic_fetch_sub(&vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
                    stat_add(&g_stats.writebacks, 1);
                    lru_add(run[i]);   /* clean again: reclaimable */
                }
                left -= done < left ? done : left;
                idx += done - 1;
            }
        }
        if (rc != 0) {
            /* Recorded where it is seen, under the lock every write-back
             * passes through; the failed pages stay dirty for the next
             * attempt. Each open file is told once (file_sync, file_flush). */
            pc->wb_err = rc;
            pc->wb_seq++;
            stat_add(&g_stats.wb_errors, 1);
        }
    }
    mutex_unlock(&pc->lock);
    return rc;
}

bool pagecache_error_since(struct pagecache *pc, uint32_t seen, int *err, uint32_t *now)
{
    mutex_lock(&pc->lock);
    *err = pc->wb_err;
    *now = pc->wb_seq;
    bool newer = pc->wb_seq != seen;
    mutex_unlock(&pc->lock);
    return newer;
}

uint32_t pagecache_wb_seq(struct pagecache *pc)
{
    mutex_lock(&pc->lock);
    uint32_t seq = pc->wb_seq;
    mutex_unlock(&pc->lock);
    return seq;
}

void pagecache_truncate(struct vnode *vn, uint64_t size)
{
    struct pagecache *pc = &vn->pc;
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    mutex_lock(&pc->lock);
    /* The bound first, then the mappings, then the frames: a fault that
     * takes this mutex next sees the new end, and no PTE names a frame
     * the loop below frees. */
    pc->trim_bound = size;
    struct vm_file_map *m;
    list_for_each_entry(m, &pc->mappings, link)
        vm_file_map_truncate(m, keep);
    for (unsigned b = 0; b < PC_HASH; b++) {
        struct pc_entry *e = pc->buckets[b];
        while (e) {
            struct pc_entry *next = e->next;
            if (e->index >= keep)
                remove_entry(pc, e);
            e = next;
        }
    }
    if (size % PAGE_SIZE) {
        struct pc_entry *e = find(pc, size / PAGE_SIZE);
        if (e)
            memset((uint8_t *)page_to_virt(e->page) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    }
    mutex_unlock(&pc->lock);
}

unsigned pagecache_drop(struct vnode *vn, bool count_lost)
{
    struct pagecache *pc = &vn->pc;
    unsigned lost = 0;
    mutex_lock(&pc->lock);
    /* A mapping holds a reference to the vnode, so a vnode being dropped
     * has none; a forced unmount reaches here only through the same
     * release. The frames below are therefore at reference 1. */
    KASSERT(list_empty(&pc->mappings));
    for (unsigned b = 0; b < PC_HASH; b++) {
        struct pc_entry *e = pc->buckets[b];
        pc->buckets[b] = NULL;
        while (e) {
            struct pc_entry *next = e->next;
            lru_remove(e);
            if (e->dirty) {
                pc->nr_dirty--;
                __atomic_fetch_sub(&vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
                lost++;
            }
            pc->nr_pages--;
            __atomic_fetch_sub(&vn->mnt->cache_pages, 1u, __ATOMIC_RELAXED);
            stat_add(&g_stats.pages, -1);
            e->page->flags &= ~PG_PAGECACHE;
            pmm_free_page(e->page);
            kfree(e);
            e = next;
        }
    }
    if (lost && count_lost)
        stat_add(&g_stats.dropped_dirty, (int64_t)lost);
    mutex_unlock(&pc->lock);
    return count_lost ? lost : 0;
}

void pagecache_lock(struct vnode *vn)
{
    reclaim_if_needed();
    mutex_lock(&vn->pc.lock);
}

void pagecache_unlock(struct vnode *vn)
{
    mutex_unlock(&vn->pc.lock);
}

int pagecache_fault_page(struct vnode *vn, uint64_t index, bool dirty, struct page **out)
{
    struct pagecache *pc = &vn->pc;
    lockdep_assert_held(&pc->lock, LOCKDEP_KIND_MUTEX);
    /* The end of the file as a mapping sees it: the size, and the trim
     * bound for the window between a trim and the size drop that follows
     * it. Past it get() would make a zero page, which is right for a
     * write() and wrong for a mapping (POSIX: SIGBUS). */
    uint64_t size = __atomic_load_n(&vn->size, __ATOMIC_RELAXED);
    uint64_t end = size < pc->trim_bound ? size : pc->trim_bound;
    if (index >= (end + PAGE_SIZE - 1) / PAGE_SIZE)
        return -EFBIG;
    int err = 0;
    struct pc_entry *e = get(vn, index, &err);
    if (e == NULL)
        return err;
    if (dirty)
        mark_dirty(pc, e);
    pmm_page_get(e->page);   /* the mapping's reference, taken under the mutex reclaim decides under */
    *out = e->page;
    return 0;
}

int pagecache_get_page(struct vnode *vn, uint64_t index, void *buf)
{
    reclaim_if_needed();
    mutex_lock(&vn->pc.lock);
    int err = 0;
    struct pc_entry *e = get(vn, index, &err);
    if (e)
        memcpy(buf, page_to_virt(e->page), PAGE_SIZE);
    mutex_unlock(&vn->pc.lock);
    return e ? 0 : err;
}

int pagecache_put_page(struct vnode *vn, uint64_t index, const void *buf)
{
    reclaim_if_needed();
    mutex_lock(&vn->pc.lock);
    int err = 0;
    struct pc_entry *e = get(vn, index, &err);
    if (e) {
        memcpy(page_to_virt(e->page), buf, PAGE_SIZE);
        mark_dirty(&vn->pc, e);
    }
    mutex_unlock(&vn->pc.lock);
    return e ? 0 : err;
}

void pagecache_get_stats(struct pagecache_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, s);
}
