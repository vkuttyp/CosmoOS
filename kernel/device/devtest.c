/*
 * devtest.c - Self-tests for the device model, PCI, DMA, the entropy
 * pool, the block layer (through whatever virtio-blk registered) and
 * the virtio console sink.
 */

#include <kernel/blk.h>
#include <kernel/console.h>
#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include <drivers/pci.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define VIRTIO_VENDOR 0x1af4

/* --- device model with a synthetic bus ------------------------------ */

static bool fake_match(struct device *dev, struct device_driver *drv)
{
    return strcmp(dev->name, (const char *)drv->match_data) == 0;
}

static struct bus_type fake_bus = { .name = "selftest", .match = fake_match };
static int fake_probes, fake_removes, fake_probe_rc;

static int fake_probe(struct device *dev)
{
    fake_probes++;
    dev->drvdata = &fake_probes;
    return fake_probe_rc;
}

static void fake_remove(struct device *dev)
{
    (void)dev;
    fake_removes++;
}

static int count_cb(struct device *dev, void *arg)
{
    (void)dev;
    (*(unsigned *)arg)++;
    return 0;
}

bool selftest_device(const char **reason)
{
    static bool registered;
    if (!registered) {
        bus_register(&fake_bus);
        registered = true;
    }
    static struct device d1, d2;
    static struct device_driver drv = { .name = "fake", .match_data = "fake0", .probe = fake_probe,
                                        .remove = fake_remove };
    drv.bus = &fake_bus;
    fake_probes = fake_removes = 0;
    fake_probe_rc = 0;

    device_setup(&d1, &fake_bus, NULL, "fake0");
    device_setup(&d2, &fake_bus, NULL, "fake1");
    CHECK(d1.dma_mask == 0xFFFFFFFFULL && d1.state == DEV_UNBOUND);
    CHECK(device_add_resource(&d1, RES_MMIO, 0x1000, 0x100, 0) == 0);
    CHECK(device_add_resource(&d1, RES_IRQ, 5, 1, 0) == 0);
    CHECK(device_resource(&d1, RES_MMIO, 0)->start == 0x1000);
    CHECK(device_resource(&d1, RES_MMIO, 1) == NULL);
    CHECK(device_resource(&d1, RES_IRQ, 0)->start == 5);

    /* Device first, then driver: driver_register probes it. */
    CHECK(device_register(&d1) == 0);
    CHECK(device_register(&d1) == -EEXIST);
    CHECK(device_register(&d2) == 0);
    CHECK(device_count(&fake_bus) == 2);
    CHECK(driver_register(&drv) == 0);
    CHECK(fake_probes == 1 && d1.state == DEV_BOUND && d1.driver == &drv && d1.drvdata == &fake_probes);
    CHECK(d2.state == DEV_UNBOUND && drv.bound == 1);
    CHECK(driver_register(&drv) == -EEXIST);

    struct device *f = device_find(&fake_bus, "fake0");
    CHECK(f == &d1 && kobject_refcount(&d1.obj) == 3);   /* init + bus + find */
    device_put(f);
    CHECK(device_find(&fake_bus, "nope") == NULL);
    unsigned n = 0;
    CHECK(device_for_each(&fake_bus, count_cb, &n) == 0 && n == 2);

    /* Driver removal unbinds. */
    driver_unregister(&drv);
    CHECK(fake_removes == 1 && d1.state == DEV_UNBOUND && d1.driver == NULL && d1.drvdata == NULL);

    /* Driver first, then device: device_register probes; a failing
     * probe leaves the device registered and marked. */
    fake_probe_rc = -EIO;
    CHECK(driver_register(&drv) == 0);
    CHECK(fake_probes == 2 && d1.state == DEV_FAILED && d1.probe_error == -EIO && d1.driver == NULL);
    driver_unregister(&drv);
    CHECK(d1.state == DEV_UNBOUND);
    fake_probe_rc = 0;
    device_unregister(&d1);
    device_unregister(&d2);
    CHECK(device_count(&fake_bus) == 0);
    CHECK(kobject_refcount(&d1.obj) == 1);
    CHECK(bus_find("selftest") == &fake_bus && bus_find("pci") == &pci_bus);
    return true;
}

/* --- PCI ------------------------------------------------------------------ */

bool selftest_pci(const char **reason)
{
    CHECK(pci_device_count() >= 1);
    struct pci_device *host = pci_device_at(0);
    CHECK(host != NULL && host->bus == 0 && host->slot == 0 && host->func == 0);
    CHECK(host->class == 0x06 && host->subclass == 0x00);   /* host bridge */
    CHECK(strcmp(host->dev.name, "pci:00:00.0") == 0 && host->dev.bus == &pci_bus);

    struct device *d = device_find(&pci_bus, "pci:00:00.0");
    CHECK(d == &host->dev);
    device_put(d);

    for (unsigned i = 0; i < pci_device_count(); i++) {
        struct pci_device *p = pci_device_at(i);
        CHECK(p != NULL);
        /* Access widths agree. */
        uint32_t w = pci_cfg_read32(p, PCI_VENDOR_ID);
        CHECK((w & 0xffff) == pci_cfg_read16(p, PCI_VENDOR_ID));
        CHECK((w >> 16) == pci_cfg_read16(p, PCI_DEVICE_ID));
        CHECK((w & 0xff) == pci_cfg_read8(p, PCI_VENDOR_ID));
        CHECK((uint16_t)w == p->vendor && (uint16_t)(w >> 16) == p->device);
        CHECK(p->vendor != 0xffff);
        for (unsigned b = 0; b < PCI_MAX_BARS; b++) {
            if (p->bar[b].size == 0)
                continue;
            CHECK((p->bar[b].size & (p->bar[b].size - 1)) == 0);
            CHECK((p->bar[b].base & (p->bar[b].size - 1)) == 0 || p->bar[b].base == 0);
        }
        if (p->cap_msix)
            CHECK(pci_find_capability(p, PCI_CAP_ID_MSIX, 0) == p->cap_msix);
        if (p->cap_msi)
            CHECK(pci_find_capability(p, PCI_CAP_ID_MSI, 0) == p->cap_msi);
        CHECK(pci_find_capability(p, 0xfe, 0) == 0);
    }
    CHECK(pci_device_at(pci_device_count()) == NULL);
    CHECK(pci_find_device(0xdead, 0xbeef, NULL) == NULL);
    CHECK(pci_find_device(host->vendor, host->device, NULL) == host);

    /* Every virtio device QEMU attached must have MSI-X and a 64-bit BAR. */
    unsigned virtio = 0;
    for (struct pci_device *p = pci_find_device(VIRTIO_VENDOR, PCI_ANY, NULL); p;
         p = pci_find_device(VIRTIO_VENDOR, PCI_ANY, p)) {
        virtio++;
        CHECK(p->cap_msix != 0);
        bool mem = false;
        for (unsigned b = 0; b < PCI_MAX_BARS; b++)
            mem = mem || (p->bar[b].size && !p->bar[b].io);
        CHECK(mem);
    }
    kinfo("selftest: pci: %u devices, %u virtio, %s access", pci_device_count(), virtio,
          pci_ecam_in_use() ? "ECAM" : "legacy");
    return true;
}

/* --- DMA ------------------------------------------------------------------ */

bool selftest_dma(const char **reason)
{
    struct dma_stats before, after;
    dma_get_stats(&before);

    dma_addr_t dma = 0;
    uint8_t *va = dma_alloc(NULL, 8192, &dma, DMA_ZERO);
    CHECK(va != NULL && dma != 0 && dma + 8192 <= 0x100000000ULL);
    CHECK(((uintptr_t)va & (PAGE_SIZE - 1)) == 0 && (dma & (PAGE_SIZE - 1)) == 0);
    CHECK(virt_is_direct_map((vaddr_t)va) && virt_to_phys(va) == dma);
    bool zero = true;
    for (unsigned i = 0; i < 8192; i++)
        zero = zero && va[i] == 0;
    CHECK(zero);
    va[0] = 1;
    va[8191] = 2;
    CHECK(dma_map(NULL, va, 8192, DMA_TO_DEVICE) == dma);
    CHECK(dma_map(NULL, va + 100, 50, DMA_FROM_DEVICE) == dma + 100);
    dma_free(NULL, 8192, va, dma);

    /* A 24-bit device gets memory below 16 MiB. */
    struct device tiny;
    device_setup(&tiny, &pci_bus, NULL, "dma-test");
    CHECK(dma_set_mask(&tiny, 23) == -EINVAL && dma_set_mask(&tiny, 65) == -EINVAL);
    CHECK(dma_set_mask(&tiny, 24) == 0 && tiny.dma_mask == 0xFFFFFFULL);
    dma_addr_t low = 0;
    void *lva = dma_alloc(&tiny, 4096, &low, 0);
    CHECK(lva != NULL && low + 4096 <= 0x1000000ULL);
    dma_free(&tiny, 4096, lva, low);
    CHECK(dma_set_mask(&tiny, 64) == 0 && tiny.dma_mask == UINT64_MAX);

    /* kmalloc memory maps; arena memory and a stack address do not. */
    void *kb = kmalloc(256, 0);
    CHECK(kb != NULL && dma_map(NULL, kb, 256, DMA_TO_DEVICE) == virt_to_phys(kb));
    kfree(kb);
    vaddr_t arena = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_POPULATE, VM_PROT_RW);
    CHECK(arena != 0 && dma_map(NULL, (void *)arena, 64, DMA_TO_DEVICE) == 0);
    vm_kernel_free(arena);
    int on_stack = 0;
    CHECK(dma_map(NULL, &on_stack, sizeof(on_stack), DMA_TO_DEVICE) == 0);
    CHECK(dma_map(NULL, NULL, 0, DMA_TO_DEVICE) == 0);

    dma_get_stats(&after);
    CHECK(after.allocs == before.allocs + 2 && after.frees == before.frees + 2);
    CHECK(after.bytes_allocated == before.bytes_allocated);
    CHECK(after.maps == before.maps + 3 && after.map_failures == before.map_failures + 3);
    return true;
}

/* --- entropy -------------------------------------------------------------- */

bool selftest_random(const char **reason)
{
    uint64_t a = random_u64(), b = random_u64();
    CHECK(a != b);
    uint8_t buf[64];
    random_get_bytes(buf, sizeof(buf));
    bool nonzero = false;
    for (unsigned i = 0; i < sizeof(buf); i++)
        nonzero = nonzero || buf[i] != 0;
    CHECK(nonzero);
    unsigned bits = random_entropy_bits();
    random_add_entropy("selftest", 8, 8);
    CHECK(random_entropy_bits() >= bits && random_entropy_bits() <= 512);

    /* If QEMU attached a virtio-rng and its driver loaded, the pool must
     * have been fed by now. */
    bool rng_present = pci_find_device(VIRTIO_VENDOR, 0x1005, NULL) || pci_find_device(VIRTIO_VENDOR, 0x1044, NULL);
    if (rng_present) {
        CHECK(random_source_bytes() > 0);
        kinfo("selftest: random: %llu bytes from hardware, %u bits credited",
              (unsigned long long)random_source_bytes(), random_entropy_bits());
    } else {
        kinfo("selftest: random: no virtio-rng present");
    }
    return true;
}

/* --- block ---------------------------------------------------------------- */

bool selftest_blk(const char **reason)
{
    struct blkdev *bd = blk_find("vda");
    if (bd == NULL) {
        kinfo("selftest: blk: no vda; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: blk: step failed at line %d", __LINE__); ok = false; } } while (0)
    CHECK(bd->sector_size == 512 && bd->capacity >= 2048 && bd->max_sectors >= 8 && bd->max_sectors <= 1024);
    uint64_t reads0 = bd->reads, writes0 = bd->writes, errors0 = bd->errors;

    /* One more than the driver's per-bio limit, so each helper call
     * splits into two bios (max_sectors and the remainder). */
    const uint32_t N = bd->max_sectors + 1;
    size_t bytes = (size_t)N * bd->sector_size;
    uint8_t *w = kmalloc(bytes, 0);
    uint8_t *r = kmalloc(bytes, 0);
    if (w == NULL || r == NULL) {
        kfree(w);
        kfree(r);
        blkdev_put(bd);
        *reason = "kmalloc failed";
        return false;
    }
    for (size_t i = 0; i < bytes; i++)
        w[i] = (uint8_t)(i * 7 + 3);
    uint64_t base = 1000;

    STEP(blk_write(bd, base, N, w) == 0);
    memset(r, 0, bytes);
    STEP(blk_read(bd, base, N, r) == 0);
    STEP(memcmp(w, r, bytes) == 0);

    /* Overwrite the middle sector and re-read the whole span. */
    memset(w + bd->sector_size * 4, 0xa5, bd->sector_size);
    STEP(blk_write(bd, base + 4, 1, w + bd->sector_size * 4) == 0);
    STEP(blk_read(bd, base, N, r) == 0 && memcmp(w, r, bytes) == 0);
    STEP(blk_flush(bd) == 0);

    /* Rejections. */
    STEP(blk_read(bd, bd->capacity, 1, r) == -EINVAL);
    STEP(blk_read(bd, bd->capacity - 1, 2, r) == -EINVAL);
    STEP(blk_read(bd, 0, 0, r) == -EINVAL);
    uint8_t stackbuf[512];
    STEP(blk_read(bd, 0, 1, stackbuf) == -EINVAL);   /* not DMA-able */
    struct bio bad = { .dev = bd, .dir = BIO_READ, .sector = 0, .nsectors = 1, .buf = r, .done = NULL };
    STEP(blk_submit(&bad) == -EINVAL);

    kfree(w);
    kfree(r);
    kinfo("selftest: blk: reads %llu->%llu writes %llu->%llu errors %llu->%llu", (unsigned long long)reads0,
          (unsigned long long)bd->reads, (unsigned long long)writes0, (unsigned long long)bd->writes,
          (unsigned long long)errors0, (unsigned long long)bd->errors);
    STEP(bd->reads == reads0 + 4 && bd->writes == writes0 + 3 && bd->errors == errors0);
    blkdev_put(bd);
    CHECK(ok);
#undef STEP
    return true;
}

/* --- virtio console --------------------------------------------------------- */

bool selftest_virtio_console(const char **reason)
{
    bool present = pci_find_device(VIRTIO_VENDOR, 0x1003, NULL) || pci_find_device(VIRTIO_VENDOR, 0x1043, NULL);
    if (!present) {
        kinfo("selftest: virtio-console: no device; skipping");
        return true;
    }
    CHECK(console_has_sink("virtio-console"));
    CHECK(!console_has_sink("no-such-sink"));
    return true;
}
