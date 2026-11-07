/*
 * pagecache_floor.c - The C under pagecache.cee (roadmap 2.0 SA7): the
 * global LRU, a static whose list head points into itself, and its lock;
 * the dirty bound's waitqueue and its lockdep completion class, whose
 * calls name their caller's return address; the injected readpage
 * failure and the cache lock's lockdep assertion, which a build without
 * fault injection or lockdep makes inline or nothing; a frame's address
 * through the direct map; and the values pagecache.cee writes as its own
 * constants, asserted. The page cache itself is CeeSafe (pagecache.cee,
 * generated into pagecache.c).
 */

#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/lockdep.h>
#include <kernel/list.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

_Static_assert(EIO == 5 && ENOMEM == 12 && EFBIG == 27 && ENOSPC == 28 && PAGE_SIZE == 4096 && PC_HASH == 32 &&
                   PAGECACHE_WRITE_RUN == 8 && MOUNT_CACHE_IS_STORE == 2 && PG_PAGECACHE == 128 &&
                   PMM_FLAGS_ZERO == 4 && VNODE_REG == 1 && sizeof(struct pagecache_stats) == 11 * 8,
               "pagecache.cee's errno.h, page.h, pagecache.h, pmm.h and vfs.h values");
_Static_assert(offsetof(struct pagecache, buckets) == 0 &&
                   sizeof(((struct pagecache *)0)->buckets[0]) == sizeof(size_t),
               "pagecache.cee's buckets: the cache's first field, an array of pointers");

/* What pagecache.cee calls here. */
struct list_node *pagecache_floor_lru(void);
spinlock_t *pagecache_floor_lru_lock(void);
struct waitqueue *pagecache_floor_dirty_wq(void);
void pagecache_floor_dirty_init(void);
void pagecache_floor_dirty_wait(void);
void pagecache_floor_dirty_signal(void);
bool pagecache_floor_readpage_fails(void);
void pagecache_floor_assert_held(struct mutex *m);
void *pagecache_floor_virt(const struct page *page);

/* The global LRU of clean, reclaimable pages: head = most recent. */
static LIST_HEAD(g_lru);
static spinlock_t g_lru_lock = SPINLOCK_INIT("pagecache-lru");
static struct waitqueue g_dirty_wq;
static uint16_t g_dirty_class;    /* the sleep's lockdep completion class */

struct list_node *pagecache_floor_lru(void)
{
    return &g_lru;
}

spinlock_t *pagecache_floor_lru_lock(void)
{
    return &g_lru_lock;
}

struct waitqueue *pagecache_floor_dirty_wq(void)
{
    return &g_dirty_wq;
}

void pagecache_floor_dirty_init(void)
{
    waitqueue_init(&g_dirty_wq, "pagecache-dirty");
    lockdep_completion_init(&g_dirty_class, "pagecache-dirty", (uintptr_t)__builtin_return_address(0));
}

void pagecache_floor_dirty_wait(void)
{
    lockdep_completion_wait(&g_dirty_wq, &g_dirty_class, "pagecache-dirty", (uintptr_t)__builtin_return_address(0));
}

void pagecache_floor_dirty_signal(void)
{
    lockdep_completion_signal(&g_dirty_wq, &g_dirty_class, "pagecache-dirty",
                              (uintptr_t)__builtin_return_address(0));
}

/* The injected failure stands in for readpage on ANY miss, a hole on
 * ramfs included, so the SIGBUS proof does not need a disk. */
bool pagecache_floor_readpage_fails(void)
{
    return faultinject_should_fail(FI_FILE_READPAGE);
}

void pagecache_floor_assert_held(struct mutex *m)
{
    (void)m;
    lockdep_assert_held(m, LOCKDEP_KIND_MUTEX);
}

void *pagecache_floor_virt(const struct page *page)
{
    return page_to_virt(page);
}
