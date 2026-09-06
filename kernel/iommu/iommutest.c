/*
 * iommutest.c - The `iommu` self-test (docs/kernel/iommu/testing.md): a
 * domain of its own, mappings and lookups, the IOVA allocator, the DMA
 * API through an attached device, the unit's reserved ranges, and a
translation fault provoked through a real device.
 */

#include <kernel/blk.h>
#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/iommu.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/wait.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool selftest_iommu(const char **reason)
{
    if (!iommu_present()) {
        kinfo("selftest: iommu: no unit (QEMU without an IOMMU); skipping");
        return true;
    }
    struct iommu_stats s0, s1;
    iommu_get_stats(&s0);
    struct iommu_unit *u = iommu_unit_first();
    CHECK(u != NULL && s0.units >= 1 && s0.domains >= 1);   /* the drivers' domains exist */

    /* The IOVA allocator over a window of 8 pages. */
    struct iova_space sp;
    CHECK(iova_init(&sp, 0x10000, 0x10000 + 8 * PAGE_SIZE) == 0);
    uint64_t a = iova_alloc(&sp, 3), b = iova_alloc(&sp, 3), c = iova_alloc(&sp, 2);
    CHECK(a && b && c && a + 3 * PAGE_SIZE <= b && b + 3 * PAGE_SIZE <= c);
    CHECK(iova_alloc(&sp, 1) == 0);                          /* full */
    iova_free(&sp, b, 3);
    uint64_t d2 = iova_alloc(&sp, 3);
    CHECK(d2 == b);                                          /* the hole is reused */
    CHECK(iova_alloc(&sp, 1) == 0 && sp.used == 8);
    iova_free(&sp, a, 3);
    iova_free(&sp, d2, 3);
    iova_free(&sp, c, 2);
    CHECK(sp.used == 0 && iova_alloc(&sp, 8) == 0x10000);
    iova_free(&sp, 0x10000, 8);
    iova_fini(&sp);

    /* A domain: map, look up, refuse an overlap, unmap. */
    struct iommu_domain *dom = iommu_domain_create(u);
    CHECK(dom != NULL && dom->id != 0 && dom->root != 0);
    struct page *pg = pmm_alloc_pages(2, PMM_FLAGS_ZERO);   /* four pages */
    CHECK(pg != NULL);
    paddr_t pa = page_to_phys(pg);
    uint64_t iova = 0x200000;
    CHECK(iommu_map(dom, iova, pa, 3 * PAGE_SIZE, IOMMU_PROT_READ | IOMMU_PROT_WRITE) == 0);
    paddr_t got = 0;
    CHECK(iommu_lookup(dom, iova, &got) && got == pa);
    CHECK(iommu_lookup(dom, iova + 2 * PAGE_SIZE + 0x123, &got) && got == pa + 2 * PAGE_SIZE + 0x123);
    CHECK(!iommu_lookup(dom, iova + 3 * PAGE_SIZE, &got));
    CHECK(iommu_map(dom, iova + PAGE_SIZE, pa, PAGE_SIZE, IOMMU_PROT_READ) == -EEXIST);   /* overlap refused */
    CHECK(iommu_map(dom, iova + 3 * PAGE_SIZE, pa + 3 * PAGE_SIZE, PAGE_SIZE, IOMMU_PROT_READ) == 0);
    CHECK(iommu_lookup(dom, iova + 3 * PAGE_SIZE, &got) && got == pa + 3 * PAGE_SIZE);
    CHECK(iommu_map(dom, iova + 8, pa, PAGE_SIZE, IOMMU_PROT_READ) == -EINVAL);          /* unaligned */
    CHECK(iommu_unmap(dom, iova, 4 * PAGE_SIZE) == 0);
    CHECK(!iommu_lookup(dom, iova, &got) && !iommu_lookup(dom, iova + 3 * PAGE_SIZE, &got));
    CHECK(dom->maps == 2 && dom->unmaps == 1 && dom->pages_mapped == 0);

    /* The DMA API through a device attached to this domain. */
    struct device dev;
    memset(&dev, 0, sizeof(dev));
    strlcpy(dev.name, "iommu-test", sizeof(dev.name));
    dev.dma_mask = UINT64_MAX;
    dev.iommu = dom;
    uint8_t *buf = kmalloc(3000, 0);
    CHECK(buf != NULL);
    dma_addr_t m1 = dma_map(&dev, buf + 100, 2500, DMA_TO_DEVICE);
    CHECK(m1 != 0 && m1 >= IOMMU_IOVA_LO && m1 < IOMMU_IOVA_HI);
    CHECK((m1 & (PAGE_SIZE - 1)) == (((uintptr_t)buf + 100) & (PAGE_SIZE - 1)));   /* the page offset survives */
    CHECK(iommu_lookup(dom, m1, &got) && got == virt_to_phys(buf + 100));
    CHECK(iommu_lookup(dom, m1 + 2499, &got) && got == virt_to_phys(buf + 2599));
    dma_addr_t m2 = dma_map(&dev, buf, 3000, DMA_FROM_DEVICE);
    CHECK(m2 != 0 && m2 != m1);
    dma_unmap(&dev, m1, 2500, DMA_TO_DEVICE);
    CHECK(!iommu_lookup(dom, m1, &got));
    dma_unmap(&dev, m2, 3000, DMA_FROM_DEVICE);
    dma_addr_t ring = 0;
    void *rva = dma_alloc(&dev, 8192, &ring, DMA_ZERO);
    CHECK(rva != NULL && ring >= IOMMU_IOVA_LO && ring < IOMMU_IOVA_HI && (ring & (PAGE_SIZE - 1)) == 0);
    CHECK(iommu_lookup(dom, ring + PAGE_SIZE, &got) && got == virt_to_phys((uint8_t *)rva + PAGE_SIZE));
    dma_free(&dev, 8192, rva, ring);
    CHECK(!iommu_lookup(dom, ring, &got) && dom->iova.used == dom->iova.reserved);
    /* The unit's reserved ranges are out of the allocator and, when
     * identity-mapped (an MSI doorbell), translate to themselves. */
    if (u->ops->reserved) {
        struct iommu_range r[8];
        unsigned n = u->ops->reserved(u, r, 8);
        for (unsigned i = 0; i < n; i++) {
            if (r[i].base < IOMMU_IOVA_LO || r[i].base >= IOMMU_IOVA_HI)
                continue;
            CHECK(iommu_lookup(dom, r[i].base, &got) == r[i].identity && (!r[i].identity || got == r[i].base));
        }
    }
    kfree(buf);
    pmm_free_pages(pg, 2);
    iommu_domain_destroy(dom);

    /* Nothing above, and nothing the boot did so far, faulted. */
    iommu_get_stats(&s1);
    CHECK(s1.faults == s0.faults && s1.domains == s0.domains);

    /* A real device DMAing outside its domain: the translation is refused,
     * the command fails, the unit reports the fault, and the device keeps
     * working. Skipped where the block device has no hook or no domain. */
    struct blkdev *bd = blk_find("nvme0n1");
    unsigned long long faulted = 0;
    const char *why = NULL;
    if (bd != NULL && bd->ops->debug_dma != NULL && bd->dev != NULL && bd->dev->iommu != NULL) {
        uint64_t bad = IOMMU_IOVA_HI - PAGE_SIZE;      /* the last page: the allocator hands out the lowest */
        uint8_t *sec = kmalloc(512, 0);
        if (iommu_lookup(bd->dev->iommu, bad, &got)) {
            why = "the provoking address is mapped";
        } else {
            /* The status of the command itself is the device's business:
             * QEMU's controller reports success for an Identify whose data
             * never arrived. What must hold is that the write did not reach
             * memory and that the unit said so. */
            (void)bd->ops->debug_dma(bd, bad);
            for (unsigned ms = 0; ms < 500; ms++) {    /* the fault interrupt is asynchronous */
                iommu_get_stats(&s1);
                if (s1.faults != s0.faults)
                    break;
                thread_sleep_ms(1);
            }
            faulted = (unsigned long long)(s1.faults - s0.faults);
            if (faulted == 0)
                why = "the unit reported no fault";
            else if (sec == NULL || blk_read(bd, 0, 1, sec) != 0)
                why = "the device did not survive the fault";   /* mapped DMA still works */
        }
        kfree(sec);
    }
    if (bd != NULL)
        blkdev_put(bd);
    if (why != NULL) {
        *reason = why;
        return false;
    }
    iommu_get_stats(&s1);
    kinfo("selftest: iommu: %s, %u domains, %llu maps, %llu unmaps, %llu faults (%llu provoked)", u->name,
          s1.domains, (unsigned long long)s1.maps, (unsigned long long)s1.unmaps,
          (unsigned long long)s1.faults, faulted);
    return true;
}
