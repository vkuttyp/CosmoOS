/*
 * slab.h - Slab header, private to kernel/memory and the host tests.
 */

#ifndef KERNEL_MEMORY_SLAB_H
#define KERNEL_MEMORY_SLAB_H

#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/page.h>

struct slab {
    struct list_node link;      /* in cache->partial / full / empty */
    struct kmem_cache *cache;
    struct page *page;          /* first frame */
    void *objects;              /* first slot */
    unsigned free_count;
    unsigned next_hint;         /* bitmap word to scan first */
    uint64_t bitmap[];          /* 1 = free; objects_per_slab bits */
};

/* Initialise the cache-of-caches. Called once by kmalloc_init before any
 * kmem_cache_create. */
void slab_bootstrap(void);

/* Slab owning `obj`, or NULL if the frame is not a slab frame. */
struct slab *slab_of(const void *obj);

void slab_dump(void);
uint64_t slab_total_pages(void);

#endif /* KERNEL_MEMORY_SLAB_H */
