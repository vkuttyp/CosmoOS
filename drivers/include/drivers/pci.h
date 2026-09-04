/*
 * pci.h - PCI bus: configuration access, enumeration, BARs, capabilities,
 * MSI/MSI-X. Drivers bind through the "pci" bus with a struct pci_id
 * table; they never touch configuration space layout themselves
 * (constitution section 25).
 */

#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <kernel/device.h>
#include <kernel/interrupt.h>
#include <kernel/list.h>
#include <kernel/types.h>

#define PCI_ANY 0xffffu

/* Configuration space offsets and bits the core uses (spec 3.0 chapter 6). */
#define PCI_VENDOR_ID     0x00
#define PCI_DEVICE_ID     0x02
#define PCI_COMMAND       0x04
#define PCI_STATUS        0x06
#define PCI_REVISION      0x08
#define PCI_CLASS         0x09   /* prog_if, subclass, class at 0x09..0x0b */
#define PCI_HEADER_TYPE   0x0e
#define PCI_BAR0          0x10
#define PCI_SUBSYS_VENDOR 0x2c
#define PCI_SUBSYS_ID     0x2e
#define PCI_CAP_PTR       0x34
#define PCI_IRQ_LINE      0x3c
#define PCI_IRQ_PIN       0x3d
#define PCI_SECONDARY_BUS 0x19

#define PCI_COMMAND_IO         (1u << 0)
#define PCI_COMMAND_MEMORY     (1u << 1)
#define PCI_COMMAND_MASTER     (1u << 2)
#define PCI_COMMAND_INTX_OFF   (1u << 10)
#define PCI_STATUS_CAP_LIST    (1u << 4)

#define PCI_CAP_ID_MSI    0x05
#define PCI_CAP_ID_VENDOR 0x09
#define PCI_CAP_ID_MSIX   0x11

#define PCI_MAX_BARS 6

struct pci_bar {
    uint64_t base;
    uint64_t size;      /* 0: unimplemented */
    bool io;
    bool is64;
    bool prefetch;
};

struct pci_msix_state {
    vaddr_t table;      /* mapped MSI-X table, 0 until enabled */
    unsigned count;     /* table size */
    int *vectors;       /* per entry, -1 when unused */
};

struct pci_device {
    struct device dev;              /* name "pci:BB:DD.F" */
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint16_t subsys_vendor, subsys_id;
    uint8_t class, subclass, prog_if, revision;
    uint8_t header_type;
    uint8_t irq_pin;
    struct pci_bar bar[PCI_MAX_BARS];
    uint8_t cap_msi, cap_msix;      /* capability offsets, 0 if absent */
    struct pci_msix_state msix;
    int msi_vector;                 /* single-message MSI vector, -1 when off */
    struct list_node link;          /* all PCI devices, enumeration order */
};

static inline struct pci_device *to_pci_device(struct device *dev)
{
    return container_of(dev, struct pci_device, dev);
}

#define PCI_ID_CLASS (1u << 0)      /* match class/subclass too */
struct pci_id {
    uint16_t vendor, device;        /* PCI_ANY wildcards */
    uint8_t class, subclass;
    unsigned flags;
};
#define PCI_ID_END { 0, 0, 0, 0, 0xffffffffu }

struct pci_driver {
    struct device_driver drv;
    const struct pci_id *ids;       /* terminated by PCI_ID_END */
    int (*probe)(struct pci_device *pdev, const struct pci_id *id);
    void (*remove)(struct pci_device *pdev);
};

/* Enumerate every bus and register the devices. Needs ACPI (for MCFG),
 * the VMM (for ECAM mapping) and the device model. Once. */
void pci_init(void);
extern struct bus_type pci_bus;

/* Register/unregister a driver; probes existing devices. Sleeps. */
int pci_register_driver(struct pci_driver *pdrv);
void pci_unregister_driver(struct pci_driver *pdrv);

/* Configuration access. Any context; a spinlock in the legacy path. */
uint8_t  pci_cfg_read8(const struct pci_device *pdev, uint16_t off);
uint16_t pci_cfg_read16(const struct pci_device *pdev, uint16_t off);
uint32_t pci_cfg_read32(const struct pci_device *pdev, uint16_t off);
void pci_cfg_write8(const struct pci_device *pdev, uint16_t off, uint8_t v);
void pci_cfg_write16(const struct pci_device *pdev, uint16_t off, uint16_t v);
void pci_cfg_write32(const struct pci_device *pdev, uint16_t off, uint32_t v);

/* Set command register bits (memory/IO decode, bus mastering). */
void pci_enable_device(struct pci_device *pdev, bool bus_master);

/* Map a memory BAR uncached. 0 if the BAR is absent or an I/O BAR.
 * Sleeps (VMM). Unmap with device_unmap_mmio. */
vaddr_t pci_map_bar(struct pci_device *pdev, unsigned bar);

/* Offset of the next capability with `id` after `prev` (0 = first), or 0. */
uint8_t pci_find_capability(const struct pci_device *pdev, uint8_t id, uint8_t prev);

/* MSI-X: map the table, mask every entry, enable the function. `want` is
 * the number of vectors the driver needs; the result is min(want,
 * table size) or a negative errno (-ENODEV without MSI-X). Sleeps. */
int pci_msix_enable(struct pci_device *pdev, unsigned want);
/* Route entry `index` to a handler on `cpu`; returns the vector. */
int pci_msix_request(struct pci_device *pdev, unsigned index, interrupt_handler_fn fn, void *arg,
                     const char *name, unsigned cpu);
void pci_msix_release(struct pci_device *pdev, unsigned index);
void pci_msix_disable(struct pci_device *pdev);

/* Single-message MSI fallback: returns the vector or a negative errno. */
int pci_msi_enable(struct pci_device *pdev, interrupt_handler_fn fn, void *arg, const char *name,
                   unsigned cpu);
void pci_msi_disable(struct pci_device *pdev);

/* Enumeration order walk (referenced pointers are not handed out: the
 * PCI core owns every device for the life of the kernel). */
unsigned pci_device_count(void);
struct pci_device *pci_device_at(unsigned index);
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *after);
bool pci_ecam_in_use(void);

#endif /* DRIVERS_PCI_H */
