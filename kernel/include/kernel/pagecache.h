/*
 * pagecache.h - Per-vnode page cache.
 *
 * Maps a regular file's 4 KiB page indices to page frames. Reads miss
 * through the filesystem's readpage; writes dirty pages that reach the
 * filesystem's writepage only on pagecache_sync. Frames are addressed
 * through the direct map. Locking: pagecache.lock, taken under the
 * vnode lock. See docs/kernel-services/vfs/design.md.
 */

#ifndef KERNEL_PAGECACHE_H
#define KERNEL_PAGECACHE_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/types.h>

#define PC_HASH 32

struct vnode;
struct page;

struct pc_entry {
    uint64_t index;
    struct page *page;
    bool dirty;
    bool on_lru;               /* clean and reclaimable: linked on the global LRU */
    struct pc_entry *next;
    struct vnode *vn;          /* owner (unreferenced; the entry dies with its cache) */
    struct list_node lru;      /* global LRU, under the LRU lock */
};

struct pagecache {
    struct pc_entry *buckets[PC_HASH];
    unsigned nr_pages;
    unsigned nr_dirty;
    struct mutex lock;
};

void pagecache_init(struct pagecache *pc);

/* Bounded by vn->size; returns bytes read or a negative errno. */
int64_t pagecache_read(struct vnode *vn, uint64_t off, void *buf, size_t len);
/* Grows vn->size; returns bytes written or a negative errno. */
int64_t pagecache_write(struct vnode *vn, uint64_t off, const void *buf, size_t len);
/* The most consecutive dirty pages offered to writepages at once. A
 * filesystem that compresses records wants its whole record; anything
 * larger is memory held across one call for no gain. */
#define PAGECACHE_WRITE_RUN 8u

/* writepage() every dirty page, or writepages() for a run of them. */
int pagecache_sync(struct vnode *vn);
/* Drop pages entirely past `size` and zero the tail of the last page. */
void pagecache_truncate(struct vnode *vn, uint64_t size);
/* Free every page; dirty pages are lost (caller synced or does not care). */
void pagecache_drop(struct vnode *vn);

/* Fill `buf` (4 KiB) with page `index` through the cache (used by
 * filesystems that keep directories in file data). */
int pagecache_get_page(struct vnode *vn, uint64_t index, void *buf);
int pagecache_put_page(struct vnode *vn, uint64_t index, const void *buf);   /* whole page, dirty */

struct pagecache_stats {
    uint64_t hits, misses, writebacks, pages;
    uint64_t reclaimed;        /* clean pages evicted by the global limit */
    uint64_t budget_refusals;  /* misses refused by a mount's page budget (-ENOSPC) */
};
void pagecache_get_stats(struct pagecache_stats *out);

/* The global cap on cached pages (docs/kernel/security/design.md §3): set
 * at boot to a quarter of the buddy's pages; 0 disables reclaim. Clean
 * pages of mounts without MOUNT_CACHE_IS_STORE are evicted from a global
 * LRU when a miss would exceed it. */
void pagecache_set_limit(uint64_t pages);
uint64_t pagecache_limit(void);
/* Evict up to `max` clean pages from the LRU tail; returns the number evicted. */
unsigned pagecache_reclaim(unsigned max);

#endif /* KERNEL_PAGECACHE_H */
