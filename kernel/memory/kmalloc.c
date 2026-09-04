/*
 * kmalloc.c - General-purpose kernel allocation over size-class slabs.
 *
 * Requests up to KMALLOC_MAX_SLAB go to the smallest size class that fits.
 * Larger requests get whole frames from the buddy, flagged
 * PG_KMALLOC_LARGE with the order in page->order, so kfree can tell the
 * two paths apart from the page descriptor alone.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include "slab.h"

static const size_t size_classes[] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048, 4096, 8192,
};
#define SIZE_CLASS_COUNT ARRAY_SIZE(size_classes)

static struct kmem_cache *g_classes[SIZE_CLASS_COUNT];
static const char *const class_names[SIZE_CLASS_COUNT] = {
    "kmalloc-16", "kmalloc-32", "kmalloc-48", "kmalloc-64", "kmalloc-96", "kmalloc-128",
    "kmalloc-192", "kmalloc-256", "kmalloc-384", "kmalloc-512", "kmalloc-768", "kmalloc-1024",
    "kmalloc-2048", "kmalloc-4096", "kmalloc-8192",
};

static uint64_t g_large_pages;
static spinlock_t g_large_lock = SPINLOCK_INIT("kmalloc_large");
static bool g_initialized;

STATIC_ASSERT(sizeof(size_classes) / sizeof(size_classes[0]) == 15, "size class table");

void kmalloc_init(void)
{
    KASSERT(!g_initialized);
    slab_bootstrap();
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        g_classes[i] = kmem_cache_create(class_names[i], size_classes[i], KMALLOC_MIN_ALIGN);
        if (g_classes[i] == NULL)
            panic("kmalloc: cannot create %s", class_names[i]);
    }
    g_initialized = true;
    kinfo("kmalloc: %zu size classes up to %u bytes, page path up to %llu KiB", SIZE_CLASS_COUNT,
          KMALLOC_MAX_SLAB, (unsigned long long)(KMALLOC_MAX_SIZE >> 10));
}

static struct kmem_cache *class_for(size_t size)
{
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++) {
        if (size <= size_classes[i])
            return g_classes[i];
    }
    return NULL;
}

static unsigned order_for(size_t size)
{
    unsigned order = 0;
    while ((PAGE_SIZE << order) < size)
        order++;
    return order;
}

void *kmalloc(size_t size, unsigned flags)
{
    KASSERT(g_initialized);
    if (size == 0 || size > KMALLOC_MAX_SIZE)
        return NULL;

    if (size <= KMALLOC_MAX_SLAB)
        return kmem_cache_alloc(class_for(size), flags);

    unsigned order = order_for(size);
    struct page *page = pmm_alloc_pages(order, (flags & KMEM_ZERO) ? PMM_FLAGS_ZERO : 0);
    if (page == NULL)
        return NULL;
    page->flags |= PG_KMALLOC_LARGE;

    arch_irq_state_t s = spin_lock_irqsave(&g_large_lock);
    g_large_pages += (uint64_t)1 << order;
    spin_unlock_irqrestore(&g_large_lock, s);

    return page_to_virt(page);
}

void *kzalloc(size_t size)
{
    return kmalloc(size, KMEM_ZERO);
}

size_t kmalloc_size(const void *ptr)
{
    struct page *page = virt_to_page(ptr);
    if (page == NULL)
        return 0;
    if (page->flags & PG_KMALLOC_LARGE)
        return PAGE_SIZE << page->order;
    struct slab *slab = slab_of(ptr);
    return slab ? slab->cache->object_size : 0;
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    struct page *page = virt_to_page(ptr);
    if (page == NULL)
        panic("kfree: %p is not heap memory", ptr);

    if (page->flags & PG_KMALLOC_LARGE) {
        if ((void *)page_to_virt(page) != ptr)
            panic("kfree: %p is not the start of a large allocation", ptr);
        unsigned order = page->order;
        page->flags &= ~PG_KMALLOC_LARGE;

        arch_irq_state_t s = spin_lock_irqsave(&g_large_lock);
        g_large_pages -= (uint64_t)1 << order;
        spin_unlock_irqrestore(&g_large_lock, s);

        pmm_free_pages(page, order);
        return;
    }

    struct slab *slab = slab_of(ptr);
    if (slab == NULL)
        panic("kfree: %p is not heap memory", ptr);
    kmem_cache_free(slab->cache, ptr);
}

void *krealloc(void *ptr, size_t new_size, unsigned flags)
{
    if (ptr == NULL)
        return kmalloc(new_size, flags);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    size_t old_size = kmalloc_size(ptr);
    if (old_size == 0)
        panic("krealloc: %p is not heap memory", ptr);
    if (new_size <= old_size && new_size > old_size / 2)
        return ptr; /* same class; nothing to gain by moving */

    void *np = kmalloc(new_size, flags);
    if (np == NULL)
        return NULL;
    memcpy(np, ptr, old_size < new_size ? old_size : new_size);
    if ((flags & KMEM_ZERO) && new_size > old_size)
        memset((uint8_t *)np + old_size, 0, new_size - old_size);
    kfree(ptr);
    return np;
}

void kmalloc_get_stats(struct kmalloc_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < SIZE_CLASS_COUNT; i++)
        out->live_objects += g_classes[i]->nr_allocated;
    out->slab_pages = slab_total_pages();
    arch_irq_state_t s = spin_lock_irqsave(&g_large_lock);
    out->large_pages = g_large_pages;
    spin_unlock_irqrestore(&g_large_lock, s);
}

void kmalloc_dump(void)
{
    slab_dump();
    kprintf("large allocations: %llu pages\n", (unsigned long long)g_large_pages);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kzalloc);
EXPORT_SYMBOL(krealloc);
EXPORT_SYMBOL(kfree);
