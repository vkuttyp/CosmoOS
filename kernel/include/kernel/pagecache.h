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

#include <kernel/mutex.h>
#include <kernel/types.h>

#define PC_HASH 32

struct vnode;
struct page;

struct pc_entry {
    uint64_t index;
    struct page *page;
    bool dirty;
    struct pc_entry *next;
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
/* writepage() every dirty page. */
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
};
void pagecache_get_stats(struct pagecache_stats *out);

#endif /* KERNEL_PAGECACHE_H */
