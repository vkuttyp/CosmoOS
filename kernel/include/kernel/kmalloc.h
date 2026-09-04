/*
 * kmalloc.h - Kernel heap: slab caches and general-purpose allocation.
 *
 * All functions are non-blocking and interrupt-safe (cache spinlock,
 * irqsave). Allocation failure returns NULL; misuse (double free, freeing
 * a non-heap pointer) panics. Results are at least 16-byte aligned.
 *
 * Ownership: the caller owns a returned object until it passes it to the
 * matching free. Objects are not zeroed unless KMEM_ZERO / kzalloc.
 */

#ifndef KERNEL_KMALLOC_H
#define KERNEL_KMALLOC_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define KMEM_ZERO (1u << 0)

#define KMALLOC_MIN_ALIGN 16u
#define KMALLOC_MAX_SLAB  8192u                 /* larger goes to whole pages */
#define KMALLOC_MAX_SIZE  ((size_t)4 << 20)     /* PMM order 10 */

struct kmem_cache {
    const char *name;
    size_t object_size;
    size_t align;
    size_t slot_size;          /* object_size rounded up to align */
    unsigned slab_order;       /* frames per slab = 1 << slab_order */
    unsigned objects_per_slab;
    size_t header_size;        /* struct slab + bitmap, rounded to align */
    struct list_node partial;  /* some free slots */
    struct list_node full;     /* no free slots */
    struct list_node empty;    /* all slots free, retained for reuse */
    unsigned nr_slabs;
    unsigned nr_empty;
    uint64_t nr_allocated;     /* live objects */
    uint64_t nr_allocs;        /* lifetime counters */
    uint64_t nr_frees;
    spinlock_t lock;
    struct list_node link;     /* global cache list */
};

/* Create a cache. `align` 0 means KMALLOC_MIN_ALIGN. Returns NULL if the
 * object cannot fit a slab (see design.md) or on allocation failure. */
struct kmem_cache *kmem_cache_create(const char *name, size_t object_size, size_t align);

/* Destroy a cache with no live objects. Panics if objects remain. */
void kmem_cache_destroy(struct kmem_cache *cache);

void *kmem_cache_alloc(struct kmem_cache *cache, unsigned flags);
void  kmem_cache_free(struct kmem_cache *cache, void *obj);

/* Frames a cache currently holds (diagnostics). */
size_t kmem_cache_pages(const struct kmem_cache *cache);

/* One-time setup of the kmalloc size classes. Requires pmm_init. */
void kmalloc_init(void);

void *kmalloc(size_t size, unsigned flags);
void *kzalloc(size_t size);

/* Resize. Copies min(kmalloc_size(ptr), new_size) bytes. With KMEM_ZERO
 * the bytes beyond the old *usable* size are zero; the heap does not
 * record requested sizes, so bytes between the old request and the old
 * usable size keep whatever the caller left there. NULL ptr behaves as
 * kmalloc; new_size 0 frees and returns NULL. On failure the original
 * block is untouched and NULL is returned. */
void *krealloc(void *ptr, size_t new_size, unsigned flags);
void  kfree(void *ptr);

/* Usable size of a live allocation. */
size_t kmalloc_size(const void *ptr);

struct kmalloc_stats {
    uint64_t live_objects;
    uint64_t slab_pages;
    uint64_t large_pages;
};

void kmalloc_get_stats(struct kmalloc_stats *out);
void kmalloc_dump(void);

#endif /* KERNEL_KMALLOC_H */
