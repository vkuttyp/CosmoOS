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
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/string.h>
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
            b->size = ~m + 1;
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

/* PCI devices are enumerated once and never unregistered today; the
 * release exists so the object model's rule holds when hot-unplug does. */
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
    if (mcfg == NULL)
        return;
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
}

/* --- driver services ---------------------------------------------------- */

void pci_enable_device(struct pci_device *p, bool bus_master)
{
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
    int vector = irq_request_msi(fn, arg, name, cpu, &msg);
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
    int vector = irq_request_msi(fn, arg, name, cpu, &msg);
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
