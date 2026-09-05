/*
 * dma.c - DMA allocation and mapping without an IOMMU.
 */

#include <kernel/device.h>
#include <kernel/dma.h>
#include <arch/cpu.h>
#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>

static struct dma_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("dma-stats");

static uint64_t mask_of(const struct device *dev)
{
    return dev ? dev->dma_mask : 0xFFFFFFFFULL;
}

static unsigned zone_flags(uint64_t mask)
{
    if (mask < 0xFFFFFFFFULL)
        return PMM_FLAGS_ZONE_DMA;      /* below 16 MiB */
    if (mask == 0xFFFFFFFFULL)
        return PMM_FLAGS_ZONE_DMA32;
    return PMM_FLAGS_ZONE_NORMAL;
}

static unsigned order_for(size_t size)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned order = 0;
    while (((size_t)1 << order) < pages)
        order++;
    return order;
}

void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags)
{
    if (size == 0 || dma_out == NULL)
        return NULL;
    uint64_t mask = mask_of(dev);
    unsigned order = order_for(size);
    unsigned pflags = zone_flags(mask) | ((flags & DMA_ZERO) ? PMM_FLAGS_ZERO : 0);
    struct page *page = pmm_alloc_pages(order, pflags);
    if (page == NULL)
        return NULL;
    paddr_t pa = page_to_phys(page);
    if (pa + ((paddr_t)PAGE_SIZE << order) - 1 > mask) {
        pmm_free_pages(page, order);
        return NULL;
    }
    *dma_out = (dma_addr_t)pa;

    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    g_stats.allocs++;
    g_stats.bytes_allocated += (uint64_t)PAGE_SIZE << order;
    spin_unlock_irqrestore(&g_stats_lock, s);
    return phys_to_virt(pa);
}

void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma)
{
    (void)dev;
    (void)va;
    if (size == 0)
        return;
    unsigned order = order_for(size);
    struct page *page = phys_to_page((paddr_t)dma);
    KASSERT(page != NULL);
    pmm_free_pages(page, order);

    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    g_stats.frees++;
    g_stats.bytes_allocated -= (uint64_t)PAGE_SIZE << order;
    spin_unlock_irqrestore(&g_stats_lock, s);
}

dma_addr_t dma_map(struct device *dev, const void *va, size_t len, enum dma_dir dir)
{
    (void)dir;
    uintptr_t v = (uintptr_t)va;
    bool ok = len > 0 && virt_is_direct_map(v) && virt_is_direct_map(v + len - 1);
    dma_addr_t dma = 0;
    if (ok) {
        dma = (dma_addr_t)virt_to_phys((const void *)v);
        /* The direct map is linear, so contiguity is guaranteed. */
        if (dma + len - 1 > mask_of(dev))
            ok = false;
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    if (ok)
        g_stats.maps++;
    else
        g_stats.map_failures++;
    spin_unlock_irqrestore(&g_stats_lock, s);
    return ok ? dma : 0;
}

void dma_unmap(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir)
{
    (void)dev;
    (void)dma;
    (void)len;
    (void)dir;
}

void dma_sync_for_device(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir)
{
    (void)dev;
    (void)dma;
    (void)len;
    (void)dir;
    /* Both targets are cache coherent for DMA on the supported machines
     * (docs/kernel/device/); order the CPU's stores before the doorbell
     * write that follows. */
    arch_dma_barrier();
}

void dma_sync_for_cpu(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir)
{
    (void)dev;
    (void)dma;
    (void)len;
    (void)dir;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

int dma_set_mask(struct device *dev, unsigned bits)
{
    if (dev == NULL || bits < 24 || bits > 64)
        return -EINVAL;
    dev->dma_mask = bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1);
    return 0;
}

void dma_get_stats(struct dma_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, s);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(dma_alloc);
EXPORT_SYMBOL(dma_free);
EXPORT_SYMBOL(dma_map);
EXPORT_SYMBOL(dma_unmap);
EXPORT_SYMBOL(dma_sync_for_device);
EXPORT_SYMBOL(dma_sync_for_cpu);
EXPORT_SYMBOL(dma_set_mask);
