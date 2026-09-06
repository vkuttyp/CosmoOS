/*
 * iommu.c - Domains, per-device attach, the IOVA allocator, fault
 * accounting (docs/kernel/iommu/design.md §2–§4). The hardware is behind
 * struct iommu_ops (drivers/iommu/); the DMA API (kernel/device/dma.c)
 * is the only caller of the mapping functions in normal operation.
 */

#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/iommu.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

static LIST_HEAD(g_units);
static LIST_HEAD(g_domains);
static spinlock_t g_lock = SPINLOCK_INIT("iommu");
static struct iommu_stats g_stats;

/* --- the IOVA allocator ---------------------------------------------------- */

int iova_init(struct iova_space *s, uint64_t lo, uint64_t hi)
{
    if (lo >= hi || (lo & (PAGE_SIZE - 1)) || (hi & (PAGE_SIZE - 1)))
        return -EINVAL;
    s->lo = lo;
    s->hi = hi;
    s->npages = (unsigned)((hi - lo) / PAGE_SIZE);
    s->used = 0;
    s->reserved = 0;
    size_t words = (s->npages + 31) / 32;
    s->bits = kzalloc(words * sizeof(uint32_t));
    return s->bits ? 0 : -ENOMEM;
}

void iova_fini(struct iova_space *s)
{
    kfree(s->bits);
    s->bits = NULL;
}

static bool bit_set(const struct iova_space *s, unsigned i) { return (s->bits[i / 32] >> (i % 32)) & 1u; }
static void bit_put(struct iova_space *s, unsigned i, bool v)
{
    if (v)
        s->bits[i / 32] |= 1u << (i % 32);
    else
        s->bits[i / 32] &= ~(1u << (i % 32));
}

/* First fit from the bottom: a run of `pages` clear bits. Lowest-first
 * (not a rolling cursor) keeps a steady workload inside the same leaf
 * tables, so the tree stops growing once the device's working set has
 * been seen; unmapping invalidates the IOTLB, so reuse is safe at once. */
uint64_t iova_alloc(struct iova_space *s, size_t pages)
{
    if (pages == 0 || pages > s->npages - s->used)
        return 0;
    unsigned i = 0, run = 0, run_start = 0;
    while (i < s->npages) {
        if (bit_set(s, i)) {
            run = 0;
            i++;
            continue;
        }
        if (run == 0)
            run_start = i;
        run++;
        i++;
        if (run == pages) {
            for (unsigned k = run_start; k < run_start + pages; k++)
                bit_put(s, k, true);
            s->used += (unsigned)pages;
            return s->lo + (uint64_t)run_start * PAGE_SIZE;
        }
    }
    return 0;
}

void iova_reserve(struct iova_space *s, uint64_t base, size_t pages)
{
    for (size_t k = 0; k < pages; k++) {
        uint64_t a = base + (uint64_t)k * PAGE_SIZE;
        if (a < s->lo || a >= s->hi)
            continue;
        unsigned i = (unsigned)((a - s->lo) / PAGE_SIZE);
        if (!bit_set(s, i)) {
            bit_put(s, i, true);
            s->used++;
            s->reserved++;
        }
    }
}

void iova_free(struct iova_space *s, uint64_t iova, size_t pages)
{
    if (iova < s->lo || iova >= s->hi)
        return;
    unsigned first = (unsigned)((iova - s->lo) / PAGE_SIZE);
    unsigned freed = 0;
    for (unsigned k = 0; k < pages && first + k < s->npages; k++) {
        KASSERT(bit_set(s, first + k));
        bit_put(s, first + k, false);
        freed++;
    }
    s->used -= freed;   /* a range clipped by the window frees only what it took */
}

/* --- units ------------------------------------------------------------------ */

void iommu_register_unit(struct iommu_unit *u)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    list_init(&u->link);
    list_push_back(&g_units, &u->link);
    g_stats.units++;
    spin_unlock_irqrestore(&g_lock, s);
}

bool iommu_present(void)
{
    return !list_empty(&g_units);
}

struct iommu_unit *iommu_unit_first(void)
{
    return list_empty(&g_units) ? NULL : list_entry(g_units.next, struct iommu_unit, link);
}

static struct iommu_unit *unit_for(uint32_t sid)
{
    struct iommu_unit *u;
    list_for_each_entry(u, &g_units, link)
        if (u->ops->covers(u, sid))
            return u;
    return NULL;
}

void iommu_init(void)
{
#if defined(ARCH_X86_64)
    intel_vtd_init();
#elif defined(ARCH_AARCH64)
    arm_smmuv3_init();
#endif
    if (!iommu_present())
        kinfo("iommu: none (devices use physical addresses)");
}

/* --- domains ------------------------------------------------------------------ */

struct iommu_domain *iommu_domain_create(struct iommu_unit *u)
{
    if (u == NULL)
        return NULL;
    struct iommu_domain *d = kzalloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    d->unit = u;
    spinlock_init(&d->lock, "iommu-domain");
    list_init(&d->link);
    if (iova_init(&d->iova, IOMMU_IOVA_LO, IOMMU_IOVA_HI)) {
        kfree(d);
        return NULL;
    }
    if (u->ops->domain_init(u, d)) {
        iova_fini(&d->iova);
        kfree(d);
        return NULL;
    }
    /* The unit's reserved ranges: out of the allocator, identity-mapped
     * where the device must still reach them (MSI doorbells). */
    if (u->ops->reserved) {
        struct iommu_range r[8];
        unsigned n = u->ops->reserved(u, r, 8);
        arch_irq_state_t ds = spin_lock_irqsave(&d->lock);   /* map runs under it by contract */
        for (unsigned i = 0; i < n; i++) {
            size_t pages = (r[i].len + PAGE_SIZE - 1) / PAGE_SIZE;
            iova_reserve(&d->iova, r[i].base, pages);
            if (r[i].identity && u->ops->map(d, r[i].base, (paddr_t)r[i].base, pages, IOMMU_PROT_READ | IOMMU_PROT_WRITE))
                kwarn("iommu: %s: cannot identity-map %p for domain %u", u->name, (void *)(uintptr_t)r[i].base, d->id);
        }
        spin_unlock_irqrestore(&d->lock, ds);
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    list_push_back(&g_domains, &d->link);
    g_stats.domains++;
    u->nr_domains++;
    spin_unlock_irqrestore(&g_lock, s);
    return d;
}

void iommu_domain_destroy(struct iommu_domain *d)
{
    if (d == NULL)
        return;
    KASSERT(d->nr_devices == 0);
    if (d->iova.used != d->iova.reserved)
        kwarn("iommu: domain %u destroyed with %u pages still mapped", d->id, d->iova.used - d->iova.reserved);
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    list_remove(&d->link);
    g_stats.domains--;
    d->unit->nr_domains--;
    spin_unlock_irqrestore(&g_lock, s);
    d->unit->ops->domain_fini(d->unit, d);
    iova_fini(&d->iova);
    kfree(d);
}

int iommu_attach_device(struct device *dev, uint32_t sid)
{
    if (dev->iommu)
        return 0;   /* already attached */
    struct iommu_unit *u = unit_for(sid);
    if (u == NULL)
        return 0;   /* no unit covers it: the identity path */
    struct iommu_domain *d = iommu_domain_create(u);
    if (d == NULL)
        return -ENOMEM;
    int rc = u->ops->attach(u, d, sid);
    if (rc) {
        iommu_domain_destroy(d);
        kerror("iommu: %s: cannot attach %s (requester %04x): %d", u->name, dev->name, sid, rc);
        return rc;
    }
    d->nr_devices = 1;
    dev->iommu = d;
    dev->iommu_sid = sid;
    kinfo("iommu: %s: %s (requester %04x) in domain %u", u->name, dev->name, sid, d->id);
    return 0;
}

void iommu_detach_device(struct device *dev)
{
    struct iommu_domain *d = dev->iommu;
    if (d == NULL)
        return;
    int rc = d->unit->ops->detach(d->unit, d, dev->iommu_sid);
    dev->iommu = NULL;
    d->nr_devices = 0;
    if (d->iova.used != d->iova.reserved)
        kwarn("iommu: %s left %u pages mapped at detach", dev->name, d->iova.used - d->iova.reserved);
    if (rc) {
        /* The unit did not confirm that it stopped translating for this
         * requester: its tables and its domain id stay allocated, because
         * freeing them would hand a live translation someone else's
         * memory. The domain is off the device and off no list it needs. */
        kerror("iommu: %s: detach not confirmed; domain %u kept", dev->name, d->id);
        return;
    }
    iommu_domain_destroy(d);
}

/* --- mapping ------------------------------------------------------------------ */

static size_t pages_of(uint64_t start, size_t len)
{
    uint64_t first = start & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (start + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return (size_t)((end - first) / PAGE_SIZE);
}

int iommu_map(struct iommu_domain *d, uint64_t iova, paddr_t pa, size_t len, unsigned prot)
{
    if (len == 0 || (iova & (PAGE_SIZE - 1)) || (pa & (PAGE_SIZE - 1)))
        return -EINVAL;
    size_t pages = pages_of(0, len);
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    int rc = d->unit->ops->map(d, iova, pa, pages, prot);
    if (rc == 0) {
        d->maps++;
        d->pages_mapped += pages;
    }
    spin_unlock_irqrestore(&d->lock, s);
    return rc;
}

int iommu_unmap(struct iommu_domain *d, uint64_t iova, size_t len)
{
    if (len == 0 || (iova & (PAGE_SIZE - 1)))
        return -EINVAL;
    size_t pages = pages_of(0, len);
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    int rc = d->unit->ops->unmap(d, iova, pages);
    d->unmaps++;
    d->pages_mapped -= pages < d->pages_mapped ? pages : d->pages_mapped;   /* unmapping a hole is allowed */
    spin_unlock_irqrestore(&d->lock, s);
    return rc;
}

bool iommu_lookup(struct iommu_domain *d, uint64_t iova, paddr_t *pa)
{
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    bool ok = d->unit->ops->lookup(d, iova, pa);
    spin_unlock_irqrestore(&d->lock, s);
    return ok;
}

uint64_t iommu_dma_map(struct iommu_domain *d, paddr_t pa, size_t len, unsigned prot)
{
    if (len == 0)
        return 0;
    uint64_t off = pa & (PAGE_SIZE - 1);
    size_t pages = pages_of(pa, len);
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    uint64_t iova = iova_alloc(&d->iova, pages);
    if (iova == 0) {
        spin_unlock_irqrestore(&d->lock, s);
        __atomic_fetch_add(&g_stats.iova_failures, 1, __ATOMIC_RELAXED);
        return 0;
    }
    int rc = d->unit->ops->map(d, iova, pa & ~(paddr_t)(PAGE_SIZE - 1), pages, prot);
    if (rc) {
        iova_free(&d->iova, iova, pages);
        spin_unlock_irqrestore(&d->lock, s);
        return 0;
    }
    d->maps++;
    d->pages_mapped += pages;
    spin_unlock_irqrestore(&d->lock, s);
    __atomic_fetch_add(&g_stats.maps, 1, __ATOMIC_RELAXED);
    return iova + off;
}

int iommu_dma_unmap(struct iommu_domain *d, uint64_t dma, size_t len)
{
    uint64_t iova = dma & ~(uint64_t)(PAGE_SIZE - 1);
    size_t pages = pages_of(dma, len);
    arch_irq_state_t s = spin_lock_irqsave(&d->lock);
    int rc = d->unit->ops->unmap(d, iova, pages);
    if (rc == 0) {
        iova_free(&d->iova, iova, pages);
    } else {
        /* The unit did not confirm the invalidation, so it may still
         * translate this range: the addresses stay taken for the life of
         * the domain rather than being handed to another buffer. The
         * pages behind them are the caller's to retire (dma_free). */
        d->iova.reserved += (unsigned)pages;
        __atomic_fetch_add(&g_stats.retired, (uint64_t)pages, __ATOMIC_RELAXED);
    }
    d->unmaps++;
    d->pages_mapped -= pages < d->pages_mapped ? pages : d->pages_mapped;
    spin_unlock_irqrestore(&d->lock, s);
    __atomic_fetch_add(&g_stats.unmaps, 1, __ATOMIC_RELAXED);
    if (rc)
        kwarn("iommu: %s: domain %u: %zu page(s) at %p retired unrevoked", d->unit->name, d->id, pages,
              (void *)(uintptr_t)iova);
    return rc;
}

/* --- faults ------------------------------------------------------------------- */

void iommu_note_fault(struct iommu_unit *u, uint32_t sid, uint64_t addr, unsigned reason, bool write)
{
    uint64_t n = __atomic_add_fetch(&g_stats.faults, 1, __ATOMIC_RELAXED);
    u->faults++;
    if (n <= 8)
        kwarn("iommu: %s: fault: requester %02x:%02x.%u %s %p (reason 0x%x)", u->name, sid >> 8, (sid >> 3) & 0x1f,
              sid & 7, write ? "writing" : "reading", (void *)(uintptr_t)addr, reason);
}

void iommu_get_stats(struct iommu_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_lock, s);
}

#include <kernel/module.h>
EXPORT_SYMBOL(iommu_present);
EXPORT_SYMBOL(iommu_get_stats);
