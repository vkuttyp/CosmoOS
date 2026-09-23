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
    /*
     * The file's mappings (struct vm_file_map, kernel/vmm.h), under
     * `lock`: what truncate walks to unmap before it frees, and what
     * write-back walks to lower a written page's PTEs to read-only.
     */
    struct list_node mappings;
    /*
     * The bound a fault installs below, together with vn->size. Both
     * filesystems trim the cache BEFORE they lower the size, so in that
     * window a fault reading the size alone would install a page past
     * the new end that nothing then unmaps. pagecache_truncate sets this
     * to the new size; a pagecache_write that grows the file lifts it
     * back to UINT64_MAX (docs/audit/next-subsystem-file-regions.md,
     * "The bound the cache owns").
     */
    uint64_t trim_bound;
    /* The last write-back failure and its sequence, recorded by
     * pagecache_sync under `lock` where the failure is seen (the one lock
     * every write-back passes through); read by pagecache_error_since.
     * Each open file remembers the sequence it has been told about and
     * is told once (docs/kernel-services/vfs/design.md, "Write-back
     * errors"). */
    int wb_err;
    uint32_t wb_seq;
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
/* Under pc->lock: whether a write-back failure was recorded after
 * sequence `seen`; its errno and the current sequence come back either
 * way. pagecache_wb_seq is the current sequence alone (an opener's
 * starting point). */
bool pagecache_error_since(struct pagecache *pc, uint32_t seen, int *err, uint32_t *now);
uint32_t pagecache_wb_seq(struct pagecache *pc);
/* Drop pages entirely past `size` and zero the tail of the last page. */
void pagecache_truncate(struct vnode *vn, uint64_t size);
/* Free every page; dirty pages are lost (caller synced or does not care). */
/* Drop every page. Returns how many were dirty; with `lost` they are
 * data lost (a named file's) and counted in pagecache_stats.dropped_dirty;
 * without (an unlinked file's, with no reader left) they are not. */
unsigned pagecache_drop(struct vnode *vn, bool lost);

/*
 * The cache half of a FILE fault (docs/audit/next-subsystem-file-regions.md,
 * "The fault, in two phases"). pagecache_lock takes the cache mutex
 * (after the reclaim every entry to a cache runs); pagecache_fault_page,
 * under it, returns the frame for `index` with one reference taken for
 * the caller's mapping (pmm_page_get), marking the entry dirty when
 * `dirty` -- a shared write. -EFBIG for an index at or past
 * min(vn->size, trim_bound): the caller ends the access with SIGBUS
 * rather than inheriting the zero page get() would make. The caller
 * installs the frame (or drops the reference) and then pagecache_unlock.
 * The install happens under the mutex so a truncate or a write-back on
 * the same file is serialised against it.
 */
/* Whether a program's text is mapped from this file: true while a
 * shared, executable mapping of it is on the cache's list. Call with
 * the cache lock held; a write to a busy file is -ETXTBSY
 * (docs/audit/next-subsystem-elf-shared-text.md). */
bool pagecache_text_busy(struct vnode *vn);

void pagecache_lock(struct vnode *vn);
void pagecache_unlock(struct vnode *vn);
int pagecache_fault_page(struct vnode *vn, uint64_t index, bool dirty, struct page **out);

/* Fill `buf` (4 KiB) with page `index` through the cache (used by
 * filesystems that keep directories in file data). */
int pagecache_get_page(struct vnode *vn, uint64_t index, void *buf);
int pagecache_put_page(struct vnode *vn, uint64_t index, const void *buf);   /* whole page, dirty */

struct pagecache_stats {
    uint64_t hits, misses, writebacks, pages;
    uint64_t reclaimed;        /* clean pages evicted by the global limit */
    uint64_t budget_refusals;  /* misses refused by a mount's page budget (-ENOSPC) */
    uint64_t wb_errors;        /* write-back failures recorded (pagecache_sync) */
    uint64_t dropped_dirty;    /* dirty pages dropped at a vnode's release: data lost */
    uint64_t pinned_skips;     /* reclaim candidates left alone because a mapping holds the frame */
    uint64_t exec_syncs;       /* writes into a page some mapping executes: the I-cache synced by the kernel alias */
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
