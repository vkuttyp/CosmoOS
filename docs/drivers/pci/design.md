# PCI: design

## Data structures (`drivers/include/drivers/pci.h`)

```c
struct pci_bar { uint64_t base, size; bool io, is64, prefetch; };   /* size 0: unimplemented */
struct pci_msix_state { vaddr_t table; unsigned count; int *vectors; };
struct pci_device {
    struct device dev;                     /* "pci:BB:DD.F", on pci_bus */
    uint8_t bus, slot, func;
    uint16_t vendor, device, subsys_vendor, subsys_id;
    uint8_t class, subclass, prog_if, revision, header_type, irq_pin;
    struct pci_bar bar[6];
    uint8_t cap_msi, cap_msix;             /* capability offsets, 0 if absent */
    struct pci_msix_state msix;
    struct list_node link;                 /* enumeration order */
};
struct pci_id { uint16_t vendor, device; uint8_t class, subclass; unsigned flags; };  /* PCI_ANY, PCI_ID_CLASS, PCI_ID_END */
struct pci_driver {
    struct device_driver drv;              /* name set by the driver; bus/match/probe/remove set by the core */
    const struct pci_id *ids;
    int (*probe)(struct pci_device *, const struct pci_id *);
    void (*remove)(struct pci_device *);
};
```

`pci.c` keeps `g_by_index[PCI_MAX_DEVICES]` (64) and a list in
enumeration order; the core owns every `pci_device` for the life of the
kernel (`kzalloc`ed during `pci_init`, never freed), so index lookups
return plain pointers, not references.

## Configuration access

`cfg_read`/`cfg_write` dispatch on `g_ecam`: when set, the address is
`ecam + ((bus - start_bus) << 20 | slot << 15 | func << 12 | off)` and
the access is a volatile 8/16/32-bit load or store; buses outside the
MCFG range read as all ones. Otherwise `arch_pci_legacy_read/write`
(x86: `0xCF8` address, `0xCFC` data, one spinlock, offsets below 256).
`setup_ecam` maps the first segment-0 allocation of the MCFG with
`vm_map_phys(base, buses << 20, RW, UC)`: on q35 that is `0xE0000000`
for buses 0 to 255 (256 MiB of virtual space, no RAM).

## Enumeration

`scan_bus(0)`: for each slot, read the id dword of function 0; skip
`0xffff`; read the header type; functions 1 to 7 only when bit 7
(multi-function) is set. `scan_function` fills the structure, decodes
BARs, walks capabilities, registers the device (a probe may run right
away if a driver is already registered, but at `pci_init` none is), and
recurses into `PCI_SECONDARY_BUS` for type 1 headers, depth-limited to
8. `PCI_MAX_DEVICES` excess functions are logged and ignored.

BAR decoding (`decode_bars`): memory and I/O decode are cleared in the
command register for the duration; for each BAR the original value is
saved, all ones written, the mask read back, the original restored. Bit
0 selects I/O (`size = ~(mask & ~3) + 1` within 16 bits); for memory
BARs bits 1-2 select 64-bit (the next BAR holds the upper half and is
probed the same way), bit 3 prefetchable, `size = ~(mask & ~0xf) + 1`.
Each implemented BAR becomes a `RES_MMIO`/`RES_IO` resource with the BAR
index in `flags`.

Capabilities (`find_capabilities`): if status bit 4 is set, follow the
list from `PCI_CAP_PTR` with a 48-step guard and offsets forced to
multiples of 4 and at least `0x40`; the first MSI and MSI-X capability
offsets are cached. `pci_find_capability(id, prev)` walks the same list
on demand for vendor-specific capabilities (virtio uses several).

## Driver binding

The device model calls `drv->probe(struct device *)`. The core installs
`pci_probe_thunk` there and, because the model sets `dev->driver` only
after a successful probe, the thunk re-runs `pci_match` over the bus's
registered PCI drivers (those whose probe is the thunk) to find the
`pci_driver` and the matching `pci_id`, then calls the driver's typed
probe. `pci_remove_thunk` uses `dev->driver` directly. Match rules:
vendor and device each equal or `PCI_ANY`; with `PCI_ID_CLASS` also
class and subclass equal. A table ends with `PCI_ID_END`
(`flags == 0xffffffff`).

## Interrupts

MSI-X (`pci_msix_enable(pdev, want)`): reads the table size from the
capability's control word, the table BIR/offset from `+4`, checks they
fit in a memory BAR, maps exactly the table (`device_map_mmio` on a
synthetic resource), masks every entry (vector control bit 0), clears
the function mask, sets the enable bit, and sets `PCI_COMMAND_INTX_OFF`.
It returns `min(want, table size)`. `pci_msix_request(pdev, index, fn,
arg, name, cpu)` calls `irq_request_msi`, writes address low/high and
data into the entry, and unmasks it; `pci_msix_release` masks the entry
and releases the vector; `pci_msix_disable` releases everything, clears
the enable bit, unmaps the table. MSI (`pci_msi_enable`) programs a
single message (64-bit address form when the capability says so) and
sets the enable bit with the multiple-message field cleared.

The message itself is opaque to this file: `irq_request_msi` returns it
from `arch_irqc_msi_compose`.

## Ownership and lifetime

`pci_device` objects: the core, forever. The ECAM mapping: the core,
forever. MSI-X table mappings and vector arrays: created by
`pci_msix_enable`, freed by `pci_msix_disable`, both called by the
bound driver (virtio-pci does so in probe/remove). BAR mappings made by
`pci_map_bar` belong to the caller (`device_unmap_mmio`).

## Concurrency

Configuration access is stateless through ECAM and spinlocked through
the legacy ports. Enumeration runs once, single threaded, before boot
modules load. Driver registration and probing are under the device
model lock. MSI-X entry programming happens in the driver's probe or
queue setup, thread context. There is no per-device lock; a driver
serialises its own configuration writes.

## Memory

~200 bytes per function plus the `struct device` inside it; one 256 MiB
virtual (not physical) window for ECAM; one page-rounded UC mapping per
enabled MSI-X table and per mapped BAR.

## Error handling

Missing MCFG falls back to legacy access with a log line; neither
available leaves PCI empty with a warning. Enumeration never fails a
boot: allocation failure panics (out of memory this early is fatal),
registration failure is logged. Service functions return `-ENODEV` (no
capability), `-EBUSY` (MSI-X already enabled, entry in use), `-EIO`
(table outside its BAR), `-ENOMEM`, `-EINVAL`, or the vector allocator's
`-ENOSPC`.

## Performance

Enumeration touches every slot on every reachable bus once (256 slots
per bus × a handful of reads); on q35 it takes a few milliseconds under
TCG. ECAM accesses are single uncached loads.

## Security

Only functions the firmware placed are trusted for their BAR addresses;
sizes are re-derived by probing. MMIO is mapped uncached, kernel only.
Bus mastering is enabled only by a driver's explicit
`pci_enable_device(pdev, true)`, which first calls
`iommu_attach_device` with the requester id (`bus << 8 | slot << 3 |
func`), so the device has its own address space before it can master
the bus (`docs/kernel/iommu/`). On a machine with no IOMMU unit the
device can still write any physical address, which is why DMA
addresses only ever come from `dma_alloc`/`dma_map`.

## Future extensibility

Extended capabilities (offsets above 256 through ECAM), INTx via a
static routing table or AML, resource assignment for unprogrammed BARs,
multiple MCFG segments, SR-IOV, hot-plug (`device_unregister` exists),
MSI affinity across CPUs.
