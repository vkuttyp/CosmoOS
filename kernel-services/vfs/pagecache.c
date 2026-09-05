/*
 * pagecache.c - Per-vnode page cache.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

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
        bool valid = false;
        s = spin_lock_irqsave(&g_lru_lock);
        struct pc_entry *cur;
        list_for_each_entry(cur, &g_lru, lru) {
            if (cur == e) {
                valid = e->vn == vn && !e->dirty;
                break;
            }
        }
        spin_unlock_irqrestore(&g_lru_lock, s);
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
    if (index * PAGE_SIZE < vn->size && vn->ops->readpage) {
        int rc = vn->ops->readpage(vn, index, page_to_virt(e->page));
        if (rc) {
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
        if (off + done > vn->size)
            vn->size = off + done;
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
        for (uint64_t idx = 0; idx < npages && left > 0 && rc == 0; idx++) {
            struct pc_entry *e = find(pc, idx);
            if (e == NULL || !e->dirty)
                continue;
            left--;
            if (vn->ops->writepage)
                rc = vn->ops->writepage(vn, e->index, page_to_virt(e->page));
            if (rc == 0) {
                e->dirty = false;
                pc->nr_dirty--;
                __atomic_fetch_sub(&vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
                stat_add(&g_stats.writebacks, 1);
                lru_add(e);   /* clean again: reclaimable */
            }
        }
    }
    mutex_unlock(&pc->lock);
    return rc;
}

void pagecache_truncate(struct vnode *vn, uint64_t size)
{
    struct pagecache *pc = &vn->pc;
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    mutex_lock(&pc->lock);
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

void pagecache_drop(struct vnode *vn)
{
    struct pagecache *pc = &vn->pc;
    mutex_lock(&pc->lock);
    for (unsigned b = 0; b < PC_HASH; b++) {
        struct pc_entry *e = pc->buckets[b];
        pc->buckets[b] = NULL;
        while (e) {
            struct pc_entry *next = e->next;
            lru_remove(e);
            if (e->dirty) {
                pc->nr_dirty--;
                __atomic_fetch_sub(&vn->mnt->cache_dirty, 1u, __ATOMIC_RELAXED);
            }
            pc->nr_pages--;
            __atomic_fetch_sub(&vn->mnt->cache_pages, 1u, __ATOMIC_RELAXED);
            stat_add(&g_stats.pages, -1);
            pmm_free_page(e->page);
            kfree(e);
            e = next;
        }
    }
    mutex_unlock(&pc->lock);
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
