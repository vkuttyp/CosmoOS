/*
 * dma.c - DMA allocation and mapping without an IOMMU.
 */

#include <kernel/device.h>
#include <kernel/dma.h>
#include <arch/cpu.h>
#include <kernel/errno.h>
#include <kernel/iommu.h>
#include <kernel/log.h>
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
    if (dev && dev->iommu) {
        /* The device sees an I/O virtual address (kernel/iommu). */
        uint64_t iova = iommu_dma_map(dev->iommu, pa, (size_t)PAGE_SIZE << order, IOMMU_PROT_READ | IOMMU_PROT_WRITE);
        if (iova == 0) {
            pmm_free_pages(page, order);
            return NULL;
        }
        *dma_out = (dma_addr_t)iova;
    } else {
        *dma_out = (dma_addr_t)pa;
    }

    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    g_stats.allocs++;
    g_stats.bytes_allocated += (uint64_t)PAGE_SIZE << order;
    spin_unlock_irqrestore(&g_stats_lock, s);
    return phys_to_virt(pa);
}

void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma)
{
    if (size == 0)
        return;
    unsigned order = order_for(size);
    bool revoked = true;
    if (dev && dev->iommu)
        revoked = iommu_dma_unmap(dev->iommu, dma, (size_t)PAGE_SIZE << order) == 0;
    struct page *page = phys_to_page(virt_to_phys(va));   /* dma may be an IOVA */
    KASSERT(page != NULL);
    if (revoked) {
        pmm_free_pages(page, order);
    } else {
        /* The unit still has the old translation: giving these frames to
         * another allocation would give the device a window into it. They
         * are lost for this boot, which is the cheap half of the trade. */
        kerror("dma: %s: %zu byte(s) leaked: the IOMMU did not revoke them", dev->name, size);
        g_stats.leaked += (uint64_t)PAGE_SIZE << order;
    }

    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    g_stats.frees++;
    g_stats.bytes_allocated -= (uint64_t)PAGE_SIZE << order;
    spin_unlock_irqrestore(&g_stats_lock, s);
}

/* The bus address of a direct-map range within the device's mask, else 0. */
static dma_addr_t translate(struct device *dev, const void *va, size_t len)
{
    uintptr_t v = (uintptr_t)va;
    if (len == 0 || !virt_is_direct_map(v) || !virt_is_direct_map(v + len - 1))
        return 0;
    dma_addr_t dma = (dma_addr_t)virt_to_phys((const void *)v);
    /* The direct map is linear, so contiguity is guaranteed. */
    if (dma + len - 1 > mask_of(dev))
        return 0;
    return dma;
}

bool dma_mappable(struct device *dev, const void *va, size_t len)
{
    return translate(dev, va, len) != 0;
}

dma_addr_t dma_map(struct device *dev, const void *va, size_t len, enum dma_dir dir)
{
    dma_addr_t dma = translate(dev, va, len);
    if (dma != 0 && dev && dev->iommu) {
        unsigned prot = IOMMU_PROT_READ | (dir == DMA_TO_DEVICE ? 0 : IOMMU_PROT_WRITE);
        dma = (dma_addr_t)iommu_dma_map(dev->iommu, (paddr_t)dma, len, prot);
    }
    bool ok = dma != 0;
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
    (void)dir;
    KASSERT(dma != 0);
    if (dev && dev->iommu && iommu_dma_unmap(dev->iommu, dma, len) != 0) {
        /* The buffer is the caller's: it goes back to kmalloc or to a
         * page cache the moment this returns, and the device still holds
         * a translation to it that the unit would not drop. There is
         * nothing left to withhold and no way to warn the next owner, so
         * this is where the kernel stops rather than let a device write
         * into memory that has been handed to something else. */
        panic("dma: %s: the IOMMU did not revoke %p; the device can still reach reused memory", dev->name,
              (void *)(uintptr_t)dma);
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    g_stats.unmaps++;   /* without a domain there is nothing to tear down; the pairing is the contract */
    spin_unlock_irqrestore(&g_stats_lock, s);
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
EXPORT_SYMBOL(dma_mappable);
EXPORT_SYMBOL(dma_sync_for_device);
EXPORT_SYMBOL(dma_sync_for_cpu);
EXPORT_SYMBOL(dma_set_mask);
