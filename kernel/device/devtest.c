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
#include <kernel/quiesce.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/cosmofs.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include <uapi/cosmo/syscall.h>

#include <arch/cpu.h>

#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/virtio.h>

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

/* Registered once, by whichever test runs first: the model panics on a
 * second registration of the same name, and two tests now use it --
 * which is how the second one panicked the machine, each holding its
 * own `registered` flag. */
static void ensure_fake_bus(void)
{
    static bool registered;
    if (!registered) {
        bus_register(&fake_bus);
        registered = true;
    }
}
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
    ensure_fake_bus();
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

#if CONFIG_DEBUG
/* Local twins of the quiesce suite's helpers: this file has its own
 * CHECK and cannot share statics across translation units. Debug only,
 * with the tests that use them. */
static unsigned other_cpu_for_blk(void)
{
    for (unsigned c = 1; c < cpu_count(); c++)
        if (cpu_online(c))
            return c;
    return 0;
}

/* A second CPU that is neither 0 nor `avoid`, for a test that needs two
 * threads to make progress independently; `avoid` again when the machine
 * has only the one. */
static unsigned other_cpu_than(unsigned avoid)
{
    for (unsigned c = 1; c < cpu_count(); c++)
        if (c != avoid && cpu_online(c))
            return c;
    return avoid;
}

static bool wait_flag_blk(const volatile unsigned *flag, unsigned ms)
{
    uint64_t end = clock_now_ns() + (uint64_t)ms * 1000000ULL;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
        if (clock_now_ns() > end)
            return false;
        arch_cpu_relax();
    }
    return true;
}

static bool threads_settle_blk(unsigned expected)
{
    uint64_t deadline = clock_now_ns() + 500ULL * 1000000ULL;
    while (thread_count() != expected) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* --- removing a device that is busy ---------------------------------------
 *
 * docs/audit/next-subsystem-lifetime-windows.md. What this test drives is
 * the unbind *transition*, on a synthetic device of its own.
 *
 * It used to say that `vpci_remove` with real I/O outstanding was out of
 * reach, because the machine's only virtio-blk was the scratch disk the
 * filesystem tests run on. The machine has a second one now, attached to
 * be removed, and `virtio-remove-inflight` below does exactly that
 * (docs/audit/next-subsystem-virtio-remove-inflight.md). This test keeps
 * its own subject: the model's bookkeeping, on a device whose driver is
 * two functions long, where the assertion is exact.
 *
 * What is testable here, and is the half the review found missing, is
 * that removal is the *whole* unbind and not the driver's hook: the hook
 * alone leaves the bound count, `driver` and `drvdata` untouched, and a
 * driver that frees what `drvdata` points at -- which the virtio one
 * does -- leaves a bound device holding a dangling pointer.
 */

static unsigned busy_removes;
static unsigned busy_inflight;      /* work outstanding when remove ran */
static void *busy_drvdata;          /* what the driver allocated */

static int busy_probe(struct device *dev)
{
    busy_drvdata = kzalloc(64);
    if (busy_drvdata == NULL)
        return -ENOMEM;
    dev->drvdata = busy_drvdata;
    return 0;
}

/* Like vpci_remove: it frees the object drvdata names. Anything that
 * leaves `drvdata` pointing here afterwards has made a dangling
 * pointer. */
static void busy_remove(struct device *dev)
{
    busy_removes++;
    busy_inflight = __atomic_load_n(&busy_inflight, __ATOMIC_ACQUIRE);
    kfree(dev->drvdata);
    busy_drvdata = NULL;
}

#endif /* CONFIG_DEBUG: the helpers above */

bool selftest_device_remove_busy(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: device-remove-busy: no test hooks in this build; skipping");
    return true;
#else
    ensure_fake_bus();
    static struct device d;
    static struct device_driver drv = { .name = "busy", .match_data = "busy0", .probe = busy_probe,
                                        .remove = busy_remove };
    drv.bus = &fake_bus;
    busy_removes = 0;
    busy_drvdata = NULL;
    __atomic_store_n(&busy_inflight, 3u, __ATOMIC_RELEASE);   /* work outstanding across the removal */

    device_setup(&d, &fake_bus, NULL, "busy0");
    d.release = device_release_static;
    CHECK(device_register(&d) == 0);
    CHECK(driver_register(&drv) == 0);
    CHECK(d.driver == &drv && d.drvdata != NULL && drv.bound == 1);
    CHECK(d.state == DEV_BOUND);

    /* The removal, with work outstanding. */
    device_test_unbind(&d);

    /* The driver's hook ran once... */
    CHECK(busy_removes == 1);
    CHECK(busy_inflight == 3);          /* it saw the work, it did not wait for it */
    /* ...and the bookkeeping went with it, which is the half a bare hook
     * would have skipped. A device left bound here with drvdata still
     * naming freed memory is what a later unregister would remove a
     * second time. */
    CHECK(d.driver == NULL);
    CHECK(d.drvdata == NULL);
    CHECK(d.state == DEV_UNBOUND);
    CHECK(drv.bound == 0);
    CHECK(busy_drvdata == NULL);

    /* Unbinding again is a no-op rather than a second removal. */
    device_test_unbind(&d);
    CHECK(busy_removes == 1);

    driver_unregister(&drv);
    device_unregister(&d);
    kinfo("selftest: device-remove-busy: the removal took the driver's hook and the model's bookkeeping together, "
          "with %u unit(s) of work outstanding",
          busy_inflight);
    return true;
#endif
}

/* --- the unregister barrier, raced ---------------------------------------
 *
 * docs/audit/next-subsystem-lifetime-windows.md. blk_unregister sets
 * `gone` and then waits for `submitting` to fall, and the comment beside
 * it states a Dekker argument: a submitter that did not see `gone` has
 * raised `submitting` before the unregister reads it, or the unregister
 * saw the increment. Until these tests, blk_unregister had been called
 * by two tests and in both the device was quiescent -- nothing had ever
 * submitted to a device while it was being unregistered, which is the
 * only circumstance the barrier exists for.
 */

#if CONFIG_DEBUG
struct race_blk {
    struct blkdev bd;
    unsigned submits;           /* reached the driver */
    unsigned releases;
};

static int race_blk_submit(struct blkdev *bd, struct bio *bio)
{
    struct race_blk *f = bd->priv;
    __atomic_fetch_add(&f->submits, 1u, __ATOMIC_ACQ_REL);
    bio_complete(bio, 0);
    return 0;
}

static void race_blk_release(struct blkdev *bd)
{
    struct race_blk *f = bd->priv;
    f->releases++;
}

struct submitter {
    struct blkdev *bd;
    void *buf;
    unsigned n;                 /* attempts */
    volatile unsigned started;
    volatile unsigned ok;       /* accepted */
    volatile unsigned refused;  /* -ENODEV */
    volatile unsigned other;    /* anything else: must stay zero */
    volatile unsigned completions;
    volatile unsigned stop;
};

static void race_bio_done(struct bio *bio)
{
    struct submitter *s = bio->arg;
    if (s)
        __atomic_fetch_add(&s->completions, 1u, __ATOMIC_ACQ_REL);
}

static void submitter_main(void *arg)
{
    struct submitter *s = arg;
    __atomic_store_n(&s->started, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct bio bio = { .dev = s->bd,   .sector = 0,   .nsectors = 1, .dir = BIO_READ,
                           .buf = s->buf,  .done = race_bio_done, .arg = s };
        int rc = blk_submit(&bio);
        s->n++;
        if (rc == 0)
            __atomic_fetch_add(&s->ok, 1u, __ATOMIC_ACQ_REL);
        else if (rc == -ENODEV)
            __atomic_fetch_add(&s->refused, 1u, __ATOMIC_ACQ_REL);
        else
            __atomic_fetch_add(&s->other, 1u, __ATOMIC_ACQ_REL);
    }
}

/*
 * The refusal half: submitters hammering a device while it is being
 * unregistered, with the window between the `gone` store and the
 * `submitting` load held open.
 *
 * The claim is not "some were refused" -- that is true of a test that
 * ran entirely after the unregister -- but that the window was *crossed*:
 * some accepted and some refused, every bio completed exactly once, and
 * nothing reached the driver after the unregister returned.
 */
bool selftest_blk_submit_unregister(const char **reason)
{
#if !CONFIG_DEBUG
    /* The hook that holds the window open is a debug-build thing, and a
     * race test without it would be a race test hoping. */
    (void)reason;
    kinfo("selftest: blk-submit-unregister: no test hooks in this build; skipping");
    return true;
#else
    unsigned threads0 = thread_count();
    unsigned cpu = other_cpu_for_blk();
    if (cpu == 0) {
        kinfo("selftest: blk-submit-unregister: one CPU, no submitter to race");
        return true;
    }

    static struct race_blk f;
    static const struct blkdev_ops ops = { .submit = race_blk_submit, .release = race_blk_release };
    memset(&f, 0, sizeof(f));
    f.bd.ops = &ops;
    f.bd.sector_size = 512;
    f.bd.capacity = 8;
    f.bd.max_sectors = 8;
    f.bd.priv = &f;
    CHECK(blk_register(&f.bd, "rz") == 0);

    void *buf = kmalloc(512, 0);
    CHECK(buf != NULL);
    struct submitter s = { .bd = &f.bd, .buf = buf };
    struct thread *t = thread_create_on(submitter_main, &s, "blkrace", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    CHECK(wait_flag_blk(&s.started, 1000));

    /* Let it land some accepted submissions first, so "crossed the
     * window" is a fact and not an accident of scheduling. */
    while (__atomic_load_n(&s.ok, __ATOMIC_ACQUIRE) < 4)
        sched_yield();

    blk_test_unregister_pause(20);
    blk_unregister(&f.bd);
    blk_test_unregister_pause(0);
    unsigned reached_at_return = __atomic_load_n(&f.submits, __ATOMIC_ACQUIRE);

    /* Keep submitting for a while *after* the unregister returned: every
     * one of these must be refused, and none may reach the driver. */
    while (__atomic_load_n(&s.refused, __ATOMIC_ACQUIRE) < 4)
        sched_yield();
    __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t);

    CHECK(__atomic_load_n(&f.submits, __ATOMIC_ACQUIRE) == reached_at_return);   /* the claim */
    CHECK(s.other == 0);                        /* only 0 or -ENODEV, nothing else */
    CHECK(s.ok > 0 && s.refused > 0);           /* the window was crossed, not stepped over */
    CHECK(s.completions == s.ok);               /* each accepted bio completed exactly once */
    kfree(buf);
    blkdev_put(&f.bd);                          /* the creator's reference: now it may go */
    CHECK(f.releases == 1);
    CHECK(threads_settle_blk(threads0));

    kinfo("selftest: blk-submit-unregister: %u accepted and %u refused across the window; nothing reached the driver "
          "after unregister returned",
          s.ok, s.refused);
    return true;
#endif
}

struct releaser {
    volatile unsigned stop;
};

/* Releases the parked submitter, but only once the unregister is
 * demonstrably draining -- the spin counter moving is what says so. A
 * release on a timer would let the submitter leave before the unregister
 * ever looked, and the test would pass having raced nothing. */
static void releaser_main(void *arg)
{
    struct releaser *r = arg;
    while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
        if (blk_test_unregister_spins() > 0) {
            blk_test_release_in_driver();
            return;
        }
        sched_yield();
    }
}
#endif /* CONFIG_DEBUG */

/*
 * The drain half: a submitter parked *inside* the window, past the
 * `gone` check with `submitting` raised, and an unregister that must not
 * return until it leaves.
 *
 * The assertion is an order of two events, not a timing: the submitter
 * stamps the moment before it lowers `submitting`, blk_unregister stamps
 * the moment it returns, and the second must be after the first.
 *
 * Which property that rests on is worth naming, because it changed. The
 * two events happen on different CPUs -- the submitter is pinned away
 * from the unregister deliberately -- and the stamps used to be two
 * `clock_now_ns()` readings. Comparing two CPUs' readings is precisely
 * what this kernel stopped promising when it began reading the
 * invariant-TSC bit: on x86-64 under QEMU `clock_is_common()` is false,
 * so the assertion rested on a guarantee the kernel declines to give and
 * passed only because the emulator's counters agree. The stamps are
 * positions in an atomic sequence now
 * (`blk_test_drain_ordered`), which orders the two events on any machine
 * and depends on no clock at all
 * (docs/audit/next-subsystem-cpu-clock.md, step 5).
 */
bool selftest_blk_unregister_drain(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: blk-unregister-drain: no test hooks in this build; skipping");
    return true;
#else
    unsigned threads0 = thread_count();
    unsigned cpu = other_cpu_for_blk();
    if (cpu == 0) {
        kinfo("selftest: blk-unregister-drain: one CPU, nothing to park");
        return true;
    }

    static struct race_blk f;
    static const struct blkdev_ops ops = { .submit = race_blk_submit, .release = race_blk_release };
    memset(&f, 0, sizeof(f));
    f.bd.ops = &ops;
    f.bd.sector_size = 512;
    f.bd.capacity = 8;
    f.bd.max_sectors = 8;
    f.bd.priv = &f;
    CHECK(blk_register(&f.bd, "dz") == 0);

    void *buf = kmalloc(512, 0);
    CHECK(buf != NULL);
    struct submitter s = { .bd = &f.bd, .buf = buf };
    struct releaser rel = { 0 };

    blk_test_hold_in_driver(true);
    struct thread *t = thread_create_on(submitter_main, &s, "blkpark", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    /* A submitter is inside the window: this is the positive
     * synchronisation, not an assumption about scheduling. */
    uint64_t deadline = clock_now_ns() + 2ULL * 1000000000ULL;
    while (!blk_test_submitter_parked()) {
        CHECK(clock_now_ns() < deadline);
        sched_yield();
    }

    struct thread *rt = thread_create_on(releaser_main, &rel, "blkrel", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(rt != NULL);

    blk_unregister(&f.bd);
    __atomic_store_n(&rel.stop, 1u, __ATOMIC_RELEASE);

    /* It really waited, and it returned after the submitter left. */
    unsigned spins = blk_test_unregister_spins();
    CHECK(spins > 0);
    CHECK(blk_test_drain_ordered());

    __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t);
    thread_join(rt);
    blk_test_hold_in_driver(false);
    kfree(buf);
    blkdev_put(&f.bd);                          /* the creator's reference: now it may go */
    CHECK(f.releases == 1);
    CHECK(threads_settle_blk(threads0));

    kinfo("selftest: blk-unregister-drain: the unregister spun %u time(s) for a submitter inside the driver and "
          "returned after it left",
          spins);
    return true;
#endif
}

/* --- a live virtio device removed with I/O outstanding ---------------------
 *
 * docs/audit/next-subsystem-virtio-remove-inflight.md. The window the
 * lifetime-windows unit named and could not reach: `vpci_remove`, which
 * only module unload had ever run, with requests at the device. The test
 * machine now carries a virtio-blk that exists to be removed (4 MiB, the
 * only disk of that size: vdb on q35, vdc on virt, found by capacity
 * because its name differs), and this test removes it while a submitter
 * on another CPU keeps going, then brings it back.
 *
 * The lifetime unit's three steps. Hold the window open by construction:
 * a hook leaves the device's finished requests unconsumed in the driver's
 * slot table, so the remove finds them rather than racing a device that
 * answers in microseconds. Drive the other side from a real second CPU.
 * Assert the protected object: every accepted bio completes exactly
 * once, with one of three statuses -- 0 (done before the hold), -EIO (a
 * slot the remove found, completed by its leftover walk), -ENODEV (queued
 * on the block layer's pending list behind a full table, completed by
 * blk_unregister) -- and the -EIO count is exactly what the remove found;
 * nothing completes after the remove's own boundary stamp; the disk, the
 * virtio device and the binding are gone; the release runs on the last
 * put. Then the rebind: the disk comes back under its old name and reads
 * back the sector written before the removal, which is the assertion
 * that the removal left the hardware sane, and the reason the machine is
 * as this test found it when the next test runs.
 */

#if CONFIG_DEBUG
#define RM_POOL 96u   /* more bios than the driver has slots, so the pending list is exercised */
#define RM_CAPACITY 8192u   /* the removal disk's sectors: 4 MiB, and no other disk's size */

static const struct blk_test_driver_hooks *g_rm;   /* the driver's seams, published at its module init */

/*
 * This test removes a device from the machine, so a failure in the
 * middle must not leave the machine worse than it found it: the
 * submitter has to be stopped and joined, the driver's hooks cleared and
 * the function re-bound, or every test after this one runs on a machine
 * missing a disk and with a thread hammering it. CHECK returns at once,
 * which is right everywhere else in this file and wrong here, so these
 * two functions use a variant that jumps to their cleanup.
 */
#define RM_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            ok = false;                                                        \
            goto out;                                                          \
        }                                                                      \
    } while (0)

struct rm_bio {
    struct bio bio;
    volatile unsigned busy;
};

struct rm_submitter {
    struct blkdev *bd;
    void *buf;
    struct rm_bio pool[RM_POOL];
    volatile unsigned started, stop;
    volatile unsigned ok, refused, other;          /* blk_submit's answers */
    volatile unsigned c_ok, c_eio, c_enodev, c_other, c_double;   /* completions by status */
    volatile uint64_t max_seq;                     /* the latest completion's stamp */
    struct blkdev *volatile rehold;                /* `held-inside`: store the hold from the next completion */
};

static void rm_done(struct bio *bio)
{
    struct rm_bio *rb = container_of(bio, struct rm_bio, bio);
    struct rm_submitter *s = bio->arg;
    /* The `held-inside` pass: its holds are stored from here, inside
     * the handler's loop, which is the moment that pass is about. */
    struct blkdev *rh = __atomic_exchange_n(&s->rehold, NULL, __ATOMIC_ACQ_REL);
    if (rh != NULL)
        g_rm->hold_completions(rh);
    uint64_t seq = blk_test_tick();
    uint64_t seen = __atomic_load_n(&s->max_seq, __ATOMIC_ACQUIRE);
    while (seq > seen && !__atomic_compare_exchange_n(&s->max_seq, &seen, seq, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
    switch (bio->status) {
    case 0:       __atomic_fetch_add(&s->c_ok, 1u, __ATOMIC_ACQ_REL); break;
    case -EIO:    __atomic_fetch_add(&s->c_eio, 1u, __ATOMIC_ACQ_REL); break;
    case -ENODEV: __atomic_fetch_add(&s->c_enodev, 1u, __ATOMIC_ACQ_REL); break;
    default:      __atomic_fetch_add(&s->c_other, 1u, __ATOMIC_ACQ_REL); break;
    }
    /* Exactly once: a second completion of a bio that is not in flight
     * is the double the leftover walk must never produce. */
    if (__atomic_exchange_n(&rb->busy, 0u, __ATOMIC_ACQ_REL) == 0)
        __atomic_fetch_add(&s->c_double, 1u, __ATOMIC_ACQ_REL);
}

static void rm_submitter_main(void *arg)
{
    struct rm_submitter *s = arg;
    __atomic_store_n(&s->started, 1u, __ATOMIC_RELEASE);
    unsigned next = 0;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct rm_bio *rb = &s->pool[next];
        next = (next + 1) % RM_POOL;
        if (__atomic_load_n(&rb->busy, __ATOMIC_ACQUIRE)) {
            sched_yield();   /* the pool is all in flight: that is the full table the test wants */
            continue;
        }
        rb->bio = (struct bio){ .dev = s->bd, .sector = 1, .nsectors = 1, .dir = BIO_READ,
                                .buf = s->buf, .done = rm_done, .arg = s };
        __atomic_store_n(&rb->busy, 1u, __ATOMIC_RELEASE);
        int rc = blk_submit(&rb->bio);
        if (rc == 0) {
            __atomic_fetch_add(&s->ok, 1u, __ATOMIC_ACQ_REL);
        } else {
            __atomic_store_n(&rb->busy, 0u, __ATOMIC_RELEASE);   /* refused: never completes */
            if (rc == -ENODEV)
                __atomic_fetch_add(&s->refused, 1u, __ATOMIC_ACQ_REL);
            else
                __atomic_fetch_add(&s->other, 1u, __ATOMIC_ACQ_REL);
            sched_yield();   /* the device is gone: spinning on it measures this CPU, not the window */
        }
    }
}

static unsigned rm_completions(const struct rm_submitter *s)
{
    return __atomic_load_n(&s->c_ok, __ATOMIC_ACQUIRE) + __atomic_load_n(&s->c_eio, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&s->c_enodev, __ATOMIC_ACQUIRE) + __atomic_load_n(&s->c_other, __ATOMIC_ACQUIRE);
}

static bool rm_wait_completions(const struct rm_submitter *s, unsigned n)
{
    uint64_t end = clock_now_ns() + 2000ull * 1000000ull;
    while (rm_completions(s) < n) {
        if (clock_now_ns() > end)
            return false;
        sched_yield();
    }
    return true;
}

/*
 * A pass leaving with I/O accepted -- a check failed before the remove,
 * with the hold set -- must not free the buffer the device reads into
 * while a request can still complete into it. The hold is released
 * first, so what the device has finished is consumed; then every
 * accepted bio is waited for, and one that never completes leaves its
 * buffer allocated: the storage is spoken for (`rm_io`'s rule). After a
 * pass that ran to its end this is a no-op. Found in review.
 */
static void rm_drain(struct rm_submitter *s)
{
    g_rm->hold_completions(NULL);
    unsigned accepted = __atomic_load_n(&s->ok, __ATOMIC_ACQUIRE);
    if (rm_completions(s) < accepted && !rm_wait_completions(s, accepted)) {
        kerror("selftest: virtio-remove-inflight: %u of %u accepted bios never completed; their buffer stays allocated",
               accepted - rm_completions(s), accepted);
        s->buf = NULL;
    }
}

/* The removal disk, by its size: the names differ between the machines. */
static struct blkdev *rm_find(void)
{
    static const char *const names[] = { "vdb", "vdc", "vdd", "vde" };
    for (unsigned k = 0; k < ARRAY_SIZE(names); k++) {
        struct blkdev *bd = blk_find(names[k]);
        if (bd == NULL)
            continue;
        if (bd->capacity == RM_CAPACITY && !bd->read_only && bd->sector_size == 512)
            return bd;
        blkdev_put(bd);
    }
    return NULL;
}

/* A synchronous read or write of one sector, for the before-and-after. */
/*
 * The bio and the word its completion writes are **static**, not stack
 * objects, and that is not a style choice: the block layer owns an
 * accepted bio until `bio_complete`, and the wait below gives up after
 * two seconds. A stack bio would leave the driver holding a pointer
 * into a frame that has returned, and a late completion would write
 * through it -- found in review, and the only reason the rest of this
 * test was safe is that its submitter already used a static pool. The
 * `busy` flag is the other half: if a request never completed, its
 * storage is still spoken for and reusing it would be the same bug a
 * second time, so the next call refuses instead.
 */
static struct rm_sync { volatile unsigned done, busy; } rm_sync_state;
static struct bio rm_sync_bio;
static void rm_sync_done(struct bio *bio)
{
    struct rm_sync *w = bio->arg;
    __atomic_store_n(&w->busy, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&w->done, 1u, __ATOMIC_RELEASE);
}
static int rm_io(struct blkdev *bd, enum bio_dir dir, void *buf)
{
    if (__atomic_load_n(&rm_sync_state.busy, __ATOMIC_ACQUIRE))
        return -EBUSY;   /* an earlier request never completed: its storage is still the driver's */
    __atomic_store_n(&rm_sync_state.done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&rm_sync_state.busy, 1u, __ATOMIC_RELEASE);
    rm_sync_bio = (struct bio){ .dev = bd, .sector = 0, .nsectors = 1, .dir = dir, .buf = buf,
                                .done = rm_sync_done, .arg = &rm_sync_state };
    int rc = blk_submit(&rm_sync_bio);
    if (rc) {
        __atomic_store_n(&rm_sync_state.busy, 0u, __ATOMIC_RELEASE);   /* refused: never accepted, never completes */
        return rc;
    }
    if (!wait_flag_blk(&rm_sync_state.done, 2000))
        return -ETIMEDOUT;   /* `busy` stays raised: the storage is not reusable until it completes */
    return rm_sync_bio.status;
}

/* One pass: held (the window occupied by construction) or not (the
 * natural race, a regression guard). Returns false on a failed check. */
static bool rm_pass(struct blkdev *bd, struct pci_device *pdev, bool held, unsigned threads0, const char **reason)
{
    static struct rm_submitter s;
    bool ok = true;
    struct thread *t = NULL;
    memset(&s, 0, sizeof(s));
    s.bd = bd;
    s.buf = kmalloc(4096, 0);
    RM_CHECK(s.buf != NULL);
    unsigned nr_slots = g_rm->nr_slots(bd);
    unsigned cpu = other_cpu_for_blk();

    t = thread_create_on(rm_submitter_main, &s, "vrm", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    RM_CHECK(t != NULL);
    RM_CHECK(wait_flag_blk(&s.started, 1000));

    /* Live, not merely present: four accepted and completed. */
    uint64_t end = clock_now_ns() + 2000ull * 1000000ull;
    while ((s.c_ok < 4 || s.ok < 4) && clock_now_ns() < end)
        sched_yield();
    RM_CHECK(s.c_ok >= 4);

    if (held) {
        /* Nothing completes from here: accepted minus completed grows
         * until the driver's table is full and the excess is pending. */
        g_rm->hold_completions(bd);
        end = clock_now_ns() + 2000ull * 1000000ull;
        while (s.ok - rm_completions(&s) <= nr_slots && clock_now_ns() < end)
            sched_yield();
        RM_CHECK(s.ok - rm_completions(&s) > nr_slots);
    }

    unsigned releases0 = g_rm->releases();
    RM_CHECK(pci_test_remove(pdev) == 0);   /* the removal stamps its own end */
    uint64_t boundary = g_rm->remove_seq();
    unsigned found = g_rm->inflight_at_remove();
    g_rm->hold_completions(NULL);

    /* Keep submitting after the remove: refused, and nothing completes. */
    end = clock_now_ns() + 2000ull * 1000000ull;
    while (s.refused < 4 && clock_now_ns() < end)
        sched_yield();
    __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t);
    t = NULL;

    /* The protected object. */
    RM_CHECK(s.other == 0);                             /* blk_submit answered 0 or -ENODEV, nothing else */
    RM_CHECK(s.refused >= 4);
    RM_CHECK(rm_completions(&s) == s.ok);               /* every accepted bio completed... */
    RM_CHECK(s.c_double == 0);                          /* ...exactly once */
    RM_CHECK(s.c_other == 0);                           /* with 0, -EIO or -ENODEV and nothing else */
    RM_CHECK(s.c_eio == found);                         /* the remove completed precisely the slots it held */
    if (held)
        RM_CHECK(found >= 1);                           /* the window was occupied */
    RM_CHECK(boundary != 0 && s.max_seq < boundary);    /* nothing completed after the driver was done removing */
    RM_CHECK(pdev->dev.driver == NULL && pdev->dev.drvdata == NULL && pdev->dev.state == DEV_UNBOUND);
    RM_CHECK(rm_find() == NULL);                        /* the disk is gone from the registry */
    RM_CHECK(g_rm->releases() == releases0);            /* and not yet released: this test still holds it */
    blkdev_put(bd);                                     /* the last holder */
    bd = NULL;
    RM_CHECK(g_rm->releases() == releases0 + 1);        /* now it is */
    RM_CHECK(threads_settle_blk(threads0));

    /*
     * The `found` figure is the measurement, and the two passes are what
     * make it one. Held, the driver's table is full by construction and
     * the remove has real work to do; unheld, a QEMU device answers in
     * microseconds and the remove reliably finds **nothing** -- which is
     * why the hook exists and why the unheld pass is labelled a
     * regression guard rather than a proof (the report's own words).
     */
    kinfo("selftest: virtio-remove-inflight: %s: %u accepted, %u refused; %u found in flight at the remove, "
          "%u completed -EIO, %u -ENODEV, %u ok; nothing after the boundary",
          held ? "held" : "unheld", s.ok, s.refused, found, s.c_eio, s.c_enodev, s.c_ok);

out:
    if (t != NULL) {
        __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
        thread_join(t);
    }
    rm_drain(&s);
    if (bd != NULL)
        blkdev_put(bd);
    kfree(s.buf);
    return ok;
}

/*
 * The third pass is `rm_irq_order_pass` below, which carries its own
 * account: the removal's teardown order, which is what the defect was.
 */
struct rm_remover {
    struct pci_device *pdev;
    volatile unsigned ready, go, done;
    volatile int rc;
};

/*
 * The removal runs on a thread of its own so that the test thread can
 * hold a read-side section across it without the two contending for one
 * CPU.
 */
static void rm_remover_main(void *arg)
{
    struct rm_remover *rm = arg;
    __atomic_store_n(&rm->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&rm->go, __ATOMIC_ACQUIRE))
        sched_yield();
    rm->rc = pci_test_remove(rm->pdev);
    __atomic_store_n(&rm->done, 1u, __ATOMIC_RELEASE);
}

/*
 * The third pass: the removal's teardown order, which is what the
 * defect was. `vblk_remove` must release the queue's interrupt --
 * `virtq_free` masks the MSI-X entry and `synchronize_irq`s it -- before
 * it reads and clears the slot table that `vblk_done` also reads and
 * clears, and before it frees the ring a handler would be walking. The
 * kernel's own contract makes that sufficient: a handler is a quiesce
 * read-side section, so a grace period after unregistration proves no
 * CPU is inside one (`kernel/include/kernel/interrupt.h`).
 *
 * So the adversary is a read-side section, held by this test across the
 * removal's teardown, and the evidence is three stamps from one
 * sequence: the removal enters the teardown, this section ends, the
 * removal starts its walk. In that order, and it cannot be otherwise
 * unless the teardown has stopped waiting -- or has moved back after
 * the walk, which is the mutation.
 *
 * A *thread* holds the section rather than a parked interrupt handler.
 * An earlier version parked a real completion walk, which meant
 * spinning in interrupt context on cpu0 (where every MSI-X vector
 * lands) for as long as the removal took: a CPU that cannot take an
 * interrupt cannot answer a TLB shootdown (one second) and stops
 * ticking for the lockup detectors, and `lockup-hard` failed next to
 * it. A preemption-disabled section is the same thing to
 * `synchronize_quiesce` and none of those things to the machine.
 */
struct rm_holder {
    volatile unsigned held, stop;
    volatile uint64_t exit_seq;
};

#define RM_HOLD_NS (60ull * 1000000ull)   /* long enough for a teardown, short enough to be polite */

static void rm_holder_main(void *arg)
{
    struct rm_holder *h = arg;
    uint64_t end = clock_now_ns() + RM_HOLD_NS;
    quiesce_read_lock();
    __atomic_store_n(&h->held, 1u, __ATOMIC_RELEASE);
    /* Must not block while holding it; spinning is what a read-side
     * section is allowed to do, and preemption is merely disabled, so
     * this CPU still ticks and still answers its IPIs. */
    while (clock_now_ns() < end && !__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    __atomic_store_n(&h->exit_seq, blk_test_tick(), __ATOMIC_RELEASE);
    quiesce_read_unlock();
}

static bool rm_irq_order_pass(struct blkdev *bd, struct pci_device *pdev, unsigned threads0, bool *caught,
                              const char **reason)
{
    static struct rm_submitter s;
    static struct rm_remover rm;
    static struct rm_holder h;
    bool ok = true;
    struct thread *t = NULL, *rt = NULL, *ht = NULL;
    *caught = false;
    memset(&s, 0, sizeof(s));
    memset(&rm, 0, sizeof(rm));
    memset(&h, 0, sizeof(h));
    s.bd = bd;
    rm.pdev = pdev;
    s.buf = kmalloc(4096, 0);
    RM_CHECK(s.buf != NULL);
    unsigned cpu = other_cpu_for_blk();

    t = thread_create_on(rm_submitter_main, &s, "vrm", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    RM_CHECK(t != NULL);
    RM_CHECK(wait_flag_blk(&s.started, 1000));
    uint64_t end = clock_now_ns() + 2000ull * 1000000ull;
    while ((s.c_ok < 4 || s.ok < 4) && clock_now_ns() < end)
        sched_yield();
    RM_CHECK(s.c_ok >= 4);

    g_rm->stamps_reset();
    /* Both on CPUs of their own: the holder spins, and the removal must
     * not be behind it in a run queue. */
    ht = thread_create_on(rm_holder_main, &h, "vrmhold", SCHED_PRIO_DEFAULT, CPUMASK_OF(other_cpu_than(cpu)));
    RM_CHECK(ht != NULL);
    rt = thread_create_on(rm_remover_main, &rm, "vrmrm", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    RM_CHECK(rt != NULL);
    RM_CHECK(wait_flag_blk(&rm.ready, 2000));
    RM_CHECK(wait_flag_blk(&h.held, 2000));       /* the section is open */
    __atomic_store_n(&rm.go, 1u, __ATOMIC_RELEASE);
    RM_CHECK(wait_flag_blk(&rm.done, 8000));
    thread_join(rt);
    rt = NULL;
    thread_join(ht);
    ht = NULL;
    RM_CHECK(rm.rc == 0);

    uint64_t before_irq = g_rm->before_irq_seq(), walk = g_rm->walk_seq(), held_until = h.exit_seq;
    uint64_t boundary = g_rm->remove_seq();
    unsigned found = g_rm->inflight_at_remove();

    end = clock_now_ns() + 2000ull * 1000000ull;
    while (s.refused < 4 && clock_now_ns() < end)
        sched_yield();
    __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t);
    t = NULL;

    /*
     * The removal has to have entered the teardown while the section was
     * still open, or the run says nothing and the caller tries again.
     */
    *caught = before_irq != 0 && held_until != 0 && before_irq < held_until;
    if (!*caught) {
        kinfo("selftest: virtio-remove-inflight: irq-order: the teardown did not overlap the read-side section "
              "(entered %llu, section ended %llu); retrying",
              (unsigned long long)before_irq, (unsigned long long)held_until);
        goto out;
    }
    /* And the walk did not begin until the section had ended. */
    RM_CHECK(walk != 0 && held_until < walk);
    RM_CHECK(s.other == 0 && s.c_other == 0);
    RM_CHECK(rm_completions(&s) == s.ok);
    RM_CHECK(s.c_double == 0);
    RM_CHECK(s.c_eio == found);
    RM_CHECK(boundary != 0 && s.max_seq < boundary);
    RM_CHECK(rm_find() == NULL);
    blkdev_put(bd);
    bd = NULL;
    RM_CHECK(threads_settle_blk(threads0));
    kinfo("selftest: virtio-remove-inflight: irq-order: the removal entered the queue teardown at %llu with a "
          "read-side section open, the section ended at %llu and only then did the slot walk begin, at %llu; "
          "%u accepted, %u found in flight, %u completed -EIO, %u -ENODEV, %u ok",
          (unsigned long long)before_irq, (unsigned long long)held_until, (unsigned long long)walk, s.ok, found,
          s.c_eio, s.c_enodev, s.c_ok);

out:
    __atomic_store_n(&h.stop, 1u, __ATOMIC_RELEASE);
    if (rt != NULL) {
        __atomic_store_n(&rm.go, 1u, __ATOMIC_RELEASE);
        (void)wait_flag_blk(&rm.done, 8000);
        thread_join(rt);
    }
    if (ht != NULL)
        thread_join(ht);
    if (t != NULL) {
        __atomic_store_n(&s.stop, 1u, __ATOMIC_RELEASE);
        thread_join(t);
    }
    rm_drain(&s);
    if (bd != NULL)
        blkdev_put(bd);
    kfree(s.buf);
    return ok;
}

/*
 * The fourth pass: the hold stored while the handler is inside its
 * loop. The hold's contract is "from this moment, finished requests
 * stay in flight"; a check at the handler's door kept it only for
 * handlers not yet running, and a handler already popping when the
 * hold was stored drained the table -- the held pass found 0 once in
 * CI, on a driver nothing had changed (docs/testing/flakes.md). This
 * pass builds that moment rather than racing for it, and every hold in
 * it is stored from a completion callback, i.e. from inside the
 * handler, so that no handler can be between its check and its pop
 * when a hold lands (a hold stored by a thread has exactly that
 * window against a handler that has just run a callback):
 *
 *   arm; post P: its completion stores the hold from inside the loop,
 *   and the handler stops. Post A, wait until the device has finished
 *   it (`unconsumed` >= 1: parked in the used ring by the hold); post
 *   B, the same (>= 2). Arm again; release; post C.
 *
 * C's completion brings the handler in with three finished requests to
 * pop. It pops A, whose callback stores the hold from inside the loop.
 * With the check before every pop, B and C stay: the remove finds
 * exactly two and completes them -EIO. With the check at the door the
 * handler pops all three and the remove finds none. Deterministic
 * either way: nothing here waits on a clock for the device.
 */
static bool rm_post(struct rm_submitter *s, unsigned k)
{
    struct rm_bio *rb = &s->pool[k];
    rb->bio = (struct bio){ .dev = s->bd, .sector = 1, .nsectors = 1, .dir = BIO_READ,
                            .buf = s->buf, .done = rm_done, .arg = s };
    __atomic_store_n(&rb->busy, 1u, __ATOMIC_RELEASE);
    int rc = blk_submit(&rb->bio);
    if (rc == 0)
        __atomic_fetch_add(&s->ok, 1u, __ATOMIC_ACQ_REL);
    else
        __atomic_store_n(&rb->busy, 0u, __ATOMIC_RELEASE);
    return rc == 0;
}

static bool rm_wait_unconsumed(struct blkdev *bd, unsigned n)
{
    uint64_t end = clock_now_ns() + 2000ull * 1000000ull;
    while (g_rm->unconsumed(bd) < n) {
        if (clock_now_ns() > end)
            return false;
        sched_yield();
    }
    return true;
}

static bool rm_hold_inside_pass(struct blkdev *bd, struct pci_device *pdev, unsigned threads0, const char **reason)
{
    static struct rm_submitter s;
    bool ok = true;
    memset(&s, 0, sizeof(s));
    s.bd = bd;
    s.buf = kmalloc(4096, 0);
    RM_CHECK(s.buf != NULL);
    RM_CHECK(g_rm->unconsumed != NULL);
    unsigned releases0 = g_rm->releases();

    /* P: the first hold, stored from inside the handler. */
    __atomic_store_n(&s.rehold, bd, __ATOMIC_RELEASE);
    RM_CHECK(rm_post(&s, 0));
    RM_CHECK(rm_wait_completions(&s, 1));
    RM_CHECK(__atomic_load_n(&s.rehold, __ATOMIC_ACQUIRE) == NULL);
    /* A and B: finished at the device, parked by the hold. */
    RM_CHECK(rm_post(&s, 1));
    RM_CHECK(rm_wait_unconsumed(bd, 1));
    RM_CHECK(rm_post(&s, 2));
    RM_CHECK(rm_wait_unconsumed(bd, 2));
    RM_CHECK(rm_completions(&s) == 1);                 /* the hold held */
    /* Release, with the next completion re-storing it from inside the
     * loop; C's completion is what brings the handler in. */
    __atomic_store_n(&s.rehold, bd, __ATOMIC_RELEASE);
    g_rm->hold_completions(NULL);
    RM_CHECK(rm_post(&s, 3));
    RM_CHECK(rm_wait_completions(&s, 2));
    RM_CHECK(__atomic_load_n(&s.rehold, __ATOMIC_ACQUIRE) == NULL);

    RM_CHECK(pci_test_remove(pdev) == 0);
    uint64_t boundary = g_rm->remove_seq();
    unsigned found = g_rm->inflight_at_remove();
    g_rm->hold_completions(NULL);

    RM_CHECK(s.c_double == 0 && s.c_other == 0 && s.c_enodev == 0);
    RM_CHECK(s.c_ok == 2);                             /* P, and the one pop before the hold landed inside the loop */
    RM_CHECK(found == 2);                              /* the two behind it, still in the table when the remove walked it */
    RM_CHECK(s.c_eio == found);
    RM_CHECK(rm_completions(&s) == 4);
    RM_CHECK(boundary != 0 && s.max_seq < boundary);
    RM_CHECK(pdev->dev.driver == NULL && pdev->dev.drvdata == NULL && pdev->dev.state == DEV_UNBOUND);
    RM_CHECK(rm_find() == NULL);
    RM_CHECK(g_rm->releases() == releases0);
    blkdev_put(bd);
    bd = NULL;
    RM_CHECK(g_rm->releases() == releases0 + 1);
    RM_CHECK(threads_settle_blk(threads0));
    kinfo("selftest: virtio-remove-inflight: held-inside: the hold stored from a completion callback with three "
          "finished requests before the handler; it popped %u in all, the remove found %u in flight and completed "
          "them -EIO", s.c_ok, found);
out:
    __atomic_store_n(&s.rehold, NULL, __ATOMIC_RELEASE);   /* before the release: no callback re-arms it */
    rm_drain(&s);
    if (bd != NULL)
        blkdev_put(bd);
    kfree(s.buf);
    return ok;
}
#endif /* CONFIG_DEBUG */

bool selftest_virtio_remove_inflight(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: virtio-remove-inflight: no test hooks in this build; skipping");
    return true;
#else
    if (other_cpu_for_blk() == 0) {
        kinfo("selftest: virtio-remove-inflight: one CPU, no submitter to race");
        return true;
    }
    g_rm = blk_test_driver_hooks("virtio_blk");
    if (g_rm == NULL) {
        kinfo("selftest: virtio-remove-inflight: virtio_blk published no test seams; skipping");
        return true;
    }
    struct blkdev *bd = rm_find();
    if (bd == NULL) {
        kinfo("selftest: virtio-remove-inflight: no removal disk (QEMU_RMDISK=0); skipping");
        return true;
    }
    unsigned threads0 = thread_count();
    struct pci_device *pdev = to_pci_device(to_virtio_device(bd->dev)->hw);
    const char *pci_name = pdev->dev.name;
    char name[BLKDEV_NAME_MAX];
    memcpy(name, bd->name, sizeof(name));

    /* Something to read back after the rebind. */
    uint8_t *pattern = kmalloc(4096, 0), *check = kmalloc(4096, 0);
    if (pattern == NULL || check == NULL) {
        kfree(pattern);
        kfree(check);
        blkdev_put(bd);
        *reason = "check failed: the test's own buffers";
        return false;
    }
    for (unsigned i = 0; i < 512; i++)
        pattern[i] = (uint8_t)(i * 7 + 3);
    bool io_ok = rm_io(bd, BIO_WRITE, pattern) == 0;
    if (!io_ok) {
        kfree(pattern);
        kfree(check);
        blkdev_put(bd);
        *reason = "check failed: the removal disk does not answer a write";
        return false;
    }

    bool ok = true;
    unsigned order_attempts = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        if (pass > 0) {
            bd = rm_find();
            RM_CHECK(bd != NULL);
        }
        /* 0: the driver's slot table filled by construction. 1: the
         * natural race, a regression guard. 2: the teardown order --
         * a read-side section held across the removal's release of the
         * queue's interrupt. 3: the hold stored while the handler is
         * inside its loop. */
        bool caught = true;
        bool passed = pass == 2 ? rm_irq_order_pass(bd, pdev, threads0, &caught, reason)
                    : pass == 3 ? rm_hold_inside_pass(bd, pdev, threads0, reason)
                                : rm_pass(bd, pdev, pass == 0, threads0, reason);
        bd = NULL;   /* every pass drops the reference it was given */
        if (!passed) {
            ok = false;
            goto out;
        }
        if (!caught) {
            /* Nothing was in the window: rebind and run this pass again,
             * a bounded number of times. */
            RM_CHECK(++order_attempts < 4);
            RM_CHECK(pci_test_rebind(pdev) == 0);
            pass--;
            continue;
        }
        /* Back: the same function, the same name, the same sector. */
        RM_CHECK(pci_test_rebind(pdev) == 0);
        RM_CHECK(pci_test_rebind(pdev) == -EBUSY);   /* and only from unbound */
        struct blkdev *again = rm_find();
        RM_CHECK(again != NULL);
        RM_CHECK(strcmp(again->name, name) == 0);
        memset(check, 0, 512);
        RM_CHECK(rm_io(again, BIO_READ, check) == 0);
        RM_CHECK(memcmp(check, pattern, 512) == 0);
        blkdev_put(again);
    }
    kinfo("selftest: virtio-remove-inflight: %s (%s) removed with I/O outstanding and re-probed, four times", name,
          pci_name);

out:
    /* Whatever happened, the machine goes back as it was found: the
     * function bound, the disk registered. Every test after this one
     * depends on it. */
    if (bd != NULL)
        blkdev_put(bd);
    if (pdev->dev.state == DEV_UNBOUND && pci_test_rebind(pdev) != 0)
        kerror("selftest: virtio-remove-inflight: %s could not be re-bound; later tests run without it", pci_name);
    kfree(pattern);
    kfree(check);
    return ok;
#endif
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

/* The bus holds more than one kind of device now (a disk and a keyboard),
 * so a walk picks the one it means by interface class rather than taking
 * whichever enumerated first. */
struct usb_enum_walk {
    unsigned devices;
    struct usb_device *storage;
    struct usb_device *keyboard;
    struct usb_device *hub;
};

static int usb_enum_visit(struct device *dev, void *arg)
{
    struct usb_enum_walk *w = arg;
    struct usb_device *udev = to_usb_device(dev);
    w->devices++;
    if (udev->nr_intf == 0)
        return 0;
    uint8_t class = udev->intf[0].desc.bInterfaceClass;
    if (class == USB_CLASS_MASS_STORAGE && w->storage == NULL) {
        device_get(dev);
        w->storage = udev;
    } else if (class == USB_CLASS_HID && w->keyboard == NULL) {
        device_get(dev);
        w->keyboard = udev;
    } else if (class == USB_CLASS_HUB && w->hub == NULL) {
        device_get(dev);
        w->hub = udev;
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
    struct usb_enum_walk w = { 0, NULL, NULL, NULL };
    device_for_each(bus, usb_enum_visit, &w);
    if (w.devices == 0) {
        kinfo("selftest: usb-enum: no device on the usb bus (QEMU_USB=0); skipping");
        return true;
    }
    if (w.hub)
        device_put(&w.hub->dev);
    if (w.storage == NULL) {
        kinfo("selftest: usb-enum: no mass-storage device on the usb bus; skipping");
        if (w.keyboard)
            device_put(&w.keyboard->dev);
        return true;
    }
    struct usb_device *udev = w.storage;
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: usb-enum: step failed at line %d", __LINE__); ok = false; } } while (0)
    STEP(w.devices >= 1);
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
    /* The keyboard, when the harness attached one: the first device in
     * the tree with an interrupt endpoint, which is the part of the
     * controller driver nothing exercised before this unit. */
    if (w.keyboard != NULL) {
        struct usb_device *kbd = w.keyboard;
        const struct usb_interface *ki = &kbd->intf[0];
        STEP(kbd->slot != 0 && !kbd->gone);
        STEP(kbd->dev.parent != NULL);
        /* Where it is, which is what the controller was told (U2). On a
         * root port: no parent device on this bus, no route, the port is
         * the root port. Behind a hub: the parent is that hub, the route
         * names this port in the tier the hub sits at, and the root port
         * is the hub's. */
        if (kbd->parent == NULL) {
            STEP(kbd->depth == 0 && kbd->route == 0 && kbd->root_port == kbd->port);
            STEP(kbd->dev.parent == kbd->hcd->dev);
        } else {
            const struct usb_device *hub = kbd->parent;
            STEP(kbd->dev.parent == &hub->dev);
            STEP(kbd->depth == hub->depth + 1 && kbd->depth <= USB_MAX_DEPTH);
            STEP(kbd->root_port == hub->root_port);
            STEP(kbd->route == (hub->route | (kbd->port << (4 * hub->depth))));
            STEP(kbd->route != 0);
            STEP(hub->nr_intf > 0 && hub->intf[0].desc.bInterfaceClass == USB_CLASS_HUB);
            if (ok)
                kinfo("selftest: usb-enum: %s is behind %s: tier %u, route 0x%05x, root port %u",
                      kbd->dev.name, hub->dev.name, kbd->depth, kbd->route, kbd->root_port);
        }
        STEP(ki->desc.bInterfaceSubClass == 1 && ki->desc.bInterfaceProtocol == 1);   /* boot keyboard */
        STEP(ki->nr_ep == 1);
        const struct usb_endpoint_descriptor *ke = &ki->ep[0].desc;
        STEP(USB_EP_XFER(ke->bmAttributes) == USB_EP_INTERRUPT);
        STEP((ke->bEndpointAddress & USB_EP_DIR_IN) != 0);
        STEP((ke->wMaxPacketSize & 0x7ff) >= 8);
        STEP(ke->bInterval > 0);
        if (ok)
            kinfo("selftest: usb-enum: %s is a boot keyboard: endpoint 0x%02x, %u bytes every %u frames",
                  kbd->dev.name, ke->bEndpointAddress, ke->wMaxPacketSize & 0x7ff, ke->bInterval);
        device_put(&kbd->dev);
    }
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
/* The mass-storage device, which is what the unplug and timeout tests
 * mean by "the device"; the keyboard is another and is left alone. */
static struct usb_device *usb_first_device(void)
{
    struct bus_type *bus = bus_find("usb");
    if (bus == NULL)
        return NULL;
    struct usb_enum_walk w = { 0, NULL, NULL, NULL };
    device_for_each(bus, usb_enum_visit, &w);
    if (w.keyboard)
        device_put(&w.keyboard->dev);
    if (w.hub)
        device_put(&w.hub->dev);
    return w.storage;
}

/* The hub, when the harness put one there (QEMU_KBD=hub). */
static struct usb_device *usb_hub_device(void)
{
    struct bus_type *bus = bus_find("usb");
    if (bus == NULL)
        return NULL;
    struct usb_enum_walk w = { 0, NULL, NULL, NULL };
    device_for_each(bus, usb_enum_visit, &w);
    if (w.keyboard)
        device_put(&w.keyboard->dev);
    if (w.storage)
        device_put(&w.storage->dev);
    return w.hub;
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
        uint64_t dt = clock_since_ns(t0);
        total_dt += dt;
        faultinject_clear(kind);
        if (rc != -ETIMEDOUT)
            kerror("selftest: %s: the read returned %d after %llu ms, not -ETIMEDOUT (timeouts %llu -> %llu)",
                   tag, rc, (unsigned long long)(dt / 1000000), (unsigned long long)timeouts0,
                   (unsigned long long)bd->timeouts);
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
    STEP(bus != NULL);
    /* Counted against what is there, not against one: the keyboard is on
     * the same bus and this test must not notice it. */
    unsigned devices0 = bus != NULL ? device_count(bus) : 0;
    STEP(devices0 >= 1);
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
    STEP(device_count(bus) == devices0 - 1);
    STEP(bd == NULL || blk_read(bd, 0, 1, buf) == -ENODEV);   /* a holder's reference: refused, not served */
    if (bd != NULL)
        blkdev_put(bd);   /* the test's reference: the storage driver's memory can go */
    STEP(hcd->released == released0 + 1);   /* the usb_device release ran, once */

    /* Back: the port is reset and the device enumerated again; a fresh
     * usb_device, a fresh sda. */
    STEP(hcd->ops->debug_port(hcd, port, true) == 0);
    STEP(hcd->enumerated == enumerated0 + 1);
    STEP(device_count(bus) == devices0);
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
 * A hub unplugged with a device behind it (docs/drivers/usb/testing.md).
 *
 * The path this proves is the one the design is most easily got wrong
 * on: the controller reports the hub's root port gone, the core runs the
 * hub driver's `remove` with its own lock held, and `remove` joins the
 * worker whose last act is to take the hub's children down. A child
 * teardown that reached for that lock again would deadlock here and
 * nowhere else, and nothing in an ordinary boot would ever run it.
 */
bool selftest_usb_hub_unplug(const char **reason)
{
    struct usb_device *hub = usb_hub_device();
    if (hub == NULL) {
        kinfo("selftest: usb-hub-unplug: no hub on this machine (QEMU_KBD is not hub); skipping");
        return true;
    }
    struct usb_hcd *hcd = hub->hcd;
    unsigned port = hub->port;
    char name[DEVICE_NAME_MAX];
    strlcpy(name, hub->dev.name, sizeof(name));
    struct bus_type *bus = bus_find("usb");
    if (hcd->ops->debug_port == NULL || bus == NULL) {
        device_put(&hub->dev);
        kinfo("selftest: usb-hub-unplug: the controller has no debug port hook; skipping");
        return true;
    }
    bool ok = true;
#define STEP(x) do { if (ok && !(x)) { kerror("selftest: usb-hub-unplug: step failed at line %d", __LINE__); ok = false; } } while (0)
    unsigned devices0 = device_count(bus);
    uint64_t released0 = hcd->released;
    STEP(hub->parent == NULL && hub->depth == 0);   /* the harness puts the hub on a root port */
    device_put(&hub->dev);   /* the walk's: the releases must be able to run */

    /* Out: the hub and everything behind it, children first. The bus is
     * clear of both by the time the port call returns, because the core
     * takes the children down before the hub. */
    STEP(hcd->ops->debug_port(hcd, port, false) == 0);
    STEP(device_count(bus) == devices0 - 2);        /* the hub and its keyboard */
    /* The releases are not synchronous: the hub's worker holds a
     * reference to its own device until it has finished tidying up,
     * which is after `remove` returned (U9). Both must arrive, and
     * quickly. */
    for (unsigned i = 0; i < 2000 && hcd->released != released0 + 2; i++)
        thread_sleep_ms(1);
    STEP(hcd->released == released0 + 2);

    /* Back in: the hub enumerates, and its worker finds the device on
     * its port again -- which takes a debounce and a reset, so this is
     * the part of the test that waits. */
    STEP(hcd->ops->debug_port(hcd, port, true) == 0);
    for (unsigned i = 0; i < 2000 && device_count(bus) != devices0; i++)
        thread_sleep_ms(1);
    STEP(device_count(bus) == devices0);
    struct usb_device *again = usb_hub_device();
    STEP(again != NULL);
    if (again != NULL) {
        STEP(strcmp(again->dev.name, name) == 0);
        device_put(&again->dev);
    }
    if (ok)
        kinfo("selftest: usb-hub-unplug: %s and the device behind it went and came back", name);
#undef STEP
    if (!ok) {
        *reason = "usb-hub-unplug: see the log";
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
    uint64_t dt = clock_since_ns(t0);
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
    uint64_t dt = clock_since_ns(t0);
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
