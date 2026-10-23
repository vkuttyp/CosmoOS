/*
 * pci.c - PCI configuration access, enumeration, and MSI/MSI-X.
 *
 * Configuration space is reached through ECAM (the MCFG table's segment 0
 * window, mapped once, uncached) when ACPI provides it, otherwise through
 * the architecture's legacy mechanism. Enumeration is a depth-first walk
 * from bus 0 across bridges; every function found becomes a struct
 * pci_device on the "pci" bus. Nothing here knows any device class.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/kmalloc.h>
#include <kernel/iommu.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/pci_bar.h>
#include <kernel/printf.h>
#include <kernel/blk.h>
#include <kernel/mutex.h>
#include <kernel/part.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/pci.h>

#include <drivers/pci.h>

#define PCI_MAX_DEVICES 64

struct acpi_mcfg_alloc {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __packed;

struct acpi_mcfg {
    struct acpi_sdt_header hdr;
    uint64_t reserved;
    struct acpi_mcfg_alloc alloc[];
} __packed;

static vaddr_t g_ecam;               /* virtual base of the bus window, 0 = legacy */
static uint8_t g_ecam_start_bus, g_ecam_end_bus;
static LIST_HEAD(g_devices);
static struct pci_device *g_by_index[PCI_MAX_DEVICES];
static unsigned g_count;
static bool g_initialized;

/* --- configuration access ------------------------------------------- */

static volatile void *ecam_addr(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    uintptr_t rel = ((uintptr_t)(bus - g_ecam_start_bus) << 20) | ((uintptr_t)slot << 15) |
                    ((uintptr_t)func << 12) | (off & 0xfff);
    return (volatile void *)(g_ecam + rel);
}

static uint32_t cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width)
{
    if (g_ecam) {
        if (bus < g_ecam_start_bus || bus > g_ecam_end_bus)
            return 0xffffffffu;
        volatile void *a = ecam_addr(bus, slot, func, off);
        switch (width) {
        case 1: return *(volatile uint8_t *)a;
        case 2: return *(volatile uint16_t *)a;
        default: return *(volatile uint32_t *)a;
        }
    }
    return arch_pci_legacy_read(bus, slot, func, off, width);
}

static void cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width, uint32_t v)
{
    if (g_ecam) {
        if (bus < g_ecam_start_bus || bus > g_ecam_end_bus)
            return;
        volatile void *a = ecam_addr(bus, slot, func, off);
        switch (width) {
        case 1: *(volatile uint8_t *)a = (uint8_t)v; break;
        case 2: *(volatile uint16_t *)a = (uint16_t)v; break;
        default: *(volatile uint32_t *)a = v; break;
        }
        return;
    }
    arch_pci_legacy_write(bus, slot, func, off, width, v);
}

uint8_t pci_cfg_read8(const struct pci_device *p, uint16_t off)
{
    return (uint8_t)cfg_read(p->bus, p->slot, p->func, off, 1);
}
uint16_t pci_cfg_read16(const struct pci_device *p, uint16_t off)
{
    return (uint16_t)cfg_read(p->bus, p->slot, p->func, off, 2);
}
uint32_t pci_cfg_read32(const struct pci_device *p, uint16_t off)
{
    return cfg_read(p->bus, p->slot, p->func, off, 4);
}
void pci_cfg_write8(const struct pci_device *p, uint16_t off, uint8_t v)
{
    cfg_write(p->bus, p->slot, p->func, off, 1, v);
}
void pci_cfg_write16(const struct pci_device *p, uint16_t off, uint16_t v)
{
    cfg_write(p->bus, p->slot, p->func, off, 2, v);
}
void pci_cfg_write32(const struct pci_device *p, uint16_t off, uint32_t v)
{
    cfg_write(p->bus, p->slot, p->func, off, 4, v);
}

bool pci_ecam_in_use(void)
{
    return g_ecam != 0;
}

/* --- the bus ----------------------------------------------------------- */

static bool pci_match(struct device *dev, struct device_driver *drv)
{
    const struct pci_device *p = to_pci_device(dev);
    const struct pci_id *id = drv->match_data;
    for (; id->flags != 0xffffffffu; id++) {
        if (id->vendor != PCI_ANY && id->vendor != p->vendor)
            continue;
        if (id->device != PCI_ANY && id->device != p->device)
            continue;
        if ((id->flags & PCI_ID_CLASS) && (id->class != p->class || id->subclass != p->subclass))
            continue;
        return true;
    }
    return false;
}

struct bus_type pci_bus = {
    .name = "pci",
    .match = pci_match,
};

static const struct pci_id *matching_id(const struct pci_driver *pdrv, const struct pci_device *p)
{
    for (const struct pci_id *id = pdrv->ids; id->flags != 0xffffffffu; id++) {
        if ((id->vendor == PCI_ANY || id->vendor == p->vendor) &&
            (id->device == PCI_ANY || id->device == p->device) &&
            (!(id->flags & PCI_ID_CLASS) || (id->class == p->class && id->subclass == p->subclass)))
            return id;
    }
    return NULL;
}

/* The device model calls drv->probe(dev); we translate to the PCI form. */
static int pci_probe_thunk(struct device *dev);
static void pci_remove_thunk(struct device *dev);

int pci_register_driver(struct pci_driver *pdrv)
{
    KASSERT(pdrv->ids != NULL && pdrv->probe != NULL);
    pdrv->drv.bus = &pci_bus;
    pdrv->drv.match_data = pdrv->ids;
    pdrv->drv.probe = pci_probe_thunk;
    pdrv->drv.remove = pci_remove_thunk;
    return driver_register(&pdrv->drv);
}

void pci_unregister_driver(struct pci_driver *pdrv)
{
    driver_unregister(&pdrv->drv);
}

/* The model names the driver in dev->driver before calling probe. */
static int pci_probe_thunk(struct device *dev)
{
    struct pci_driver *pdrv = container_of(dev->driver, struct pci_driver, drv);
    struct pci_device *p = to_pci_device(dev);
    const struct pci_id *id = matching_id(pdrv, p);
    if (id == NULL)
        return -ENODEV;
    return pdrv->probe(p, id);
}

static void pci_remove_thunk(struct device *dev)
{
    struct pci_driver *pdrv = container_of(dev->driver, struct pci_driver, drv);
    if (pdrv->remove)
        pdrv->remove(to_pci_device(dev));
}

#if CONFIG_DEBUG
/*
 * Remove a live PCI device: the bound driver's `remove` and the model's
 * bookkeeping, which is one operation and not two
 * (docs/audit/next-subsystem-lifetime-windows.md).
 */
int pci_test_remove(struct pci_device *p)
{
    if (p == NULL || p->dev.driver == NULL)
        return -ENODEV;
    device_test_unbind(&p->dev);
    return 0;
}

/* Re-probe a function a test removed: the registered drivers against an
 * unbound device, which is how the boot bound it in the first place. The
 * assertion this exists for is that a removal left the hardware sane
 * (docs/audit/next-subsystem-virtio-remove-inflight.md). */
int pci_test_rebind(struct pci_device *p)
{
    if (p == NULL)
        return -ENODEV;
    return device_test_bind(&p->dev);
}
#endif

/* --- enumeration --------------------------------------------------------- */

static void decode_bars(struct pci_device *p)
{
    unsigned nbars = (p->header_type & 0x7f) == 0 ? 6 : 2;
    uint16_t cmd = pci_cfg_read16(p, PCI_COMMAND);
    pci_cfg_write16(p, PCI_COMMAND, cmd & (uint16_t)~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));

    for (unsigned i = 0; i < nbars; i++) {
        uint16_t off = (uint16_t)(PCI_BAR0 + 4 * i);
        uint32_t orig = pci_cfg_read32(p, off);
        pci_cfg_write32(p, off, 0xffffffffu);
        uint32_t mask = pci_cfg_read32(p, off);
        pci_cfg_write32(p, off, orig);
        if (mask == 0)
            continue;

        struct pci_bar *b = &p->bar[i];
        if (orig & 1) {
            b->io = true;
            b->base = orig & ~3u;
            b->size = (~(mask & ~3u) + 1) & 0xffffu;
        } else {
            b->is64 = ((orig >> 1) & 3) == 2;
            b->prefetch = (orig & 8) != 0;
            uint64_t base = orig & ~0xfULL;
            uint64_t m = mask & ~0xfULL;
            if (b->is64 && i + 1 < nbars) {
                uint16_t off_hi = (uint16_t)(off + 4);
                uint32_t orig_hi = pci_cfg_read32(p, off_hi);
                pci_cfg_write32(p, off_hi, 0xffffffffu);
                uint32_t mask_hi = pci_cfg_read32(p, off_hi);
                pci_cfg_write32(p, off_hi, orig_hi);
                base |= (uint64_t)orig_hi << 32;
                m |= (uint64_t)mask_hi << 32;
            } else {
                m |= 0xffffffff00000000ULL;
            }
            b->base = base;
            b->size = pci_bar_mem_size(m);   /* the mask's lowest set bit */
        }
        if (b->size)
            device_add_resource(&p->dev, b->io ? RES_IO : RES_MMIO, b->base, b->size, i);
        if (b->is64)
            i++;
    }
    pci_cfg_write16(p, PCI_COMMAND, cmd);
}

static void find_capabilities(struct pci_device *p)
{
    if ((pci_cfg_read16(p, PCI_STATUS) & PCI_STATUS_CAP_LIST) == 0)
        return;
    uint8_t off = pci_cfg_read8(p, PCI_CAP_PTR) & 0xfc;
    for (unsigned guard = 0; off >= 0x40 && guard < 48; guard++) {
        uint8_t id = pci_cfg_read8(p, off);
        if (id == PCI_CAP_ID_MSI && p->cap_msi == 0)
            p->cap_msi = off;
        if (id == PCI_CAP_ID_MSIX && p->cap_msix == 0)
            p->cap_msix = off;
        off = pci_cfg_read8(p, (uint16_t)(off + 1)) & 0xfc;
    }
}

static void scan_bus(uint8_t bus, struct device *parent, unsigned depth);
static void pciehp_init(void);

/* A function is enumerated at boot or when a hot-plug slot gains it, and
 * unregistered when the slot loses it (remove_bus); the release runs when
 * the last holder of its device is gone. */
static void pci_device_release(struct device *dev)
{
    struct pci_device *p = container_of(dev, struct pci_device, dev);
    kfree(p->msix.vectors);
    kfree(p);
}

static void scan_function(uint8_t bus, uint8_t slot, uint8_t func, struct device *parent, unsigned depth)
{
    uint32_t id = cfg_read(bus, slot, func, PCI_VENDOR_ID, 4);
    if ((id & 0xffff) == 0xffff)
        return;
    if (g_count == PCI_MAX_DEVICES) {
        kwarn("pci: more than %u devices; ignoring %02x:%02x.%u", PCI_MAX_DEVICES, bus, slot, func);
        return;
    }
    struct pci_device *p = kzalloc(sizeof(*p));
    if (p == NULL)
        panic("pci: out of memory during enumeration");
    char name[DEVICE_NAME_MAX];
    ksnprintf(name, sizeof(name), "pci:%02x:%02x.%u", bus, slot, func);
    device_setup(&p->dev, &pci_bus, parent, name);
    p->dev.release = pci_device_release;
    list_init(&p->link);
    p->msi_vector = -1;
    p->bus = bus;
    p->slot = slot;
    p->func = func;
    p->vendor = (uint16_t)id;
    p->device = (uint16_t)(id >> 16);
    uint32_t cls = pci_cfg_read32(p, PCI_REVISION);
    p->revision = (uint8_t)cls;
    p->prog_if = (uint8_t)(cls >> 8);
    p->subclass = (uint8_t)(cls >> 16);
    p->class = (uint8_t)(cls >> 24);
    p->header_type = pci_cfg_read8(p, PCI_HEADER_TYPE);
    p->irq_pin = pci_cfg_read8(p, PCI_IRQ_PIN);
    if ((p->header_type & 0x7f) == 0) {
        p->subsys_vendor = pci_cfg_read16(p, PCI_SUBSYS_VENDOR);
        p->subsys_id = pci_cfg_read16(p, PCI_SUBSYS_ID);
    }
    decode_bars(p);
    find_capabilities(p);

    list_push_back(&g_devices, &p->link);
    g_by_index[g_count++] = p;
    kdebug("pci: %s %04x:%04x class %02x.%02x rev %u hdr %u%s%s", name, p->vendor, p->device, p->class,
           p->subclass, p->revision, p->header_type & 0x7f, p->cap_msi ? " msi" : "",
           p->cap_msix ? " msix" : "");

    int rc = device_register(&p->dev);
    if (rc)
        kwarn("pci: cannot register %s (%d)", name, rc);

    if ((p->header_type & 0x7f) == 1 && depth < 8) {
        uint8_t secondary = pci_cfg_read8(p, PCI_SECONDARY_BUS);
        if (secondary > bus)
            scan_bus(secondary, &p->dev, depth + 1);
    }
}

static void scan_bus(uint8_t bus, struct device *parent, unsigned depth)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = cfg_read(bus, slot, 0, PCI_VENDOR_ID, 4);
        if ((id & 0xffff) == 0xffff)
            continue;
        uint8_t hdr = (uint8_t)cfg_read(bus, slot, 0, PCI_HEADER_TYPE, 1);
        unsigned nfunc = (hdr & 0x80) ? 8 : 1;
        for (uint8_t func = 0; func < nfunc; func++)
            scan_function(bus, slot, func, parent, depth);
    }
}

static void setup_ecam(void)
{
    const struct acpi_mcfg *mcfg = (const struct acpi_mcfg *)acpi_find_table("MCFG");
    if (mcfg == NULL || mcfg->hdr.length < sizeof(*mcfg))
        return;   /* a table shorter than its header would wrap the count below */
    size_t n = (mcfg->hdr.length - sizeof(*mcfg)) / sizeof(struct acpi_mcfg_alloc);
    for (size_t i = 0; i < n; i++) {
        const struct acpi_mcfg_alloc *a = &mcfg->alloc[i];
        if (a->segment != 0 || a->end_bus < a->start_bus)
            continue;
        size_t buses = (size_t)a->end_bus - a->start_bus + 1;
        vaddr_t va = vm_map_phys((paddr_t)a->base, buses << 20, VM_PROT_RW, VM_CACHE_UC);
        if (va == 0) {
            kwarn("pci: cannot map ECAM at 0x%llx (%zu buses); using legacy access",
                  (unsigned long long)a->base, buses);
            return;
        }
        g_ecam = va;
        g_ecam_start_bus = a->start_bus;
        g_ecam_end_bus = a->end_bus;
        kinfo("pci: ECAM at 0x%llx, buses %u-%u", (unsigned long long)a->base, a->start_bus, a->end_bus);
        return;
    }
}

void pci_init(void)
{
    KASSERT(!g_initialized);
    g_initialized = true;
    bus_register(&pci_bus);
    setup_ecam();
    if (g_ecam == 0) {
        if (!arch_pci_legacy_available()) {
            kwarn("pci: no ECAM and no legacy access; PCI unavailable");
            return;
        }
        kinfo("pci: no MCFG; using legacy configuration access");
    }
    scan_bus(0, NULL, 0);
    kinfo("pci: %u device(s) enumerated", g_count);
    pciehp_init();
}

/* --- PCIe native hotplug (roadmap 1.2 B3, DRV-6) ------------------------
 *
 * docs/drivers/pci/design.md, "Hotplug". A root or downstream port whose
 * slot capability says it is hot-plug capable gets its slot's events --
 * the attention button QEMU's device_del presses, presence detect and
 * data-link-layer changes -- as an MSI; one thread acts on them: a
 * removal unbinds and unregisters the functions behind the port and then
 * powers the slot off (which is what lets QEMU finish removing the
 * device), an insertion powers the slot on, waits for the link, gives the
 * new function BARs from the port's windows and enumerates it, and the
 * drivers probe it as they would at boot. A port whose MSI cannot be set
 * up is polled at a slow rate instead, and says so.
 */

#define PCI_CAP_ID_EXP        0x10
#define PCI_EXP_FLAGS         0x02
#define PCI_EXP_FLAGS_TYPE(f) (((f) >> 4) & 0xf)
#define PCI_EXP_FLAGS_SLOT    (1u << 8)
#define PCI_EXP_TYPE_ROOT     4u
#define PCI_EXP_TYPE_DOWN     6u
#define PCI_EXP_LNKSTA        0x12
#define PCI_EXP_LNKSTA_DLLLA  (1u << 13)
#define PCI_EXP_SLTCAP        0x14
#define PCI_EXP_SLTCAP_ABP    (1u << 0)
#define PCI_EXP_SLTCAP_PCP    (1u << 1)
#define PCI_EXP_SLTCAP_HPC    (1u << 6)
#define PCI_EXP_SLTCTL        0x18
#define PCI_EXP_SLTCTL_ABPE   (1u << 0)
#define PCI_EXP_SLTCTL_PDCE   (1u << 3)
#define PCI_EXP_SLTCTL_HPIE   (1u << 5)
#define PCI_EXP_SLTCTL_PIC    (3u << 8)
#define PCI_EXP_SLTCTL_PIC_ON  (1u << 8)
#define PCI_EXP_SLTCTL_PIC_OFF (3u << 8)
#define PCI_EXP_SLTCTL_PCC    (1u << 10)   /* 1: power off */
#define PCI_EXP_SLTCTL_DLLSCE (1u << 12)
#define PCI_EXP_SLTSTA        0x1a
#define PCI_EXP_SLTSTA_ABP    (1u << 0)
#define PCI_EXP_SLTSTA_PDC    (1u << 3)
#define PCI_EXP_SLTSTA_CC     (1u << 4)
#define PCI_EXP_SLTSTA_PDS    (1u << 6)
#define PCI_EXP_SLTSTA_DLLSC  (1u << 8)
#define PCI_EXP_SLTSTA_EVENTS (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_CC | PCI_EXP_SLTSTA_DLLSC | \
                               (1u << 1) | (1u << 2))

#define PCI_IO_BASE           0x1c
#define PCI_MEMORY_BASE       0x20
#define PCI_PREF_MEMORY_BASE  0x24
#define PCI_PREF_BASE_UPPER   0x28
#define PCI_PREF_LIMIT_UPPER  0x2c
#define PCI_IO_BASE_UPPER     0x30

#define HP_POLL_MS       20u     /* the thread's look at what the interrupts said */
#define HP_SLOW_POLL_MS  500u    /* a port without an interrupt: read its slot */
#define HP_LINK_WAIT_MS  1000u

struct hp_slot {
    struct pci_device *port;
    uint8_t exp;                 /* the PCI Express capability's offset */
    uint8_t secondary;
    bool msi;                    /* events arrive as interrupts; else polled */
    bool powered;
    uint32_t irqs;               /* interrupts taken (atomic) */
    uint32_t seen;               /* of which the thread has acted on */
    uint64_t next_poll_ns;
    uint64_t events;             /* acted on, for the console and the tests */
};

#define HP_MAX_SLOTS 16
static struct hp_slot g_slots[HP_MAX_SLOTS];
static unsigned g_nslots;
static struct thread *g_hp_thread;
static struct mutex g_hp_lock;   /* the device list against a scan or a removal */

static void hp_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct hp_slot *s = arg;
    __atomic_fetch_add(&s->irqs, 1, __ATOMIC_RELEASE);
}

static uint16_t sltsta(const struct hp_slot *s) { return pci_cfg_read16(s->port, (uint16_t)(s->exp + PCI_EXP_SLTSTA)); }
static uint16_t sltctl(const struct hp_slot *s) { return pci_cfg_read16(s->port, (uint16_t)(s->exp + PCI_EXP_SLTCTL)); }

static void set_sltctl(struct hp_slot *s, uint16_t clear, uint16_t set)
{
    uint16_t v = sltctl(s);
    v = (uint16_t)((v & ~clear) | set);
    pci_cfg_write16(s->port, (uint16_t)(s->exp + PCI_EXP_SLTCTL), v);
}

/* The functions on bus `bus`, and everything below them. */
static void remove_bus(uint8_t bus, unsigned depth)
{
    for (bool again = true; again;) {
        again = false;
        for (unsigned i = 0; i < g_count; i++) {
            struct pci_device *p = g_by_index[i];
            if (p->bus != bus)
                continue;
            if ((p->header_type & 0x7f) == 1 && depth < 8)
                remove_bus(pci_cfg_read8(p, PCI_SECONDARY_BUS), depth + 1);
            kinfo("pci: %s %04x:%04x removed", p->dev.name, p->vendor, p->device);
            for (unsigned k = i; k + 1 < g_count; k++)
                g_by_index[k] = g_by_index[k + 1];
            g_by_index[--g_count] = NULL;
            list_remove(&p->link);
            device_unregister(&p->dev);   /* the driver's remove, the bus's reference */
            device_put(&p->dev);          /* the enumeration's: the release frees it */
            again = true;
            break;
        }
    }
}

/* A window of the port: [base, limit], 0 limit for none. */
struct hp_window {
    uint64_t base, limit, next;
};

static void port_windows(const struct pci_device *port, struct hp_window *mem, struct hp_window *pref,
                         struct hp_window *io)
{
    uint32_t m = pci_cfg_read32(port, PCI_MEMORY_BASE);
    mem->base = (uint64_t)(m & 0xfff0u) << 16;
    mem->limit = ((uint64_t)(m >> 16 & 0xfff0u) << 16) | 0xfffffu;
    if (mem->limit < mem->base)
        mem->base = mem->limit = 0;
    uint32_t p = pci_cfg_read32(port, PCI_PREF_MEMORY_BASE);
    pref->base = (uint64_t)(p & 0xfff0u) << 16;
    pref->limit = ((uint64_t)(p >> 16 & 0xfff0u) << 16) | 0xfffffu;
    if ((p & 0xf) == 1) {
        pref->base |= (uint64_t)pci_cfg_read32(port, PCI_PREF_BASE_UPPER) << 32;
        pref->limit |= (uint64_t)pci_cfg_read32(port, PCI_PREF_LIMIT_UPPER) << 32;
    }
    if (pref->limit < pref->base)
        pref->base = pref->limit = 0;
    uint32_t i = pci_cfg_read32(port, PCI_IO_BASE);
    io->base = (uint64_t)(i & 0xf0u) << 8;
    io->limit = ((uint64_t)(i >> 8 & 0xf0u) << 8) | 0xfffu;
    if ((i & 0xf) == 1) {
        uint32_t up = pci_cfg_read32(port, PCI_IO_BASE_UPPER);
        io->base |= (uint64_t)(up & 0xffffu) << 16;
        io->limit |= (uint64_t)(up >> 16) << 16;
    }
    if (io->limit < io->base || (i & 0xffffu) == 0)
        io->base = io->limit = 0;
    mem->next = mem->base;
    pref->next = pref->base;
    io->next = io->base;
}

static bool take(struct hp_window *w, uint64_t size, uint64_t *at)
{
    if (w->limit == 0 || size == 0)
        return false;
    uint64_t a = (w->next + size - 1) & ~(size - 1);
    if (a < w->next || a + size - 1 > w->limit)
        return false;
    *at = a;
    w->next = a + size;
    return true;
}

/*
 * A function behind a port that nobody configured: firmware gave its BARs
 * addresses at boot, and a device added after it has none. Each BAR gets
 * one inside the port's window -- prefetchable memory into the
 * prefetchable window when there is one, other memory into the memory
 * window, I/O into the I/O window -- aligned to its size. A BAR that does
 * not fit is left at 0 and said; the driver will find it unusable.
 */
static void assign_bars(uint8_t bus, uint8_t slot, uint8_t func, struct hp_window *mem, struct hp_window *pref,
                        struct hp_window *io)
{
    uint8_t hdr = (uint8_t)cfg_read(bus, slot, func, PCI_HEADER_TYPE, 1);
    unsigned nbars = (hdr & 0x7f) == 0 ? 6 : 2;
    uint16_t cmd = (uint16_t)cfg_read(bus, slot, func, PCI_COMMAND, 2);
    cfg_write(bus, slot, func, PCI_COMMAND, 2, cmd & (uint16_t)~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    for (unsigned i = 0; i < nbars; i++) {
        uint16_t off = (uint16_t)(PCI_BAR0 + 4 * i);
        cfg_write(bus, slot, func, off, 4, 0xffffffffu);
        uint32_t mask = cfg_read(bus, slot, func, off, 4);
        if (mask == 0 || mask == 0xffffffffu)
            continue;
        bool isio = mask & 1;
        bool is64 = !isio && ((mask >> 1) & 3) == 2;
        bool prefetch = !isio && (mask & 8);
        uint64_t m;
        if (isio) {
            m = (mask & ~3u) | 0xffff0000u;
            m |= 0xffffffff00000000ULL;
        } else {
            m = mask & ~0xfULL;
            if (is64 && i + 1 < nbars) {
                cfg_write(bus, slot, func, (uint16_t)(off + 4), 4, 0xffffffffu);
                m |= (uint64_t)cfg_read(bus, slot, func, (uint16_t)(off + 4), 4) << 32;
            } else {
                m |= 0xffffffff00000000ULL;
            }
        }
        uint64_t size = pci_bar_mem_size(m);
        uint64_t at = 0;
        bool ok = isio ? take(io, size, &at)
                       : (prefetch && is64 && take(pref, size, &at)) || take(mem, size, &at) ||
                             (prefetch && take(pref, size, &at));
        if (!ok) {
            kwarn("pci: %02x:%02x.%u BAR%u (%llu bytes) does not fit the port's window", bus, slot, func, i,
                  (unsigned long long)size);
            at = 0;
        }
        cfg_write(bus, slot, func, off, 4, (uint32_t)at | (mask & (isio ? 3u : 0xfu)));
        if (is64) {
            cfg_write(bus, slot, func, (uint16_t)(off + 4), 4, (uint32_t)(at >> 32));
            i++;
        }
    }
    cfg_write(bus, slot, func, PCI_COMMAND, 2, cmd);
}

/* The whole disks registered now, by name (the hot-added ones are the
 * ones not in the list taken before their probe). */
#define HP_DISKS_MAX 64
static unsigned disk_names(char (*out)[BLKDEV_NAME_MAX])
{
    unsigned n = 0;
    for (unsigned i = 0; n < HP_DISKS_MAX; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        if (!blk_is_partition(bd))
            strlcpy(out[n++], bd->name, BLKDEV_NAME_MAX);
        blkdev_put(bd);
    }
    return n;
}

/*
 * A disk that arrived with the slot's device (BLK-5): its partitions are
 * read now, as boot composition reads every disk's, without being asked.
 * Here and not at every registration after boot: a self-test that unbinds
 * and rebinds a disk must not find a scan holding it.
 */
static void scan_new_disks(char (*before)[BLKDEV_NAME_MAX], unsigned nbefore)
{
    static char now[HP_DISKS_MAX][BLKDEV_NAME_MAX];
    unsigned n = disk_names(now);
    for (unsigned i = 0; i < n; i++) {
        bool old = false;
        for (unsigned k = 0; k < nbefore && !old; k++)
            old = strcmp(now[i], before[k]) == 0;
        if (old)
            continue;
        struct blkdev *bd = blk_find(now[i]);
        if (bd == NULL)
            continue;
        unsigned parts = 0;
        int rc = blk_part_scan(bd, &parts);
        kinfo("part: %s registered after boot: %u partition(s)%s", bd->name, parts, rc == 0 ? "" : " (no table)");
        blkdev_put(bd);
    }
}

/* What the slot holds now, enumerated and handed to the drivers. */
static void add_slot_devices(struct hp_slot *s)
{
    static char before[HP_DISKS_MAX][BLKDEV_NAME_MAX];
    unsigned nbefore = disk_names(before);
    uint32_t id = cfg_read(s->secondary, 0, 0, PCI_VENDOR_ID, 4);
    if ((id & 0xffff) == 0xffff) {
        kwarn("pci: %s: a device is present and does not answer", s->port->dev.name);
        return;
    }
    struct hp_window mem, pref, io;
    port_windows(s->port, &mem, &pref, &io);
    uint8_t hdr = (uint8_t)cfg_read(s->secondary, 0, 0, PCI_HEADER_TYPE, 1);
    unsigned nfunc = (hdr & 0x80) ? 8 : 1;
    for (uint8_t f = 0; f < nfunc; f++)
        if ((cfg_read(s->secondary, 0, f, PCI_VENDOR_ID, 4) & 0xffff) != 0xffff)
            assign_bars(s->secondary, 0, f, &mem, &pref, &io);
    /* The bridge forwards nothing until memory and I/O decode are on. */
    uint16_t cmd = pci_cfg_read16(s->port, PCI_COMMAND);
    pci_cfg_write16(s->port, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_IO | PCI_COMMAND_MASTER);
    unsigned nfn = g_count;
    for (uint8_t f = 0; f < nfunc; f++)
        scan_function(s->secondary, 0, f, &s->port->dev, 1);
    kinfo("pci: %s: %u function(s) added behind it", s->port->dev.name, g_count - nfn);
    scan_new_disks(before, nbefore);
}

static bool link_up(const struct hp_slot *s)
{
    return (pci_cfg_read16(s->port, (uint16_t)(s->exp + PCI_EXP_LNKSTA)) & PCI_EXP_LNKSTA_DLLLA) != 0;
}

static bool slot_has_devices(const struct hp_slot *s)
{
    for (unsigned i = 0; i < g_count; i++)
        if (g_by_index[i]->bus == s->secondary)
            return true;
    return false;
}

/* One slot's events, read and cleared, then acted on. */
static void hp_handle(struct hp_slot *s)
{
    uint16_t st = sltsta(s);
    uint16_t ev = st & PCI_EXP_SLTSTA_EVENTS;
    if (ev)
        pci_cfg_write16(s->port, (uint16_t)(s->exp + PCI_EXP_SLTSTA), ev);   /* RW1C */
    bool present = (st & PCI_EXP_SLTSTA_PDS) != 0;
    if (ev & (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC))
        s->events++;
    mutex_lock(&g_hp_lock);
    if ((ev & PCI_EXP_SLTSTA_ABP) && slot_has_devices(s)) {
        /* The button: the device is to go. Its drivers first, then the
         * slot's power -- QEMU removes the device when the power goes. */
        kinfo("pci: %s: attention button: removing the device behind it", s->port->dev.name);
        remove_bus(s->secondary, 1);
        set_sltctl(s, PCI_EXP_SLTCTL_PIC, PCI_EXP_SLTCTL_PCC | PCI_EXP_SLTCTL_PIC_OFF);
        s->powered = false;
    } else if (!present && slot_has_devices(s)) {
        /* Gone without a button press: what was behind it is gone. */
        kinfo("pci: %s: the device went away", s->port->dev.name);
        remove_bus(s->secondary, 1);
    } else if (present && !slot_has_devices(s) && (ev & (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC))) {
        if (!s->powered || (sltctl(s) & PCI_EXP_SLTCTL_PCC)) {
            set_sltctl(s, PCI_EXP_SLTCTL_PCC | PCI_EXP_SLTCTL_PIC, PCI_EXP_SLTCTL_PIC_ON);
            s->powered = true;
        }
        for (unsigned ms = 0; !link_up(s) && ms < HP_LINK_WAIT_MS; ms += 10)
            thread_sleep_ms(10);
        if (!link_up(s)) {
            kwarn("pci: %s: a device is present and its link did not come up", s->port->dev.name);
        } else {
            thread_sleep_ms(100);   /* PCIe: 100 ms from link up to the first configuration request */
            kinfo("pci: %s: a device was added", s->port->dev.name);
            add_slot_devices(s);
        }
    }
    mutex_unlock(&g_hp_lock);
}

static void hp_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(HP_POLL_MS);
        uint64_t now = clock_now_ns();
        for (unsigned i = 0; i < g_nslots; i++) {
            struct hp_slot *s = &g_slots[i];
            uint32_t irqs = __atomic_load_n(&s->irqs, __ATOMIC_ACQUIRE);
            bool due = irqs != s->seen;
            if (!s->msi && now >= s->next_poll_ns) {
                due = due || (sltsta(s) & PCI_EXP_SLTSTA_EVENTS) != 0;
                s->next_poll_ns = now + (uint64_t)HP_SLOW_POLL_MS * 1000000ull;
            }
            if (!due)
                continue;
            s->seen = irqs;
            hp_handle(s);
        }
    }
}

/*
 * After enumeration: every hot-plug capable slot's events enabled, its
 * interrupt requested, and the thread that acts on them started. A slot
 * already holding a device at boot is powered (firmware did it); an empty
 * one is left as firmware left it until a device arrives.
 */
static void pciehp_init(void)
{
    mutex_init(&g_hp_lock, "pciehp");
    for (unsigned i = 0; i < g_count && g_nslots < HP_MAX_SLOTS; i++) {
        struct pci_device *p = g_by_index[i];
        if ((p->header_type & 0x7f) != 1)
            continue;
        uint8_t exp = pci_find_capability(p, PCI_CAP_ID_EXP, 0);
        if (exp == 0)
            continue;
        uint16_t flags = pci_cfg_read16(p, (uint16_t)(exp + PCI_EXP_FLAGS));
        unsigned type = PCI_EXP_FLAGS_TYPE(flags);
        if ((type != PCI_EXP_TYPE_ROOT && type != PCI_EXP_TYPE_DOWN) || !(flags & PCI_EXP_FLAGS_SLOT))
            continue;
        uint32_t cap = pci_cfg_read32(p, (uint16_t)(exp + PCI_EXP_SLTCAP));
        if (!(cap & PCI_EXP_SLTCAP_HPC))
            continue;
        struct hp_slot *s = &g_slots[g_nslots++];
        s->port = p;
        s->exp = exp;
        s->secondary = pci_cfg_read8(p, PCI_SECONDARY_BUS);
        s->powered = !(sltctl(s) & PCI_EXP_SLTCTL_PCC);
    }
}

/*
 * After the IOMMU is up (kernel/core/main.c): each slot's interrupt and
 * events enabled, and the thread started. An MSI is a write the port
 * makes, so it needs bus mastering and, behind an SMMU, a DMA domain --
 * where the interrupt controller's doorbell is mapped; attached before the
 * IOMMU exists, the port has none, and on AArch64 its first event faults
 * in the SMMU and never arrives. That is why this is not pci_init's.
 */
void pci_hotplug_start(void)
{
    for (unsigned k = 0; k < g_nslots; k++) {
        struct hp_slot *s = &g_slots[k];
        struct pci_device *p = s->port;
        uint16_t flags = pci_cfg_read16(p, (uint16_t)(s->exp + PCI_EXP_FLAGS));
        uint32_t cap = pci_cfg_read32(p, (uint16_t)(s->exp + PCI_EXP_SLTCAP));
        pci_cfg_write16(p, (uint16_t)(s->exp + PCI_EXP_SLTSTA), PCI_EXP_SLTSTA_EVENTS);
        pci_enable_device(p, true);
        /* MSI-X (QEMU's pcie-root-port has only that, on a BAR of its own)
         * at the entry its Express capability names for hot-plug events,
         * else MSI, else none and a poll. */
        int v = -ENODEV;
        if (p->cap_msix) {
            unsigned entry = (flags >> 9) & 0x1f;   /* Interrupt Message Number */
            v = pci_msix_enable(p, entry + 1);
            if (v > (int)entry)
                v = pci_msix_request(p, entry, hp_irq, s, "pciehp", IRQ_CPU_ANY);
            else if (v >= 0)
                v = -ENOSPC;
            if (v < 0 && p->msix.table)
                pci_msix_disable(p);
        }
        if (v < 0 && p->cap_msi)
            v = pci_msi_enable(p, hp_irq, s, "pciehp", IRQ_CPU_ANY);
        s->msi = v >= 0;
        uint16_t en = PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_DLLSCE | (s->msi ? PCI_EXP_SLTCTL_HPIE : 0);
        if (cap & PCI_EXP_SLTCAP_ABP)
            en |= PCI_EXP_SLTCTL_ABPE;
        set_sltctl(s, 0, en);
        if (s->msi)
            kinfo("pci: %s: hot-plug slot %u (bus %02x)%s, events by %s", p->dev.name, cap >> 19, s->secondary,
                  (cap & PCI_EXP_SLTCAP_ABP) ? ", attention button" : "", p->msix.table ? "MSI-X" : "MSI");
        else
            kinfo("pci: %s: hot-plug slot %u (bus %02x)%s, no MSI or MSI-X (%d): polled every 500 ms", p->dev.name,
                  cap >> 19, s->secondary, (cap & PCI_EXP_SLTCAP_ABP) ? ", attention button" : "", v);
    }
    if (g_nslots == 0)
        return;
    g_hp_thread = thread_create(hp_thread, NULL, "pciehp", SCHED_PRIO_DEFAULT);
    if (g_hp_thread == NULL)
        kerror("pci: no hotplug thread; slots are not watched");
}

/* Counters for the tests: interrupts taken and events acted on, over every slot. */
void pci_hotplug_stats(unsigned *slots, uint64_t *irqs, uint64_t *events)
{
    uint64_t i = 0, e = 0;
    for (unsigned k = 0; k < g_nslots; k++) {
        i += __atomic_load_n(&g_slots[k].irqs, __ATOMIC_RELAXED);
        e += g_slots[k].events;
    }
    *slots = g_nslots;
    *irqs = i;
    *events = e;
}

/* --- driver services ---------------------------------------------------- */

void pci_enable_device(struct pci_device *p, bool bus_master)
{
    /* Before the device may master the bus it gets its DMA domain, so its
     * first bus address is already a translated one (kernel/iommu). */
    if (bus_master) {
        int rc = iommu_attach_device(&p->dev, pci_requester_id(p));
        if (rc)
            kerror("pci: %s: no DMA domain (%d); bus mastering will fault", p->dev.name, rc);
    }
    uint16_t cmd = pci_cfg_read16(p, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_IO;
    if (bus_master)
        cmd |= PCI_COMMAND_MASTER;
    pci_cfg_write16(p, PCI_COMMAND, cmd);
}

vaddr_t pci_map_bar(struct pci_device *p, unsigned bar)
{
    if (bar >= PCI_MAX_BARS || p->bar[bar].size == 0 || p->bar[bar].io)
        return 0;
    struct resource r = { .type = RES_MMIO, .start = p->bar[bar].base, .size = p->bar[bar].size, .flags = bar };
    return device_map_mmio(&p->dev, &r);
}

bool pci_io_bar_usable(const struct pci_device *p, unsigned bar)
{
    return bar < PCI_MAX_BARS && p->bar[bar].io && p->bar[bar].size != 0 && arch_pci_io_available() &&
           p->bar[bar].base + p->bar[bar].size <= arch_pci_io_limit();   /* inside the port space */
}

static bool io_in_bar(const struct pci_device *p, unsigned bar, uint32_t off, unsigned width)
{
    return pci_io_bar_usable(p, bar) && (width == 1 || width == 2 || width == 4) &&
           (uint64_t)off + width <= p->bar[bar].size;
}

uint32_t pci_io_read(const struct pci_device *p, unsigned bar, uint32_t off, unsigned width)
{
    if (!io_in_bar(p, bar, off, width))
        return 0xFFFFFFFFu;
    return arch_pci_io_read((uint32_t)p->bar[bar].base + off, width);
}

void pci_io_write(const struct pci_device *p, unsigned bar, uint32_t off, unsigned width, uint32_t v)
{
    if (io_in_bar(p, bar, off, width))
        arch_pci_io_write((uint32_t)p->bar[bar].base + off, width, v);
}

uint8_t pci_find_capability(const struct pci_device *p, uint8_t id, uint8_t prev)
{
    if ((pci_cfg_read16(p, PCI_STATUS) & PCI_STATUS_CAP_LIST) == 0)
        return 0;
    uint8_t off = prev ? (pci_cfg_read8(p, (uint16_t)(prev + 1)) & 0xfc) : (pci_cfg_read8(p, PCI_CAP_PTR) & 0xfc);
    for (unsigned guard = 0; off >= 0x40 && guard < 48; guard++) {
        if (pci_cfg_read8(p, off) == id)
            return off;
        off = pci_cfg_read8(p, (uint16_t)(off + 1)) & 0xfc;
    }
    return 0;
}

/* MSI-X capability: control at +2 (table size in bits 0-10, function
 * mask bit 14, enable bit 15), table BIR/offset at +4. */
#define MSIX_CTRL_ENABLE  (1u << 15)
#define MSIX_CTRL_FMASK   (1u << 14)
#define MSIX_ENTRY_SIZE   16
#define MSIX_VECTOR_MASKED 1u

int pci_msix_enable(struct pci_device *p, unsigned want)
{
    if (p->cap_msix == 0)
        return -ENODEV;
    if (p->msix.table)
        return -EBUSY;
    uint16_t ctrl = pci_cfg_read16(p, (uint16_t)(p->cap_msix + 2));
    unsigned count = (ctrl & 0x7ff) + 1;
    uint32_t tbl = pci_cfg_read32(p, (uint16_t)(p->cap_msix + 4));
    unsigned bir = tbl & 7;
    uint32_t offset = tbl & ~7u;
    if (bir >= PCI_MAX_BARS || p->bar[bir].size == 0 || p->bar[bir].io ||
        (uint64_t)offset + (uint64_t)count * MSIX_ENTRY_SIZE > p->bar[bir].size)
        return -EIO;

    struct resource r = { .type = RES_MMIO, .start = p->bar[bir].base + offset,
                          .size = (uint64_t)count * MSIX_ENTRY_SIZE, .flags = bir };
    vaddr_t table = device_map_mmio(&p->dev, &r);
    if (table == 0)
        return -ENOMEM;
    int *vectors = kmalloc(count * sizeof(int), 0);
    if (vectors == NULL) {
        device_unmap_mmio(table);
        return -ENOMEM;
    }
    for (unsigned i = 0; i < count; i++) {
        vectors[i] = -1;
        volatile uint32_t *e = (volatile uint32_t *)(table + i * MSIX_ENTRY_SIZE);
        e[3] = MSIX_VECTOR_MASKED;
    }
    p->msix.table = table;
    p->msix.count = count;
    p->msix.vectors = vectors;

    /* Enable MSI-X with the function mask off; INTx off. */
    pci_cfg_write16(p, (uint16_t)(p->cap_msix + 2), (uint16_t)((ctrl & ~MSIX_CTRL_FMASK) | MSIX_CTRL_ENABLE));
    uint16_t cmd = pci_cfg_read16(p, PCI_COMMAND);
    pci_cfg_write16(p, PCI_COMMAND, cmd | PCI_COMMAND_INTX_OFF);
    unsigned granted = want < count ? want : count;
    kdebug("pci: %s: MSI-X enabled, %u of %u vectors", p->dev.name, granted, count);
    return (int)granted;
}

int pci_msix_request(struct pci_device *p, unsigned index, interrupt_handler_fn fn, void *arg,
                     const char *name, unsigned cpu)
{
    if (p->msix.table == 0 || index >= p->msix.count)
        return -EINVAL;
    if (p->msix.vectors[index] >= 0)
        return -EBUSY;
    struct irq_msi_msg msg;
    int vector = irq_request_msi(fn, arg, name, cpu, pci_requester_id(p), &msg);
    if (vector < 0)
        return vector;
    volatile uint32_t *e = (volatile uint32_t *)(p->msix.table + index * MSIX_ENTRY_SIZE);
    e[0] = (uint32_t)msg.addr;
    e[1] = (uint32_t)(msg.addr >> 32);
    e[2] = msg.data;
    e[3] = 0;   /* unmask */
    p->msix.vectors[index] = vector;
    return vector;
}

void pci_msix_release(struct pci_device *p, unsigned index)
{
    if (p->msix.table == 0 || index >= p->msix.count || p->msix.vectors[index] < 0)
        return;
    volatile uint32_t *e = (volatile uint32_t *)(p->msix.table + index * MSIX_ENTRY_SIZE);
    e[3] = MSIX_VECTOR_MASKED;
    irq_release_msi(p->msix.vectors[index]);
    p->msix.vectors[index] = -1;
}

void pci_msix_disable(struct pci_device *p)
{
    if (p->msix.table == 0)
        return;
    for (unsigned i = 0; i < p->msix.count; i++)
        pci_msix_release(p, i);
    uint16_t ctrl = pci_cfg_read16(p, (uint16_t)(p->cap_msix + 2));
    pci_cfg_write16(p, (uint16_t)(p->cap_msix + 2), (uint16_t)(ctrl & ~MSIX_CTRL_ENABLE));
    device_unmap_mmio(p->msix.table);
    kfree(p->msix.vectors);
    p->msix.table = 0;
    p->msix.vectors = NULL;
    p->msix.count = 0;
}

/* MSI capability: control at +2 (enable bit 0, 64-bit bit 7), address
 * at +4 (+8 upper if 64-bit), data after the address. */
#define MSI_CTRL_ENABLE (1u << 0)
#define MSI_CTRL_64BIT  (1u << 7)

int pci_msi_enable(struct pci_device *p, interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu)
{
    if (p->cap_msi == 0)
        return -ENODEV;
    if (p->msi_vector >= 0)
        return -EBUSY;
    struct irq_msi_msg msg;
    int vector = irq_request_msi(fn, arg, name, cpu, pci_requester_id(p), &msg);
    if (vector < 0)
        return vector;
    p->msi_vector = vector;
    uint16_t ctrl = pci_cfg_read16(p, (uint16_t)(p->cap_msi + 2));
    pci_cfg_write32(p, (uint16_t)(p->cap_msi + 4), (uint32_t)msg.addr);
    if (ctrl & MSI_CTRL_64BIT) {
        pci_cfg_write32(p, (uint16_t)(p->cap_msi + 8), (uint32_t)(msg.addr >> 32));
        pci_cfg_write16(p, (uint16_t)(p->cap_msi + 12), (uint16_t)msg.data);
    } else {
        pci_cfg_write16(p, (uint16_t)(p->cap_msi + 8), (uint16_t)msg.data);
    }
    ctrl = (uint16_t)((ctrl & ~0x70u) | MSI_CTRL_ENABLE);   /* one message */
    pci_cfg_write16(p, (uint16_t)(p->cap_msi + 2), ctrl);
    uint16_t cmd = pci_cfg_read16(p, PCI_COMMAND);
    pci_cfg_write16(p, PCI_COMMAND, cmd | PCI_COMMAND_INTX_OFF);
    return vector;
}

void pci_msi_disable(struct pci_device *p)
{
    if (p->cap_msi == 0)
        return;
    uint16_t ctrl = pci_cfg_read16(p, (uint16_t)(p->cap_msi + 2));
    pci_cfg_write16(p, (uint16_t)(p->cap_msi + 2), (uint16_t)(ctrl & ~MSI_CTRL_ENABLE));
    if (p->msi_vector >= 0) {
        irq_release_msi(p->msi_vector);
        p->msi_vector = -1;
    }
}

unsigned pci_device_count(void)
{
    return g_count;
}

struct pci_device *pci_device_at(unsigned index)
{
    return index < g_count ? g_by_index[index] : NULL;
}

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *after)
{
    unsigned start = 0;
    if (after) {
        for (unsigned i = 0; i < g_count; i++) {
            if (g_by_index[i] == after) {
                start = i + 1;
                break;
            }
        }
    }
    for (unsigned i = start; i < g_count; i++) {
        struct pci_device *p = g_by_index[i];
        if ((vendor == PCI_ANY || p->vendor == vendor) && (device == PCI_ANY || p->device == device))
            return p;
    }
    return NULL;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(pci_bus);
EXPORT_SYMBOL(pci_register_driver);
EXPORT_SYMBOL(pci_unregister_driver);
EXPORT_SYMBOL(pci_cfg_read8);
EXPORT_SYMBOL(pci_cfg_read16);
EXPORT_SYMBOL(pci_cfg_read32);
EXPORT_SYMBOL(pci_cfg_write8);
EXPORT_SYMBOL(pci_cfg_write16);
EXPORT_SYMBOL(pci_cfg_write32);
EXPORT_SYMBOL(pci_enable_device);
EXPORT_SYMBOL(pci_map_bar);
EXPORT_SYMBOL(pci_io_bar_usable);
EXPORT_SYMBOL(pci_io_read);
EXPORT_SYMBOL(pci_io_write);
EXPORT_SYMBOL(pci_find_capability);
EXPORT_SYMBOL(pci_msix_enable);
EXPORT_SYMBOL(pci_msix_request);
EXPORT_SYMBOL(pci_msix_release);
EXPORT_SYMBOL(pci_msix_disable);
EXPORT_SYMBOL(pci_msi_enable);
EXPORT_SYMBOL(pci_msi_disable);
EXPORT_SYMBOL(pci_device_count);
EXPORT_SYMBOL(pci_device_at);
EXPORT_SYMBOL(pci_find_device);
