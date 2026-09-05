/*
 * slab.c - Fixed-size object caches on top of the buddy allocator.
 *
 * A slab is 2^order frames: an on-slab header (struct slab + a free
 * bitmap) followed by equally sized, aligned slots. Every frame of the
 * slab is flagged PG_SLAB with page->slab pointing at the header, so an
 * object pointer resolves to its slab and cache without a size argument.
 * The bitmap makes double frees detectable and keeps free slots
 * untouched (no freelist pointers inside freed memory).
 */

#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include "slab.h"

#define SLAB_MIN_OBJECTS 8u
#define SLAB_MAX_ORDER   3u
#define SLAB_KEEP_EMPTY  2u

static LIST_HEAD(g_caches);
static spinlock_t g_caches_lock = SPINLOCK_INIT("kmem_caches");

/* The cache of caches: struct kmem_cache objects come from here, which
 * needs a statically initialised bootstrap instance. */
static struct kmem_cache g_cache_cache;

static inline size_t align_up_size(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static size_t header_bytes(unsigned objects, size_t align)
{
    size_t words = (objects + 63) / 64;
    return align_up_size(sizeof(struct slab) + words * sizeof(uint64_t), align);
}

/* Choose slab order and objects per slab. Returns false if one object
 * does not fit even the largest slab. */
static bool layout(size_t slot_size, size_t align, unsigned *order_out, unsigned *objects_out,
                   size_t *header_out)
{
    for (unsigned order = 0; order <= SLAB_MAX_ORDER; order++) {
        size_t bytes = PAGE_SIZE << order;
        /* Iterate: header depends on object count, count on header. */
        unsigned objects = (unsigned)((bytes - header_bytes(1, align)) / slot_size);
        while (objects > 0 && header_bytes(objects, align) + (size_t)objects * slot_size > bytes)
            objects--;
        if (objects == 0)
            continue;
        if (objects >= SLAB_MIN_OBJECTS || order == SLAB_MAX_ORDER) {
            *order_out = order;
            *objects_out = objects;
            *header_out = header_bytes(objects, align);
            return true;
        }
    }
    return false;
}

static void cache_init(struct kmem_cache *cache, const char *name, size_t object_size, size_t align)
{
    memset(cache, 0, sizeof(*cache));
    cache->name = name;
    cache->object_size = object_size;
    cache->align = align;
    cache->slot_size = align_up_size(object_size, align);
    list_init(&cache->partial);
    list_init(&cache->full);
    list_init(&cache->empty);
    list_init(&cache->link);
    spinlock_init(&cache->lock, name);
}

static bool cache_setup(struct kmem_cache *cache, const char *name, size_t object_size, size_t align)
{
    if (align == 0)
        align = KMALLOC_MIN_ALIGN;
    if ((align & (align - 1)) != 0 || align < KMALLOC_MIN_ALIGN || object_size == 0)
        return false;

    cache_init(cache, name, object_size, align);
    return layout(cache->slot_size, align, &cache->slab_order, &cache->objects_per_slab,
                  &cache->header_size);
}

/* --- slab lifecycle (cache lock held) --- */

static struct slab *slab_grow(struct kmem_cache *cache)
{
    struct page *page = pmm_alloc_pages(cache->slab_order, 0);
    if (page == NULL)
        return NULL;

    unsigned frames = 1u << cache->slab_order;
    for (unsigned i = 0; i < frames; i++) {
        page[i].flags |= PG_SLAB;
        page[i].slab = NULL; /* set below once the header exists */
    }

    struct slab *slab = page_to_virt(page);
    memset(slab, 0, cache->header_size);
    list_init(&slab->link);
    slab->cache = cache;
    slab->page = page;
    slab->objects = (uint8_t *)slab + cache->header_size;
    slab->free_count = cache->objects_per_slab;
    slab->next_hint = 0;

    unsigned words = (cache->objects_per_slab + 63) / 64;
    for (unsigned w = 0; w < words; w++)
        slab->bitmap[w] = ~0ULL;
    unsigned tail = cache->objects_per_slab % 64;
    if (tail)
        slab->bitmap[words - 1] = (1ULL << tail) - 1;

    for (unsigned i = 0; i < frames; i++)
        page[i].slab = slab;

    cache->nr_slabs++;
    return slab;
}

static void slab_release(struct kmem_cache *cache, struct slab *slab)
{
    KASSERT(slab->free_count == cache->objects_per_slab);
    struct page *page = slab->page;
    unsigned frames = 1u << cache->slab_order;
    for (unsigned i = 0; i < frames; i++) {
        page[i].flags &= ~PG_SLAB;
        page[i].slab = NULL;
    }
    cache->nr_slabs--;
    pmm_free_pages(page, cache->slab_order);
}

static int slab_take_slot(struct kmem_cache *cache, struct slab *slab)
{
    unsigned words = (cache->objects_per_slab + 63) / 64;
    for (unsigned n = 0; n < words; n++) {
        unsigned w = (slab->next_hint + n) % words;
        if (slab->bitmap[w] == 0)
            continue;
        unsigned bit = (unsigned)__builtin_ctzll(slab->bitmap[w]);
        slab->bitmap[w] &= ~(1ULL << bit);
        slab->free_count--;
        slab->next_hint = w;
        return (int)(w * 64 + bit);
    }
    return -1;
}

/* --- public --- */

struct kmem_cache *kmem_cache_create(const char *name, size_t object_size, size_t align)
{
    struct kmem_cache *cache = kmem_cache_alloc(&g_cache_cache, KMEM_ZERO);
    if (cache == NULL)
        return NULL;
    if (!cache_setup(cache, name, object_size, align)) {
        kmem_cache_free(&g_cache_cache, cache);
        return NULL;
    }

    arch_irq_state_t s = spin_lock_irqsave(&g_caches_lock);
    list_push_back(&g_caches, &cache->link);
    spin_unlock_irqrestore(&g_caches_lock, s);
    return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
    arch_irq_state_t s = spin_lock_irqsave(&cache->lock);
    if (cache->nr_allocated != 0)
        panic("kmem_cache_destroy('%s'): %llu objects still live", cache->name,
              (unsigned long long)cache->nr_allocated);
    KASSERT(list_empty(&cache->partial) && list_empty(&cache->full));

    struct slab *slab, *tmp;
    list_for_each_entry_safe(slab, tmp, &cache->empty, link) {
        list_remove(&slab->link);
        cache->nr_empty--;
        slab_release(cache, slab);
    }
    spin_unlock_irqrestore(&cache->lock, s);

    s = spin_lock_irqsave(&g_caches_lock);
    list_remove(&cache->link);
    spin_unlock_irqrestore(&g_caches_lock, s);

    kmem_cache_free(&g_cache_cache, cache);
}

void *kmem_cache_alloc(struct kmem_cache *cache, unsigned flags)
{
    if (faultinject_should_fail(FI_KMALLOC))
        return NULL;   /* debug builds: an injected allocation failure (docs/verification/) */
    arch_irq_state_t s = spin_lock_irqsave(&cache->lock);

    struct slab *slab;
    if (!list_empty(&cache->partial)) {
        slab = list_first_entry(&cache->partial, struct slab, link);
    } else if (!list_empty(&cache->empty)) {
        slab = list_first_entry(&cache->empty, struct slab, link);
        list_remove(&slab->link);
        cache->nr_empty--;
        list_push_front(&cache->partial, &slab->link);
    } else {
        slab = slab_grow(cache);
        if (slab == NULL) {
            spin_unlock_irqrestore(&cache->lock, s);
            return NULL;
        }
        list_push_front(&cache->partial, &slab->link);
    }

    int idx = slab_take_slot(cache, slab);
    KASSERT(idx >= 0);
    if (slab->free_count == 0) {
        list_remove(&slab->link);
        list_push_front(&cache->full, &slab->link);
    }
    cache->nr_allocated++;
    cache->nr_allocs++;

    spin_unlock_irqrestore(&cache->lock, s);

    void *obj = (uint8_t *)slab->objects + (size_t)idx * cache->slot_size;
    if (flags & KMEM_ZERO)
        memset(obj, 0, cache->object_size);
    return obj;
}

struct slab *slab_of(const void *obj)
{
    struct page *page = virt_to_page(obj);
    if (page == NULL || (page->flags & PG_SLAB) == 0)
        return NULL;
    return page->slab;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
    struct slab *slab = slab_of(obj);
    if (slab == NULL || slab->cache != cache)
        panic("kmem_cache_free('%s'): %p does not belong to this cache", cache->name, obj);

    size_t off = (size_t)((uint8_t *)obj - (uint8_t *)slab->objects);
    if (off % cache->slot_size != 0 || off / cache->slot_size >= cache->objects_per_slab)
        panic("kmem_cache_free('%s'): %p is not an object start", cache->name, obj);
    unsigned idx = (unsigned)(off / cache->slot_size);

    arch_irq_state_t s = spin_lock_irqsave(&cache->lock);

    uint64_t bit = 1ULL << (idx % 64);
    if (slab->bitmap[idx / 64] & bit)
        panic("kmem_cache_free('%s'): double free of %p", cache->name, obj);
    slab->bitmap[idx / 64] |= bit;
    slab->free_count++;
    cache->nr_allocated--;
    cache->nr_frees++;

    if (slab->free_count == 1) {
        /* was full */
        list_remove(&slab->link);
        list_push_front(&cache->partial, &slab->link);
    }
    if (slab->free_count == cache->objects_per_slab) {
        list_remove(&slab->link);
        if (cache->nr_empty < SLAB_KEEP_EMPTY) {
            list_push_front(&cache->empty, &slab->link);
            cache->nr_empty++;
            slab = NULL;
        }
    } else {
        slab = NULL;
    }

    spin_unlock_irqrestore(&cache->lock, s);

    if (slab != NULL)
        slab_release(cache, slab);
}

size_t kmem_cache_pages(const struct kmem_cache *cache)
{
    return (size_t)cache->nr_slabs << cache->slab_order;
}

void slab_bootstrap(void)
{
    bool ok = cache_setup(&g_cache_cache, "kmem_cache", sizeof(struct kmem_cache), 0);
    KASSERT(ok);
    list_push_back(&g_caches, &g_cache_cache.link);
}

void slab_dump(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_caches_lock);
    struct kmem_cache *c;
    kprintf("%-14s %6s %6s %5s %6s %8s %8s\n", "cache", "objsz", "slots", "order", "slabs", "live", "pages");
    list_for_each_entry(c, &g_caches, link) {
        kprintf("%-14s %6zu %6u %5u %6u %8llu %8zu\n", c->name, c->object_size, c->objects_per_slab,
                c->slab_order, c->nr_slabs, (unsigned long long)c->nr_allocated, kmem_cache_pages(c));
    }
    spin_unlock_irqrestore(&g_caches_lock, s);
}

uint64_t slab_total_pages(void)
{
    uint64_t total = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_caches_lock);
    struct kmem_cache *c;
    list_for_each_entry(c, &g_caches, link)
        total += kmem_cache_pages(c);
    spin_unlock_irqrestore(&g_caches_lock, s);
    return total;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(kmem_cache_create);
EXPORT_SYMBOL(kmem_cache_destroy);
EXPORT_SYMBOL(kmem_cache_alloc);
EXPORT_SYMBOL(kmem_cache_free);
