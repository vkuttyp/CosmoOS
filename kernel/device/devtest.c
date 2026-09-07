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
#include <kernel/faultinject.h>
#include <kernel/iommu.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/cosmofs.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include <uapi/cosmo/syscall.h>

#include <drivers/pci.h>
#include <drivers/usb.h>

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
    CHECK(device_register(&d1) == -EINVAL);   /* no release: refused */
    d1.release = device_release_static;
    d2.release = device_release_static;
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
    dma_unmap(NULL, dma, 8192, DMA_TO_DEVICE);
    dma_unmap(NULL, dma + 100, 50, DMA_FROM_DEVICE);
    /* The predicate answers like dma_map without counting a mapping. */
    CHECK(dma_mappable(NULL, va, 8192) && !dma_mappable(NULL, va, 0));
    dma_free(NULL, 8192, va, dma);

    /* A 24-bit device gets memory below 16 MiB. */
    struct device tiny;
    device_setup(&tiny, &pci_bus, NULL, "dma-test");
    CHECK(dma_set_mask(&tiny, 23) == -EINVAL && dma_set_mask(&tiny, 65) == -EINVAL);
    CHECK(dma_set_mask(&tiny, 24) == 0 && tiny.dma_mask == 0xFFFFFFULL);
    dma_addr_t low = 0;
    void *lva = dma_alloc(&tiny, 4096, &low, 0);
    struct pmm_stats zs;
    pmm_get_stats(&zs);
    if (zs.zone_free[PMM_ZONE_DMA] > 0) {
        CHECK(lva != NULL && low + 4096 <= 0x1000000ULL);
        dma_free(&tiny, 4096, lva, low);
    } else {
        CHECK(lva == NULL);   /* no RAM below 16 MiB on this platform (QEMU virt) */
    }
    CHECK(dma_set_mask(&tiny, 64) == 0 && tiny.dma_mask == UINT64_MAX);

    /* kmalloc memory maps; arena memory and a stack address do not. */
    void *kb = kmalloc(256, 0);
    CHECK(kb != NULL && dma_map(NULL, kb, 256, DMA_TO_DEVICE) == virt_to_phys(kb));
    dma_unmap(NULL, virt_to_phys(kb), 256, DMA_TO_DEVICE);
    kfree(kb);
    vaddr_t arena = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_POPULATE, VM_PROT_RW);
    CHECK(arena != 0 && dma_map(NULL, (void *)arena, 64, DMA_TO_DEVICE) == 0);
    vm_kernel_free(arena);
    int on_stack = 0;
    CHECK(dma_map(NULL, &on_stack, sizeof(on_stack), DMA_TO_DEVICE) == 0);
    CHECK(dma_map(NULL, NULL, 0, DMA_TO_DEVICE) == 0);

    dma_get_stats(&after);
    unsigned tiny_ok = lva != NULL ? 1u : 0u;   /* the 24-bit allocation exists only with a DMA zone */
    CHECK(after.allocs == before.allocs + 1 + tiny_ok && after.frees == before.frees + 1 + tiny_ok);
    CHECK(after.bytes_allocated == before.bytes_allocated);
    CHECK(after.maps == before.maps + 3 && after.map_failures == before.map_failures + 3);
    CHECK(after.unmaps == before.unmaps + 3);

    /* Every mapping a driver takes for a request is undone at completion:
     * a burst of I/O on the real disk leaves maps - unmaps where it was. */
    struct blkdev *bd = blk_find("vda");
    if (bd) {
        uint8_t *buf = kmalloc(8192, 0);
        CHECK(buf != NULL);
        dma_get_stats(&before);
        for (unsigned i = 0; i < 16; i++)
            CHECK(blk_read(bd, 2048 + i * 16, 16, buf) == 0);
        CHECK(blk_write(bd, 2048, 16, buf) == 0 && blk_flush(bd) == 0);
        dma_get_stats(&after);
        CHECK(after.maps - after.unmaps == before.maps - before.unmaps);
        CHECK(after.maps > before.maps);
        kfree(buf);
        blkdev_put(bd);
    }
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

/* --- NVMe (milestone 9) --------------------------------------------------------
 *
 * The driver is a module; the kernel reaches its namespace only through the
 * block layer, which is the point: everything below is generic block I/O.
 */
struct nvme_worker {
    struct blkdev *bd;
    uint8_t *buf;
    unsigned cpu;
    int rc;
};

static void nvme_cpu_worker(void *arg)
{
    struct nvme_worker *w = arg;
    for (unsigned i = 0; i < 8 && w->rc == 0; i++)
        w->rc = blk_read(w->bd, 4096 + (uint64_t)w->cpu * 64 + i * 8, 8, w->buf);
    thread_exit(0);
}

bool selftest_nvme(const char **reason)
{
    struct blkdev *bd = blk_find("nvme0n1");
    if (bd == NULL) {
        kinfo("selftest: nvme: no nvme0n1; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: nvme: step failed at line %d", __LINE__); ok = false; } } while (0)
    STEP(bd->sector_size == 512 && bd->capacity == 16384 && bd->nr_queues >= 1 && bd->max_segments >= 8);
    STEP(bd->max_sectors >= 64);
    struct dma_stats d0, d1;
    dma_get_stats(&d0);

    /* Single-buffer round trips, a flush, and a rejection. */
    uint8_t *w = kmalloc(65536, 0), *r = kmalloc(65536, 0);
    STEP(w != NULL && r != NULL);
    if (w && r) {
        for (unsigned i = 0; i < 65536; i++)
            w[i] = (uint8_t)(i * 11 + 5);
        uint32_t n = bd->max_sectors < 128 ? bd->max_sectors : 128;
        STEP(blk_write(bd, 100, n, w) == 0);
        memset(r, 0, 65536);
        STEP(blk_read(bd, 100, n, r) == 0 && memcmp(w, r, (size_t)n * 512) == 0);
        STEP(blk_flush(bd) == 0);
        STEP(blk_read(bd, bd->capacity, 1, r) == -EINVAL);
    }

    /* Four pages in two segments: PRP1, then a PRP list (more than two pages). */
    dma_addr_t da, db, dc;
    uint8_t *a = dma_alloc(NULL, 2 * PAGE_SIZE, &da, 0), *b = dma_alloc(NULL, 2 * PAGE_SIZE, &db, 0);
    uint8_t *flat = dma_alloc(NULL, 4 * PAGE_SIZE, &dc, DMA_ZERO);
    STEP(a && b && flat);
    if (a && b && flat) {
        for (unsigned i = 0; i < 2 * PAGE_SIZE; i++) {
            a[i] = (uint8_t)(i ^ 0x5a);
            b[i] = (uint8_t)(i ^ 0xa5);
        }
        struct bio_vec vecs[2] = { { a, (uint32_t)(2 * PAGE_SIZE) }, { b, (uint32_t)(2 * PAGE_SIZE) } };
        struct sync_marker { volatile bool done; int status; } mk = { false, 0 };
        struct bio bio;
        memset(&bio, 0, sizeof(bio));
        bio.dev = bd;
        bio.dir = BIO_WRITE;
        bio.sector = 1024;
        bio.nsectors = (uint32_t)(4 * PAGE_SIZE / 512);
        bio.vecs = vecs;
        bio.nr_vecs = 2;
        /* A completion through a stack marker: done() runs in interrupt context. */
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        for (unsigned i = 0; i < 2000 && !mk.done; i++)
            thread_sleep_ms(1);
        STEP(mk.done && mk.status == 0);
        STEP(blk_read(bd, 1024, bio.nsectors, flat) == 0);
        STEP(memcmp(flat, a, 2 * PAGE_SIZE) == 0 && memcmp(flat + 2 * PAGE_SIZE, b, 2 * PAGE_SIZE) == 0);
    }

    /* Queue locality: reads issued from every CPU complete on that CPU
     * when the controller granted one queue per CPU. */
    uint64_t local0 = bd->completed_local, remote0 = bd->completed_remote;
    unsigned ncpu = cpu_count();
    struct nvme_worker workers[CONFIG_MAX_CPUS];
    struct thread *threads[CONFIG_MAX_CPUS];
    unsigned started = 0;
    for (unsigned c = 0; c < ncpu && c < CONFIG_MAX_CPUS; c++) {
        if (!cpu_online(c))
            continue;
        workers[c].bd = bd;
        workers[c].cpu = c;
        workers[c].rc = 0;
        workers[c].buf = kmalloc(4096, 0);
        threads[c] = workers[c].buf ? thread_create_on(nvme_cpu_worker, &workers[c], "nvme-cpu", SCHED_PRIO_DEFAULT,
                                                       CPUMASK_OF(c))
                                    : NULL;
        if (threads[c])
            started++;
    }
    for (unsigned c = 0; c < ncpu && c < CONFIG_MAX_CPUS; c++) {
        if (!cpu_online(c) || threads[c] == NULL)
            continue;
        thread_join(threads[c]);
        STEP(workers[c].rc == 0);
        kfree(workers[c].buf);
    }
    uint64_t local = bd->completed_local - local0, remote = bd->completed_remote - remote0;
    STEP(local + remote == 8ull * started);
    if (bd->nr_queues >= ncpu)
        STEP(local * 10 >= (local + remote) * 9);   /* at least 90 %: a migration between pick and doorbell is allowed */

    /* Every mapping undone. */
    dma_get_stats(&d1);
    STEP(d1.maps - d1.unmaps == d0.maps - d0.unmaps && d1.maps > d0.maps);

    /* A filesystem on it: format, mount, write, remount, read back. */
    STEP(vfs_mkdir(NULL, "/mnt-nvme", 0755) == 0);
    STEP(cosmofs_format(bd) == 0);
    STEP(vfs_mount("/mnt-nvme", "cosmofs", bd, 0) == 0);
    struct file *f = NULL;
    STEP(vfs_open(NULL, "/mnt-nvme/hello", COSMO_O_CREAT | COSMO_O_RDWR, 0644, &f) == 0);
    if (f) {
        STEP(file_write(f, w, 12000) == 12000);
        STEP(file_sync(f) == 0);
        file_put(f);
        f = NULL;
    }
    STEP(vfs_umount("/mnt-nvme") == 0);
    STEP(vfs_mount("/mnt-nvme", "cosmofs", bd, 0) == 0);
    STEP(vfs_open(NULL, "/mnt-nvme/hello", COSMO_O_RDONLY, 0, &f) == 0);
    if (f) {
        memset(r, 0, 65536);
        int64_t got = file_read(f, r, 65536);
        STEP(got == 12000 && memcmp(r, w, 12000) == 0);
        file_put(f);
    }
    STEP(vfs_umount("/mnt-nvme") == 0);
    STEP(vfs_rmdir(NULL, "/mnt-nvme") == 0);

    if (a)
        dma_free(NULL, 2 * PAGE_SIZE, a, da);
    if (b)
        dma_free(NULL, 2 * PAGE_SIZE, b, db);
    if (flat)
        dma_free(NULL, 4 * PAGE_SIZE, flat, dc);
    kfree(w);
    kfree(r);
    kinfo("selftest: nvme: %u queue(s); %llu of %llu completions on the issuing CPU; cosmofs mounted and read back",
          bd->nr_queues, (unsigned long long)local, (unsigned long long)(local + remote));
    blkdev_put(bd);
    CHECK(ok);
#undef STEP
    return true;
}

void selftest_nvme_mark_done(struct bio *bio)
{
    struct { volatile bool done; int status; } *mk = bio->arg;
    mk->status = bio->status;
    __atomic_store_n(&mk->done, true, __ATOMIC_RELEASE);
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

/* --- block device lifetime ---------------------------------------------------
 *
 * docs/kernel/quiesce/design.md, "Block devices": the registry and the
 * creator each hold a reference, blk_find hands out more, unregister
 * makes submit fail with -ENODEV, and the driver's release runs once,
 * when the last holder is gone.
 */
struct fake_blk {
    struct blkdev bd;
    unsigned submits;
    unsigned releases;
};

static int fake_blk_submit(struct blkdev *bd, struct bio *bio)
{
    struct fake_blk *f = bd->priv;
    f->submits++;
    bio_complete(bio, 0);
    return 0;
}

static void fake_blk_release(struct blkdev *bd)
{
    struct fake_blk *f = bd->priv;
    f->releases++;
}

static void fake_bio_done(struct bio *bio)
{
    (void)bio;
}

bool selftest_blk_lifetime(const char **reason)
{
    static struct fake_blk f;
    static const struct blkdev_ops no_release = { .submit = fake_blk_submit };
    static const struct blkdev_ops ops = { .submit = fake_blk_submit, .release = fake_blk_release };
    memset(&f, 0, sizeof(f));
    f.bd.ops = &no_release;
    f.bd.sector_size = 512;
    f.bd.capacity = 8;
    f.bd.max_sectors = 8;
    f.bd.priv = &f;
    CHECK(blk_register(&f.bd, "zz") == -EINVAL);   /* no release: refused */
    f.bd.ops = &ops;
    unsigned before = blk_count();
    CHECK(blk_register(&f.bd, "zz") == 0);
    CHECK(strcmp(f.bd.name, "zza") == 0 && blk_count() == before + 1);
    CHECK(kobject_refcount(&f.bd.obj) == 2);       /* creator + registry */

    struct blkdev *found = blk_find("zza");
    CHECK(found == &f.bd && kobject_refcount(&f.bd.obj) == 3);

    void *buf = kmalloc(512, 0);   /* blk_submit requires DMA-able memory */
    CHECK(buf != NULL);
    struct bio bio = { .dev = found, .sector = 0, .nsectors = 1, .dir = BIO_READ, .buf = buf, .done = fake_bio_done };
    CHECK(blk_submit(&bio) == 0 && f.submits == 1 && bio.status == 0);

    blk_unregister(&f.bd);
    CHECK(blk_count() == before && blk_find("zza") == NULL);
    CHECK(kobject_refcount(&f.bd.obj) == 2);       /* registry's reference gone */
    CHECK(f.bd.gone);
    CHECK(blk_submit(&bio) == -ENODEV && f.submits == 1);
    kfree(buf);

    blkdev_put(&f.bd);                             /* the creator is done */
    CHECK(f.releases == 0);                        /* the finder still holds it */
    blkdev_put(found);
    CHECK(f.releases == 1);

    /* Name exhaustion is refused before the object exists: the 27th "zy"
     * device gets -ENOSPC with no kobject and no owner count to balance. */
    static struct fake_blk many[27];
    memset(many, 0, sizeof(many));
    unsigned n;
    for (n = 0; n < 27; n++) {
        many[n].bd.ops = &ops;
        many[n].bd.sector_size = 512;
        many[n].bd.capacity = 8;
        many[n].bd.max_sectors = 8;
        many[n].bd.priv = &many[n];
        int rc = blk_register(&many[n].bd, "zy");
        if (n < 26)
            CHECK(rc == 0);
        else
            CHECK(rc == -ENOSPC);
    }
    CHECK(many[26].bd.obj.type == NULL && many[26].bd.obj.refcount == 0 && many[26].bd.obj.owner == NULL);
    CHECK(strcmp(many[25].bd.name, "zyz") == 0);
    for (n = 0; n < 26; n++) {
        blk_unregister(&many[n].bd);
        blkdev_put(&many[n].bd);
        CHECK(many[n].releases == 1);
    }
    CHECK(blk_count() == before);
    return true;
}

/* --- the USB bus (docs/drivers/usb/testing.md) ------------------------------------- */

struct usb_enum_walk {
    unsigned devices;
    struct usb_device *first;
};

static int usb_enum_visit(struct device *dev, void *arg)
{
    struct usb_enum_walk *w = arg;
    w->devices++;
    if (w->first == NULL) {
        device_get(dev);
        w->first = to_usb_device(dev);
    }
    return 0;
}

/*
 * What enumeration must have produced for the harness's mass-storage
 * device: one device on the bus whose parent is a PCI function, whose
 * descriptors are consistent with each other and with the wire, and
 * whose one interface is bulk-only mass storage with a bulk pair. The
 * kernel reaches the module's objects only through the model and the
 * shared header: no module symbol is called from here.
 */
bool selftest_usb_enum(const char **reason)
{
    struct bus_type *bus = bus_find("usb");
    if (bus == NULL) {
        kinfo("selftest: usb-enum: no usb bus (xhci module not loaded); skipping");
        return true;
    }
    struct usb_enum_walk w = { 0, NULL };
    device_for_each(bus, usb_enum_visit, &w);
    if (w.devices == 0) {
        kinfo("selftest: usb-enum: no device on the usb bus (QEMU_USB=0); skipping");
        return true;
    }
    struct usb_device *udev = w.first;
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: usb-enum: step failed at line %d", __LINE__); ok = false; } } while (0)
    STEP(w.devices == 1);
    STEP(udev->dev.parent != NULL && udev->dev.parent->bus == &pci_bus);   /* behind a controller, not a bus root */
    STEP(udev->dev.iommu == NULL);   /* the device does no DMA of its own (U1); the controller does */
    STEP(udev->port >= 1 && udev->port <= udev->hcd->nr_ports);
    STEP(udev->hcd->port_dev[udev->port] == udev);
    STEP(udev->slot != 0);
    STEP(udev->speed == USB_SPEED_HIGH || udev->speed == USB_SPEED_SUPER);
    STEP(udev->desc.bLength == 18 && udev->desc.bDescriptorType == USB_DT_DEVICE);
    STEP(udev->desc.bNumConfigurations >= 1);
    STEP(udev->desc.bMaxPacketSize0 == (udev->speed == USB_SPEED_SUPER ? 9 : 64));   /* SS: 2^9 */
    STEP(udev->config.bDescriptorType == USB_DT_CONFIG && udev->config.wTotalLength == udev->raw_len);
    STEP(udev->config.bNumInterfaces == udev->nr_intf && udev->nr_intf == 1);
    const struct usb_interface *intf = &udev->intf[0];
    STEP(intf->desc.bInterfaceClass == USB_CLASS_MASS_STORAGE && intf->desc.bInterfaceSubClass == 0x06 &&
         intf->desc.bInterfaceProtocol == 0x50);
    STEP(intf->desc.bNumEndpoints == intf->nr_ep && intf->nr_ep == 2);
    bool have_in = false, have_out = false;
    for (unsigned i = 0; i < intf->nr_ep; i++) {
        const struct usb_endpoint_descriptor *e = &intf->ep[i].desc;
        STEP(USB_EP_XFER(e->bmAttributes) == USB_EP_BULK);
        STEP((e->wMaxPacketSize & 0x7ff) == (udev->speed == USB_SPEED_SUPER ? 1024 : 512));
        if (e->bEndpointAddress & USB_EP_DIR_IN)
            have_in = true;
        else
            have_out = true;
    }
    STEP(have_in && have_out);
    STEP(!udev->gone);
    if (ok)
        kinfo("selftest: usb-enum: %s (%04x:%04x) on %s, port %u, slot %u: %u interface, bulk in/out of %u bytes",
              udev->dev.name, udev->desc.idVendor, udev->desc.idProduct, udev->dev.parent->name, udev->port,
              udev->slot, udev->nr_intf, intf->ep[0].desc.wMaxPacketSize & 0x7ff);
#undef STEP
    device_put(&udev->dev);
    if (!ok) {
        *reason = "usb-enum: see the log";
        return false;
    }
    return true;
}

/*
 * The USB disk through the block layer, as nvme is checked above: the
 * geometry the harness gave it, round trips at the start, a 64 KiB-aligned
 * middle and the last sector, a multi-segment bio, a flush, a refused
 * out-of-range read -- and the blkdev's counters advanced by exactly the
 * operations issued, because the NIC benchmark found a driver counting
 * what the layer already counts.
 */
bool selftest_usb_storage(const char **reason)
{
    struct blkdev *bd = blk_find("sda");
    if (bd == NULL) {
        kinfo("selftest: usb-storage: no sda; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: usb-storage: step failed at line %d", __LINE__); ok = false; } } while (0)
    STEP(bd->sector_size == 512 && bd->capacity == 16384 && bd->max_sectors >= 64 && bd->max_segments >= 4);
    STEP(bd->dev != NULL && bd->dev->bus == &pci_bus);   /* DMA is the controller's (U1) */
    uint64_t reads0 = bd->reads, writes0 = bd->writes, flushes0 = bd->flushes, errors0 = bd->errors;
    struct dma_stats d0, d1;
    dma_get_stats(&d0);

    uint8_t *w = kmalloc(65536, 0), *r = kmalloc(65536, 0);
    STEP(w != NULL && r != NULL);
    unsigned reads = 0, writes = 0, flushes = 0;
    if (w && r) {
        for (unsigned i = 0; i < 65536; i++)
            w[i] = (uint8_t)(i * 7 + 3);
        uint32_t n = bd->max_sectors < 128 ? bd->max_sectors : 128;
        const uint64_t at[3] = { 0, 8192, bd->capacity - n };   /* start, a 64 KiB-aligned middle, the end */
        for (unsigned k = 0; k < 3 && ok; k++) {
            w[0] = (uint8_t)k;
            STEP(blk_write(bd, at[k], n, w) == 0);
            writes++;
            memset(r, 0, 65536);
            STEP(blk_read(bd, at[k], n, r) == 0 && memcmp(w, r, (size_t)n * 512) == 0);
            reads++;
        }
        STEP(blk_flush(bd) == 0);
        flushes++;
        STEP(blk_read(bd, bd->capacity, 1, r) == -EINVAL);   /* refused by the layer: no exchange */
        STEP(blk_read(bd, 1, 1, r) == 0 && memcmp(r, w + 512, 512) == 0);   /* the first write's second sector */
        reads++;
    }

    /* Four pages in two segments, one TD of chained TRBs. */
    dma_addr_t da, db, dc;
    uint8_t *a = dma_alloc(NULL, 2 * PAGE_SIZE, &da, 0), *b = dma_alloc(NULL, 2 * PAGE_SIZE, &db, 0);
    uint8_t *flat = dma_alloc(NULL, 4 * PAGE_SIZE, &dc, DMA_ZERO);
    STEP(a && b && flat);
    if (a && b && flat) {
        for (unsigned i = 0; i < 2 * PAGE_SIZE; i++) {
            a[i] = (uint8_t)(i ^ 0x3c);
            b[i] = (uint8_t)(i ^ 0xc3);
        }
        struct bio_vec vecs[2] = { { a, (uint32_t)(2 * PAGE_SIZE) }, { b, (uint32_t)(2 * PAGE_SIZE) } };
        struct sync_marker { volatile bool done; int status; } mk = { false, 0 };
        struct bio bio;
        memset(&bio, 0, sizeof(bio));
        bio.dev = bd;
        bio.dir = BIO_WRITE;
        bio.sector = 4096;
        bio.nsectors = (uint32_t)(4 * PAGE_SIZE / 512);
        bio.vecs = vecs;
        bio.nr_vecs = 2;
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        for (unsigned i = 0; ok && i < 5000 && !mk.done; i++)
            thread_sleep_ms(1);
        STEP(mk.done && mk.status == 0);
        writes++;
        STEP(blk_read(bd, 4096, bio.nsectors, flat) == 0);
        reads++;
        STEP(memcmp(flat, a, 2 * PAGE_SIZE) == 0 && memcmp(flat + 2 * PAGE_SIZE, b, 2 * PAGE_SIZE) == 0);
    }
    if (a)
        dma_free(NULL, 2 * PAGE_SIZE, a, da);
    if (b)
        dma_free(NULL, 2 * PAGE_SIZE, b, db);
    if (flat)
        dma_free(NULL, 4 * PAGE_SIZE, flat, dc);
    kfree(w);
    kfree(r);

    /* Counted once each, by the layer; the driver adds nothing. */
    STEP(bd->reads - reads0 == reads && bd->writes - writes0 == writes && bd->flushes - flushes0 == flushes);
    STEP(bd->errors == errors0);
    dma_get_stats(&d1);
    STEP(d1.maps - d0.maps == d1.unmaps - d0.unmaps);   /* every segment mapped for a transfer was unmapped */
    if (ok)
        kinfo("selftest: usb-storage: %s: %u reads, %u writes, %u flush through %s; %llu segments mapped and unmapped",
              bd->name, reads, writes, flushes, bd->dev->name, (unsigned long long)(d1.maps - d0.maps));
#undef STEP
    blkdev_put(bd);
    if (!ok) {
        *reason = "usb-storage: see the log";
        return false;
    }
    return true;
}

/* The one device on the USB bus, referenced, or NULL. */
static struct usb_device *usb_first_device(void)
{
    struct bus_type *bus = bus_find("usb");
    if (bus == NULL)
        return NULL;
    struct usb_enum_walk w = { 0, NULL };
    device_for_each(bus, usb_enum_visit, &w);
    return w.first;
}

#if CONFIG_FAULTINJECT
struct usbs_racer {
    struct blkdev *bd;
    uint64_t timeouts0;
    unsigned delay_us;      /* after the layer reports the timeout: where in the recovery to land */
    int rc, rc2;            /* blk_submit's answers: the bio at delay_us, and a second one 1 ms later */
    uint8_t *buf, *buf2;    /* kmalloc'd: a bio's buffer must be DMA-able */
    struct bio bio, bio2;
    struct { volatile bool done; int status; } mk, mk2;
};

/*
 * One bio submitted from another thread `delay_us` after the layer
 * reports the timeout -- while the driver is cancelling the transfer and
 * resetting the device. The block layer queues a bio behind a pending
 * one without asking the driver, so a bio can reach the driver during
 * the recovery only if nothing was queued before it: this racer is that
 * bio, and the rounds below place it across the recovery's few
 * milliseconds. It must either wait its turn (-EAGAIN, queued by the
 * layer) or run after the recovery, and complete with the right data;
 * a driver that freed its slot when the cancelled transfer's callback
 * ran let it start on endpoints being reset (Greptile, PR #51).
 */
static void usbs_racer_main(void *arg)
{
    struct usbs_racer *r = arg;
    for (unsigned i = 0; i < 30000 && r->bd->timeouts == r->timeouts0; i++)
        thread_sleep_ns(100000);
    thread_sleep_ns((uint64_t)r->delay_us * 1000);
    struct bio *b = &r->bio;
    memset(b, 0, sizeof(*b));
    b->dev = r->bd;
    b->dir = BIO_READ;
    b->sector = 32;
    b->nsectors = 8;
    b->buf = r->buf;
    b->done = selftest_nvme_mark_done;
    b->arg = &r->mk;
    r->rc = blk_submit(b);
    if (r->rc) {
        r->mk.status = r->rc;
        r->mk.done = true;
    }
    /* A second bio a millisecond later: if the first started an exchange
     * the driver then forgot (its slot freed under it), this one would
     * reuse the exchange's request objects while they are in flight. */
    thread_sleep_ns(1000000);
    struct bio *b2 = &r->bio2;
    memset(b2, 0, sizeof(*b2));
    b2->dev = r->bd;
    b2->dir = BIO_READ;
    b2->sector = 48;
    b2->nsectors = 8;
    b2->buf = r->buf2;
    b2->done = selftest_nvme_mark_done;
    b2->arg = &r->mk2;
    r->rc2 = blk_submit(b2);
    if (r->rc2) {
        r->mk2.status = r->rc2;
        r->mk2.done = true;
    }
    thread_exit(0);
}
#endif /* CONFIG_FAULTINJECT */

/*
 * A device that stops answering: the CSW read of one exchange is put on
 * the ring but the controller is never told (fault injection, debug
 * builds), the block layer's timeout thread finds the bio overdue, the
 * driver takes the transfer back and resets the device, the bio
 * completes -ETIMEDOUT -- and the next read works, including one that
 * another thread submits while the recovery runs. Five rounds, the
 * racing bio placed at different points of the recovery.
 */
static bool disk_timeout_common(const char *name, enum fi_kind kind, const char *tag, const char **reason)
{
#if !CONFIG_FAULTINJECT
    (void)reason;
    (void)name; (void)kind; (void)tag;
    kinfo("selftest: %s: no fault injection in this build; skipping", tag);
    return true;
#else
    struct blkdev *bd = blk_find(name);
    if (bd == NULL) {
        kinfo("selftest: %s: no %s; skipping", tag, name);
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: %s: step failed at line %d (round %u)", tag, __LINE__, round); ok = false; } } while (0)
    static const unsigned delays_us[] = { 300, 800, 1500, 2500, 4000 };
    uint8_t *buf = kmalloc(4096, 0), *check = kmalloc(4096, 0);
    struct usbs_racer *racer = kzalloc(sizeof(*racer));
    uint64_t saved = bd->timeout_ns;
    uint64_t total_dt = 0;
    unsigned round = 0;
    STEP(buf != NULL && check != NULL && racer != NULL);
    if (racer) {
        racer->buf = kmalloc(4096, 0);
        racer->buf2 = kmalloc(4096, 0);
    }
    STEP(racer && racer->buf != NULL && racer->buf2 != NULL);
    for (round = 0; ok && round < ARRAY_SIZE(delays_us); round++) {
        uint64_t timeouts0 = bd->timeouts;
        racer->bd = bd;
        racer->timeouts0 = timeouts0;
        racer->delay_us = delays_us[round];
        racer->rc = racer->rc2 = -1;
        racer->mk.done = racer->mk2.done = false;
        racer->mk.status = racer->mk2.status = -1;
        bd->timeout_ns = 200ull * 1000000ull;   /* the thread checks every 500 ms: one exchange, at most ~700 ms */
        struct thread *rt = thread_create(usbs_racer_main, racer, "usbs-racer", SCHED_PRIO_DEFAULT);
        STEP(rt != NULL);
        faultinject_set(kind, 1, 1, NULL);   /* the next CSW, once */
        uint64_t t0 = clock_now_ns();
        int rc = blk_read(bd, 0, 8, buf);
        uint64_t dt = clock_now_ns() - t0;
        total_dt += dt;
        faultinject_clear(kind);
        STEP(rc == -ETIMEDOUT);
        STEP(bd->timeouts == timeouts0 + 1);
        STEP(dt < 3000ull * 1000000ull);
        if (rt)
            thread_join(rt);
        bd->timeout_ns = saved;
        for (unsigned w = 0; w < 3000 && !(racer->mk.done && racer->mk2.done); w++)
            thread_sleep_ms(1);
        if (racer->rc != 0 || !racer->mk.done || racer->mk.status != 0 || racer->rc2 != 0 || !racer->mk2.done ||
            racer->mk2.status != 0)
            kerror("selftest: %s: the bios submitted %u us into the recovery: submit %d/%d, done %d/%d, status %d/%d",
                   tag, racer->delay_us, racer->rc, racer->rc2, racer->mk.done, racer->mk2.done, racer->mk.status,
                   racer->mk2.status);
        STEP(racer->rc == 0 && racer->mk.done && racer->mk.status == 0);
        STEP(racer->rc2 == 0 && racer->mk2.done && racer->mk2.status == 0);
        /* Served correctly: the same sectors read again once everything settled. */
        STEP(blk_read(bd, 32, 8, check) == 0 && memcmp(check, racer->buf, 4096) == 0);
        STEP(blk_read(bd, 48, 8, check) == 0 && memcmp(check, racer->buf2, 4096) == 0);
        /* Recovered: the victim's sectors read too, and a write works. */
        STEP(blk_read(bd, 0, 8, buf) == 0);
        STEP(blk_write(bd, 16, 1, buf) == 0);
    }
    bd->timeout_ns = saved;
    if (ok)
        kinfo("selftest: %s: %s: %u rounds of -ETIMEDOUT (%llu ms each on average), reset recovery, reads again; a bio submitted into each recovery was served",
              tag, bd->name, round, (unsigned long long)(total_dt / 1000000 / (round ? round : 1)));
#undef STEP
    if (racer) {
        kfree(racer->buf);
        kfree(racer->buf2);
    }
    kfree(racer);
    kfree(buf);
    kfree(check);
    blkdev_put(bd);
    if (!ok) {
        *reason = "timeout test: see the log";
        return false;
    }
    return true;
#endif
}

bool selftest_usb_storage_timeout(const char **reason)
{
    return disk_timeout_common("sda", FI_USB_CSW, "usb-storage-timeout", reason);
}

bool selftest_ahci_timeout(const char **reason)
{
    return disk_timeout_common("ahci0p1", FI_AHCI_CI, "ahci-timeout", reason);
}

/*
 * The removal path the device model was built for and never met: the
 * controller driver's own disconnect path is run for the port while a
 * bio is in flight (its CSW held back by fault injection so it *is* in
 * flight), and afterwards the disk is gone, the bio has completed with
 * an error and not never, the device's release ran once -- then the
 * port is connected again and the disk is back and readable.
 */
bool selftest_usb_unplug(const char **reason)
{
    struct usb_device *udev = usb_first_device();
    if (udev == NULL) {
        kinfo("selftest: usb-unplug: no device on the usb bus; skipping");
        return true;
    }
    struct usb_hcd *hcd = udev->hcd;
    unsigned port = udev->port;
    char name[DEVICE_NAME_MAX];
    strlcpy(name, udev->dev.name, sizeof(name));
    if (hcd->ops->debug_port == NULL) {
        device_put(&udev->dev);
        kinfo("selftest: usb-unplug: the controller has no debug port hook; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: usb-unplug: step failed at line %d", __LINE__); ok = false; } } while (0)
    struct blkdev *bd = blk_find("sda");
    STEP(bd != NULL);
    uint64_t released0 = hcd->released, enumerated0 = hcd->enumerated;
    struct bus_type *bus = bus_find("usb");
    STEP(bus != NULL && device_count(bus) == 1);
    device_put(&udev->dev);   /* the walk's reference: the release must be able to run */

    struct { volatile bool done; int status; } mk = { false, 0 };
    uint8_t *buf = kmalloc(4096, 0);
    struct bio bio;
    memset(&bio, 0, sizeof(bio));
    if (bd != NULL && buf != NULL) {
#if CONFIG_FAULTINJECT
        /* A bio that stays in flight: its exchange never asks for the CSW. */
        faultinject_set(FI_USB_CSW, 1, 1, NULL);
#endif
        bio.dev = bd;
        bio.dir = BIO_READ;
        bio.sector = 0;
        bio.nsectors = 8;
        bio.buf = buf;
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        thread_sleep_ms(20);   /* the CBW and data phases run; the CSW is withheld */
    }

    /* The detach: the driver's remove, the slot, the device. */
    STEP(hcd->ops->debug_port(hcd, port, false) == 0);
#if CONFIG_FAULTINJECT
    faultinject_clear(FI_USB_CSW);
    STEP(mk.done && mk.status != 0);   /* in flight at the detach: completed, with an error, not never */
#else
    for (unsigned i = 0; i < 2000 && !mk.done; i++)
        thread_sleep_ms(1);
    STEP(mk.done);   /* without injection the bio may have finished first; it must have finished */
#endif
    struct blkdev *gone = blk_find("sda");
    STEP(gone == NULL);
    if (gone)
        blkdev_put(gone);
    STEP(device_count(bus) == 0);
    STEP(bd == NULL || blk_read(bd, 0, 1, buf) == -ENODEV);   /* a holder's reference: refused, not served */
    if (bd != NULL)
        blkdev_put(bd);   /* the test's reference: the storage driver's memory can go */
    STEP(hcd->released == released0 + 1);   /* the usb_device release ran, once */

    /* Back: the port is reset and the device enumerated again; a fresh
     * usb_device, a fresh sda. */
    STEP(hcd->ops->debug_port(hcd, port, true) == 0);
    STEP(hcd->enumerated == enumerated0 + 1);
    STEP(device_count(bus) == 1);
    struct blkdev *again = blk_find("sda");
    STEP(again != NULL);
    if (again != NULL) {
        STEP(buf && blk_read(again, 0, 8, buf) == 0);
        blkdev_put(again);
    }
    struct usb_device *udev2 = usb_first_device();
    STEP(udev2 != NULL && udev2->port == port && strcmp(udev2->dev.name, name) == 0);
    if (udev2)
        device_put(&udev2->dev);
    if (ok)
        kinfo("selftest: usb-unplug: %s detached with a bio in flight (status %d), released once, re-enumerated as %s",
              name, mk.status, name);
#undef STEP
    kfree(buf);
    if (!ok) {
        *reason = "usb-unplug: see the log";
        return false;
    }
    return true;
}

/*
 * Reports only (docs/drivers/usb/testing.md, "Benchmarks"): sequential
 * reads over every disk in the boot and writes over the USB disk, 64 KiB
 * and 4 KiB bios, one at a time through blk_read/blk_write, so the
 * figure is the path's round trip per request. The USB disk beside
 * nvme0n1 and vda in the same boot is what makes its number readable.
 * Writes stay off nvme0n1 and vda: the nvme self-test leaves a cosmofs
 * on the first that the shell's snapshot test mounts, and the second is
 * the storage tests' scratch disk.
 */
static void blk_bench_one(struct blkdev *bd, bool write, uint32_t bio_bytes, uint64_t total_bytes, uint8_t *buf)
{
    uint32_t n = bio_bytes / bd->sector_size;
    uint64_t sectors = total_bytes / bd->sector_size;
    if (n > bd->max_sectors)
        n = bd->max_sectors;
    if (sectors > bd->capacity)
        sectors = bd->capacity;
    unsigned reqs = 0;
    uint64_t t0 = clock_now_ns();
    for (uint64_t s = 0; s + n <= sectors; s += n) {
        int rc = write ? blk_write(bd, s, n, buf) : blk_read(bd, s, n, buf);
        if (rc)
            break;
        reqs++;
    }
    uint64_t dt = clock_now_ns() - t0;
    if (dt == 0)
        dt = 1;
    uint64_t bytes = (uint64_t)reqs * n * bd->sector_size;
    kinfo("selftest: blk-bench: %s: %s %u KiB bios: %u requests in %llu ms = %llu MiB/s, %llu req/s, %llu us per request",
          bd->name, write ? "write" : "read", (unsigned)(n * bd->sector_size / 1024), reqs,
          (unsigned long long)(dt / 1000000), (unsigned long long)(bytes * 1000000000ull / dt / (1024 * 1024)),
          (unsigned long long)((uint64_t)reqs * 1000000000ull / dt), (unsigned long long)(reqs ? dt / 1000 / reqs : 0));
}

/* Four threads, each reading 1 MiB in 4 KiB bios from its own region of
 * the disk, at once: what several commands in flight buy on a device --
 * the figure that decides whether NCQ is worth writing (docs/drivers/ahci/
 * testing.md, "Benchmarks"). */
struct bench_worker {
    struct blkdev *bd;
    uint64_t start;
    unsigned reqs;
    int rc;
    uint8_t *buf;
};

static void bench_worker_main(void *arg)
{
    struct bench_worker *w = arg;
    unsigned n = 4096 / w->bd->sector_size;
    for (uint64_t s = w->start; s + n <= w->start + (1u << 20) / w->bd->sector_size; s += n) {
        int rc = blk_read(w->bd, s, n, w->buf);
        if (rc) {
            w->rc = rc;
            break;
        }
        w->reqs++;
    }
    thread_exit(0);
}

static void blk_bench_concurrent(struct blkdev *bd)
{
    enum { N = 4 };
    struct bench_worker w[N];
    struct thread *th[N];
    unsigned started = 0;
    for (unsigned i = 0; i < N; i++) {
        w[i].bd = bd;
        w[i].start = (uint64_t)i * ((1u << 20) / bd->sector_size);
        w[i].reqs = 0;
        w[i].rc = 0;
        w[i].buf = kmalloc(4096, 0);
        th[i] = w[i].buf ? thread_create(bench_worker_main, &w[i], "blk-bench", SCHED_PRIO_DEFAULT) : NULL;
        if (th[i])
            started++;
    }
    uint64_t t0 = clock_now_ns();
    unsigned reqs = 0;
    int rc = 0;
    for (unsigned i = 0; i < N; i++) {
        if (th[i]) {
            thread_join(th[i]);
            reqs += w[i].reqs;
            if (w[i].rc)
                rc = w[i].rc;
        }
        kfree(w[i].buf);
    }
    uint64_t dt = clock_now_ns() - t0;
    if (dt == 0)
        dt = 1;
    kinfo("selftest: blk-bench: %s: %u threads reading 4 KiB bios at once: %u requests in %llu ms = %llu req/s%s",
          bd->name, started, reqs, (unsigned long long)(dt / 1000000), (unsigned long long)((uint64_t)reqs * 1000000000ull / dt),
          rc ? " (with errors)" : "");
}

bool selftest_blk_bench(const char **reason)
{
    (void)reason;
    static const char *const names[] = { "sda", "ahci0p1", "nvme0n1", "vda" };
    uint8_t *buf = kmalloc(65536, 0);
    if (buf == NULL)
        return true;
    for (unsigned i = 0; i < 65536; i++)
        buf[i] = (uint8_t)(i * 13 + 1);
    for (unsigned k = 0; k < ARRAY_SIZE(names); k++) {
        struct blkdev *bd = blk_find(names[k]);
        if (bd == NULL)
            continue;
        bool writes = k <= 1;   /* the USB and SATA disks only: the others carry filesystems later tests use */
        blk_bench_one(bd, false, 65536, 4u << 20, buf);
        if (writes)
            blk_bench_one(bd, true, 65536, 4u << 20, buf);
        blk_bench_one(bd, false, 4096, 1u << 20, buf);
        if (writes)
            blk_bench_one(bd, true, 4096, 1u << 20, buf);
        blk_bench_concurrent(bd);
        blkdev_put(bd);
    }
    kfree(buf);
    return true;
}

/* --- the SATA disk (docs/drivers/ahci/testing.md) ----------------------------------- */

/*
 * The AHCI driver's answer to the question the USB unit left: a port's
 * disk is a blkdev whose DMA device is the controller, and nothing else
 * -- no struct device per port. Geometry from IDENTIFY against the image
 * the harness gave it.
 */
bool selftest_ahci_identify(const char **reason)
{
    struct blkdev *bd = blk_find("ahci0p1");
    if (bd == NULL) {
        kinfo("selftest: ahci-identify: no ahci0p1; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: ahci-identify: step failed at line %d", __LINE__); ok = false; } } while (0)
    STEP(bd->sector_size == 512 && bd->capacity == 16384);
    STEP(bd->max_sectors >= 64 && bd->max_segments >= 8);
    STEP(bd->dev != NULL && bd->dev->bus == &pci_bus);   /* the controller: the DMA requester */
    STEP(bd->dev->iommu != NULL || !iommu_present());   /* in a domain when there is a unit */
    STEP(bd->ops->debug_dma != NULL && bd->ops->debug_presence != NULL && bd->ops->timeout != NULL);
    STEP(bd->nr_queues == 1);
    if (ok)
        kinfo("selftest: ahci-identify: %s: %llu sectors of %u bytes through %s (%u segments per command)", bd->name,
              (unsigned long long)bd->capacity, bd->sector_size, bd->dev->name, bd->max_segments);
#undef STEP
    blkdev_put(bd);
    if (!ok) {
        *reason = "ahci-identify: see the log";
        return false;
    }
    return true;
}

/* The disk through the block layer, as usb-storage checks its disk. */
bool selftest_ahci_io(const char **reason)
{
    struct blkdev *bd = blk_find("ahci0p1");
    if (bd == NULL) {
        kinfo("selftest: ahci-io: no ahci0p1; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: ahci-io: step failed at line %d", __LINE__); ok = false; } } while (0)
    uint64_t reads0 = bd->reads, writes0 = bd->writes, flushes0 = bd->flushes, errors0 = bd->errors;
    struct dma_stats d0, d1;
    dma_get_stats(&d0);
    uint8_t *w = kmalloc(131072, 0), *r = kmalloc(131072, 0);
    STEP(w != NULL && r != NULL);
    unsigned reads = 0, writes = 0, flushes = 0;
    if (w && r) {
        for (unsigned i = 0; i < 131072; i++)
            w[i] = (uint8_t)(i * 5 + 9);
        uint32_t n = bd->max_sectors < 256 ? bd->max_sectors : 256;   /* 128 KiB: one command */
        const uint64_t at[3] = { 0, 8192, bd->capacity - n };
        for (unsigned k = 0; k < 3 && ok; k++) {
            w[0] = (uint8_t)(0xa0 + k);
            STEP(blk_write(bd, at[k], n, w) == 0);
            writes++;
            memset(r, 0, 131072);
            STEP(blk_read(bd, at[k], n, r) == 0 && memcmp(w, r, (size_t)n * 512) == 0);
            reads++;
        }
        STEP(blk_flush(bd) == 0);
        flushes++;
        STEP(blk_read(bd, bd->capacity, 1, r) == -EINVAL);   /* refused by the layer: no command */
        STEP(blk_read(bd, 1, 1, r) == 0 && memcmp(r, w + 512, 512) == 0);
        reads++;
    }
    /* Four pages in two segments: a PRDT of two entries. */
    dma_addr_t da, db, dc;
    uint8_t *a = dma_alloc(NULL, 2 * PAGE_SIZE, &da, 0), *b = dma_alloc(NULL, 2 * PAGE_SIZE, &db, 0);
    uint8_t *flat = dma_alloc(NULL, 4 * PAGE_SIZE, &dc, DMA_ZERO);
    STEP(a && b && flat);
    if (a && b && flat) {
        for (unsigned i = 0; i < 2 * PAGE_SIZE; i++) {
            a[i] = (uint8_t)(i ^ 0x6c);
            b[i] = (uint8_t)(i ^ 0xc6);
        }
        struct bio_vec vecs[2] = { { a, (uint32_t)(2 * PAGE_SIZE) }, { b, (uint32_t)(2 * PAGE_SIZE) } };
        struct sync_marker { volatile bool done; int status; } mk = { false, 0 };
        struct bio bio;
        memset(&bio, 0, sizeof(bio));
        bio.dev = bd;
        bio.dir = BIO_WRITE;
        bio.sector = 4096;
        bio.nsectors = (uint32_t)(4 * PAGE_SIZE / 512);
        bio.vecs = vecs;
        bio.nr_vecs = 2;
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        for (unsigned i = 0; ok && i < 5000 && !mk.done; i++)
            thread_sleep_ms(1);
        STEP(mk.done && mk.status == 0);
        writes++;
        STEP(blk_read(bd, 4096, bio.nsectors, flat) == 0);
        reads++;
        STEP(memcmp(flat, a, 2 * PAGE_SIZE) == 0 && memcmp(flat + 2 * PAGE_SIZE, b, 2 * PAGE_SIZE) == 0);
    }
    if (a)
        dma_free(NULL, 2 * PAGE_SIZE, a, da);
    if (b)
        dma_free(NULL, 2 * PAGE_SIZE, b, db);
    if (flat)
        dma_free(NULL, 4 * PAGE_SIZE, flat, dc);
    kfree(w);
    kfree(r);
    STEP(bd->reads - reads0 == reads && bd->writes - writes0 == writes && bd->flushes - flushes0 == flushes);
    STEP(bd->errors == errors0);
    dma_get_stats(&d1);
    STEP(d1.maps - d0.maps == d1.unmaps - d0.unmaps);
    if (ok)
        kinfo("selftest: ahci-io: %s: %u reads, %u writes, %u flush through %s; %llu segments mapped and unmapped",
              bd->name, reads, writes, flushes, bd->dev->name, (unsigned long long)(d1.maps - d0.maps));
#undef STEP
    blkdev_put(bd);
    if (!ok) {
        *reason = "ahci-io: see the log";
        return false;
    }
    return true;
}

/*
 * The disk-gone path with a command in flight (its PxCI bit withheld by
 * fault injection, so it *is* in flight), driven through the driver's
 * own hotplug function: the bio completes -ENODEV and not never, the
 * blkdev is gone, a read through a held reference is -ENODEV; then the
 * port is probed again and a *new* blkdev appears under the same name.
 */
bool selftest_ahci_unplug(const char **reason)
{
    struct blkdev *bd = blk_find("ahci0p1");
    if (bd == NULL) {
        kinfo("selftest: ahci-unplug: no ahci0p1; skipping");
        return true;
    }
    if (bd->ops->debug_presence == NULL) {
        blkdev_put(bd);
        kinfo("selftest: ahci-unplug: no presence hook; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: ahci-unplug: step failed at line %d", __LINE__); ok = false; } } while (0)
    unsigned count0 = blk_count();
    struct { volatile bool done; int status; } mk = { false, 0 };
    uint8_t *buf = kmalloc(4096, 0);
    STEP(buf != NULL);
    struct bio bio;
    memset(&bio, 0, sizeof(bio));
    if (buf) {
#if CONFIG_FAULTINJECT
        faultinject_set(FI_AHCI_CI, 1, 1, NULL);   /* the next command never starts */
#endif
        bio.dev = bd;
        bio.dir = BIO_READ;
        bio.sector = 0;
        bio.nsectors = 8;
        bio.buf = buf;
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        thread_sleep_ms(20);
    }
    STEP(bd->ops->debug_presence(bd, false) == 0);
#if CONFIG_FAULTINJECT
    faultinject_clear(FI_AHCI_CI);
    STEP(mk.done && mk.status == -ENODEV);   /* in flight at the detach: completed, with the right error */
#else
    for (unsigned i = 0; i < 2000 && !mk.done; i++)
        thread_sleep_ms(1);
    STEP(mk.done);
#endif
    struct blkdev *gone = blk_find("ahci0p1");
    STEP(gone == NULL);
    if (gone)
        blkdev_put(gone);
    STEP(blk_count() == count0 - 1);
    STEP(buf && blk_read(bd, 0, 1, buf) == -ENODEV);   /* a holder's reference: refused, not served */

    /* The disk is still physically there: probe the port through the old
     * object -- it only names the port -- and a new blkdev appears. */
    STEP(bd->ops->debug_presence(bd, true) == 0);
    struct blkdev *again = blk_find("ahci0p1");
    STEP(again != NULL && again != bd);
    STEP(blk_count() == count0);
    if (again) {
        STEP(buf && blk_read(again, 0, 8, buf) == 0);
        STEP(again->capacity == bd->capacity && again->sector_size == bd->sector_size);
        blkdev_put(again);
    }
    blkdev_put(bd);   /* the old object's last holder: its release runs now */
    if (ok)
        kinfo("selftest: ahci-unplug: ahci0p1 detached with a command in flight (status %d), a new blkdev after the probe",
              mk.status);
#undef STEP
    kfree(buf);
    if (!ok) {
        *reason = "ahci-unplug: see the log";
        return false;
    }
    return true;
}

/*
 * A COMRESET with a command in flight (withheld PxCI, as above): the
 * command completes -EIO and not never, the same disk answers IDENTIFY
 * and the same blkdev keeps serving -- the recovery an error that needs
 * a link reset goes through.
 */
bool selftest_ahci_reset(const char **reason)
{
    struct blkdev *bd = blk_find("ahci0p1");
    if (bd == NULL) {
        kinfo("selftest: ahci-reset: no ahci0p1; skipping");
        return true;
    }
    if (bd->ops->debug_presence == NULL) {
        blkdev_put(bd);
        kinfo("selftest: ahci-reset: no presence hook; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: ahci-reset: step failed at line %d", __LINE__); ok = false; } } while (0)
    struct { volatile bool done; int status; } mk = { false, 0 };
    uint8_t *buf = kmalloc(4096, 0);
    STEP(buf != NULL);
    struct bio bio;
    memset(&bio, 0, sizeof(bio));
    uint64_t errors0 = bd->errors;
    if (buf) {
#if CONFIG_FAULTINJECT
        faultinject_set(FI_AHCI_CI, 1, 1, NULL);
#endif
        bio.dev = bd;
        bio.dir = BIO_READ;
        bio.sector = 16;
        bio.nsectors = 8;
        bio.buf = buf;
        bio.done = selftest_nvme_mark_done;
        bio.arg = &mk;
        STEP(blk_submit(&bio) == 0);
        thread_sleep_ms(20);
    }
    STEP(bd->ops->debug_presence(bd, true) == 0);   /* live disk: reset the link, re-identify, keep the blkdev */
#if CONFIG_FAULTINJECT
    faultinject_clear(FI_AHCI_CI);
    STEP(mk.done && mk.status == -EIO);
#else
    for (unsigned i = 0; i < 2000 && !mk.done; i++)
        thread_sleep_ms(1);
    STEP(mk.done);
#endif
    struct blkdev *same = blk_find("ahci0p1");
    STEP(same == bd);
    if (same)
        blkdev_put(same);
    STEP(buf && blk_read(bd, 16, 8, buf) == 0 && blk_write(bd, 24, 1, buf) == 0);
    STEP(bd->errors >= errors0);
    if (ok)
        kinfo("selftest: ahci-reset: %s reset with a command in flight (status %d); the same disk, the same blkdev, reads again",
              bd->name, mk.status);
#undef STEP
    kfree(buf);
    blkdev_put(bd);
    if (!ok) {
        *reason = "ahci-reset: see the log";
        return false;
    }
    return true;
}
