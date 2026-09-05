/*
 * dma.h - DMA memory for drivers (constitution section 26).
 *
 * A driver never assumes virtual == physical and never computes a bus
 * address itself: dma_alloc hands out coherent, physically contiguous
 * memory with its bus address, dma_map translates an existing
 * direct-map buffer, and the sync calls are the single place a
 * non-coherent port or an IOMMU would hook in. There is no IOMMU in
 * this phase, so a bus address equals a physical address, but callers
 * must not rely on that.
 */

#ifndef KERNEL_DMA_H
#define KERNEL_DMA_H

#include <kernel/types.h>

struct device;

typedef uint64_t dma_addr_t;

enum dma_dir {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL,
};

#define DMA_ZERO (1u << 0)

/* Coherent allocation, page granular, contiguous, inside dev->dma_mask
 * (NULL dev: 32-bit). Returns the kernel virtual address and the bus
 * address. Thread context (the PMM does not sleep, but the allocation is
 * not for interrupt handlers). NULL on failure. */
void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags);
void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma);

/* Translate an existing buffer for device access. Only direct-map
 * addresses (kmalloc, dma_alloc, page frames) qualify and the range
 * must be physically contiguous and within the mask; anything else
 * (kernel arena, stack, user) returns 0. Any context. */
dma_addr_t dma_map(struct device *dev, const void *va, size_t len, enum dma_dir dir);
void dma_unmap(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);
/* Would dma_map accept this range? A predicate with no side effect (the
 * block layer validates buffers with it; drivers map for real). */
bool dma_mappable(struct device *dev, const void *va, size_t len);

/* Ordering points around device access. Any context. */
void dma_sync_for_device(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);
void dma_sync_for_cpu(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);

/* 24..64 bits. -EINVAL otherwise. */
int dma_set_mask(struct device *dev, unsigned bits);

struct dma_stats {
    uint64_t allocs, frees, maps, unmaps, map_failures;
    uint64_t bytes_allocated;   /* currently outstanding */
};
void dma_get_stats(struct dma_stats *out);

#endif /* KERNEL_DMA_H */
