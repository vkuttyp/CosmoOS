/*
 * pagecache.c - Per-vnode page cache.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

static struct pagecache_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("pagecache-stats");

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
    e = kzalloc(sizeof(*e));
    if (e == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    e->page = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (e->page == NULL) {
        kfree(e);
        *err = -ENOMEM;
        return NULL;
    }
    e->index = index;
    if (index * PAGE_SIZE < vn->size && vn->ops->readpage) {
        int rc = vn->ops->readpage(vn, index, page_to_virt(e->page));
        if (rc) {
            pmm_free_page(e->page);
            kfree(e);
            *err = rc;
            return NULL;
        }
    }
    e->next = pc->buckets[index % PC_HASH];
    pc->buckets[index % PC_HASH] = e;
    pc->nr_pages++;
    stat_add(&g_stats.pages, 1);
    return e;
}

static void remove_entry(struct pagecache *pc, struct pc_entry *e)
{
    struct pc_entry **pp = &pc->buckets[e->index % PC_HASH];
    while (*pp && *pp != e)
        pp = &(*pp)->next;
    if (*pp)
        *pp = e->next;
    if (e->dirty)
        pc->nr_dirty--;
    pc->nr_pages--;
    stat_add(&g_stats.pages, -1);
    pmm_free_page(e->page);
    kfree(e);
}

int64_t pagecache_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    if (off >= vn->size)
        return 0;
    if (len > vn->size - off)
        len = (size_t)(vn->size - off);
    uint8_t *out = buf;
    size_t done = 0;
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
        if (!e->dirty) {
            e->dirty = true;
            vn->pc.nr_dirty++;
        }
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
        for (unsigned b = 0; b < PC_HASH && rc == 0; b++) {
            for (struct pc_entry *e = pc->buckets[b]; e && rc == 0; e = e->next) {
                if (!e->dirty)
                    continue;
                if (vn->ops->writepage)
                    rc = vn->ops->writepage(vn, e->index, page_to_virt(e->page));
                if (rc == 0) {
                    e->dirty = false;
                    pc->nr_dirty--;
                    stat_add(&g_stats.writebacks, 1);
                }
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
            if (e->dirty)
                pc->nr_dirty--;
            pc->nr_pages--;
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
    mutex_lock(&vn->pc.lock);
    int err = 0;
    struct pc_entry *e = get(vn, index, &err);
    if (e) {
        memcpy(page_to_virt(e->page), buf, PAGE_SIZE);
        if (!e->dirty) {
            e->dirty = true;
            vn->pc.nr_dirty++;
        }
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
