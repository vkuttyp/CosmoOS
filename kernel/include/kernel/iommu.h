/*
 * iommu.h - DMA remapping: domains, per-device attach, the IOVA space
 * (docs/kernel/iommu/design.md). A device with a domain reaches only the
 * pages its driver mapped, at I/O virtual addresses the DMA API hands
 * back; a device without one takes the identity path as before.
 */

#ifndef KERNEL_IOMMU_H
#define KERNEL_IOMMU_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

struct device;
struct iommu_domain;
struct iommu_unit;

#define IOMMU_PROT_READ  (1u << 0)
#define IOMMU_PROT_WRITE (1u << 1)

/* The IOVA window every domain allocates from: below the 32-bit mask
 * every device has at least, above the first megabyte so 0 stays the
 * DMA API's failure value. */
#define IOMMU_IOVA_LO (1ull << 20)
#define IOMMU_IOVA_HI (1ull << 32)

/* A bitmap allocator of page-granular ranges over [lo, hi). */
struct iova_space {
    uint64_t lo, hi;
    uint32_t *bits;       /* one bit per page */
    unsigned npages, used, reserved;
};
int iova_init(struct iova_space *s, uint64_t lo, uint64_t hi);
void iova_fini(struct iova_space *s);
uint64_t iova_alloc(struct iova_space *s, size_t pages);   /* 0 when no run of `pages` is free */
void iova_free(struct iova_space *s, uint64_t iova, size_t pages);
/* Take a range out of the allocator for good (the part inside the window). */
void iova_reserve(struct iova_space *s, uint64_t base, size_t pages);

/* A bus-address range the unit needs kept out of the IOVA space: MSI
 * doorbells the device writes (identity-mapped so the write still
 * lands) or addresses the unit interprets itself (never mapped). */
struct iommu_range {
    uint64_t base;
    size_t len;
    bool identity;
};

/* What a unit's driver provides. map/unmap/lookup run with d->lock held
 * (any context); attach/detach/domain_init/fini in thread context. */
struct iommu_ops {
    const char *name;
    bool (*covers)(struct iommu_unit *u, uint32_t sid);
    /* The ranges every domain of this unit reserves (at most `max`); may be NULL. */
    unsigned (*reserved)(struct iommu_unit *u, struct iommu_range *out, unsigned max);
    int (*domain_init)(struct iommu_unit *u, struct iommu_domain *d);   /* d->id, d->root */
    void (*domain_fini)(struct iommu_unit *u, struct iommu_domain *d);
    int (*attach)(struct iommu_unit *u, struct iommu_domain *d, uint32_t sid);
    /* -EIO when the unit did not confirm the invalidation: the device may
     * still be translating, so the domain may not be torn down. */
    int (*detach)(struct iommu_unit *u, struct iommu_domain *d, uint32_t sid);
    int (*map)(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t pages, unsigned prot);
    /* Clears the entries and invalidates; -EIO when the invalidation was
     * not confirmed, and then nothing it covered may be reused. */
    int (*unmap)(struct iommu_domain *d, uint64_t iova, size_t pages);
    bool (*lookup)(struct iommu_domain *d, uint64_t iova, paddr_t *pa);
};

struct iommu_unit {
    const struct iommu_ops *ops;
    char name[24];
    void *priv;
    struct list_node link;
    unsigned nr_domains;
    uint64_t faults;
};

struct iommu_domain {
    struct iommu_unit *unit;
    unsigned id;              /* VT-d domain id / SMMU VMID */
    paddr_t root;             /* the top-level table */
    spinlock_t lock;          /* IRQ-safe: dma_map runs in completion handlers */
    struct iova_space iova;
    unsigned nr_devices;
    uint64_t maps, unmaps, pages_mapped;
    void *priv;
    struct list_node link;
};

/* Once, after pci_init and before the boot modules: probes the units. */
void iommu_init(void);
/* A unit driver's registration (from its probe). */
void iommu_register_unit(struct iommu_unit *u);
bool iommu_present(void);
struct iommu_unit *iommu_unit_first(void);

/* Give `dev` (requester id `sid`) a domain of its own on the unit that
 * covers it; 0 with dev->iommu set, 0 with dev->iommu NULL when no unit
 * covers the device, -ENOMEM, -ERANGE (an id the unit cannot table). */
int iommu_attach_device(struct device *dev, uint32_t sid);
/* After the driver's remove: the device may not DMA any more. */
void iommu_detach_device(struct device *dev);

/* Domains for tests and future users (a domain per device is what
 * iommu_attach_device makes). */
struct iommu_domain *iommu_domain_create(struct iommu_unit *u);
void iommu_domain_destroy(struct iommu_domain *d);
/* Page-granular; -EEXIST when any page of the range is mapped (nothing
 * changes), -ENOMEM, -EINVAL. Any context. */
int iommu_map(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t len, unsigned prot);
/* -EIO when the unit did not confirm the invalidation: the range is
 * unmapped in the tables but must be treated as still reachable. */
int iommu_unmap(struct iommu_domain *d, uint64_t iova, size_t len);
bool iommu_lookup(struct iommu_domain *d, uint64_t iova, paddr_t *pa);

/* The DMA layer's shape: allocate IOVA for [pa, pa + len) and map it;
 * returns iova + the page offset, or 0. The unmap takes the same pair and
 * returns 0, or -EIO when the unit did not confirm the invalidation: the
 * addresses are then retired for the life of the domain (counted in
 * iommu_stats.retired) and the caller must not reuse the memory either. */
uint64_t iommu_dma_map(struct iommu_domain *d, paddr_t pa, size_t len, unsigned prot);
int iommu_dma_unmap(struct iommu_domain *d, uint64_t dma, size_t len);

/* A fault the unit reported: counted, logged (bounded). Interrupt context. */
void iommu_note_fault(struct iommu_unit *u, uint32_t sid, uint64_t addr, unsigned reason, bool write);

struct iommu_stats {
    unsigned units, domains;
    uint64_t maps, unmaps, faults, iova_failures;
    uint64_t retired;   /* pages never handed out again: an unconfirmed invalidation */
};
void iommu_get_stats(struct iommu_stats *out);

/* The unit drivers (kernel/iommu/iommu.c calls the one for the architecture). */
void intel_vtd_init(void);
void arm_smmuv3_init(void);

#endif /* KERNEL_IOMMU_H */
