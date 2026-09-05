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
#include <kernel/pmm.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/cosmofs.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include <uapi/cosmo/syscall.h>

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
