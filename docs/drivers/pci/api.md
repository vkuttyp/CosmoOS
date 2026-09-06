# PCI: API

Header `drivers/include/drivers/pci.h`, implementation `drivers/pci/pci.c`.
**ABI stability: Module ABI v1** for everything marked *exported*;
kernel-internal otherwise. Thread context unless stated; configuration
accessors are safe in any context.

### `void pci_init(void)`
Purpose: register the `pci` bus, pick ECAM or legacy access, enumerate.
Needs `acpi_init`, `vmm_init`, `device_init`. Once, from `kernel_main`.
Panics only on out-of-memory during enumeration.

### `struct bus_type pci_bus` *(exported)*
The bus; `match` compares `struct pci_id` tables.

### `int pci_register_driver(struct pci_driver *pdrv)` *(exported)*
Purpose: fill the embedded `device_driver` (bus, match data, thunks) and
`driver_register` it, probing every unbound PCI function that matches.
Inputs: `pdrv->ids` (terminated by `PCI_ID_END`) and `probe` non-NULL;
`pdrv->drv.name` set. Outputs: 0 or `-EEXIST`. Sleeps.
`pci_unregister_driver` *(exported)* removes every bound function.

### `uint8_t pci_cfg_read8(const struct pci_device *, uint16_t off)`, `..._read16`, `..._read32`, `pci_cfg_write8/16/32` *(exported)*
Purpose: configuration space of one function. Offsets up to 4095 through
ECAM, 255 through the legacy mechanism (reads above return all ones,
writes are dropped). Any context.

### `void pci_enable_device(struct pci_device *pdev, bool bus_master)` *(exported)*
Set memory and I/O decode, and bus mastering when asked. With
`bus_master` it first attaches the device to an IOMMU domain of its own
(`iommu_attach_device`, requester id `bus << 8 | slot << 3 | func`); a
failure is logged and the device enabled anyway, its DMA then faulting
instead of reaching memory. Sleeps when it attaches (allocation),
otherwise any context.

### `vaddr_t pci_map_bar(struct pci_device *pdev, unsigned bar)` *(exported)*
Purpose: map a memory BAR uncached; 0 for an absent or I/O BAR or on
mapping failure. Sleeps (VMM). Unmap with `device_unmap_mmio`.

### `uint8_t pci_find_capability(const struct pci_device *pdev, uint8_t id, uint8_t prev)` *(exported)*
Purpose: offset of the next capability with `id` after `prev` (0 =
first), or 0. Bounded walk (48 entries). Any context.

### `int pci_msix_enable(struct pci_device *pdev, unsigned want)` *(exported)*
Purpose: map the MSI-X table, mask all entries, enable the function
with INTx disabled. Outputs: `min(want, table size)` (>= 1), `-ENODEV`
(no MSI-X), `-EBUSY` (already enabled), `-EIO` (table outside its BAR),
`-ENOMEM`. Sleeps.

### `int pci_msix_request(struct pci_device *pdev, unsigned index, interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu)` *(exported)*
Purpose: bind table entry `index` to `fn` on `cpu`; programs and unmasks
the entry. Outputs: the vector, `-EINVAL` (not enabled, bad index),
`-EBUSY` (entry in use), `-ENOSPC` (no vector). Takes the IRQ spinlock.
`pci_msix_release(pdev, index)` *(exported)* masks the entry and frees
the vector; `pci_msix_disable(pdev)` *(exported)* releases every entry,
disables MSI-X, unmaps the table. Sleeps (unmap).

### `int pci_msi_enable(struct pci_device *pdev, interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu)` *(exported)*
Single-message MSI: the vector or `-ENODEV`/`-ENOSPC`. INTx disabled.
`pci_msi_disable` *(exported)* clears the enable bit and releases the
vector recorded in `pdev->msi_vector`. A second `pci_msi_enable` while one
is active returns `-EBUSY`.

### `unsigned pci_device_count(void)`, `struct pci_device *pci_device_at(unsigned)`, `struct pci_device *pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *after)` *(exported)*
Enumeration-order access; `PCI_ANY` wildcards; `after` continues a
search. Plain pointers: the core owns every device forever. Any context.

### `bool pci_ecam_in_use(void)`
Diagnostics: which access mechanism was chosen.

### `struct pci_device`, `struct pci_bar`, `struct pci_id`, `struct pci_driver`
Layouts in `design.md`. `to_pci_device(struct device *)` recovers the
PCI structure from the model's device. Drivers read the decoded fields
and never re-parse configuration space.

### Architecture side (`kernel/include/arch/pci.h`)
`arch_pci_legacy_available()`, `arch_pci_legacy_read(bus, slot, func, off, width)`,
`arch_pci_legacy_write(...)`: mechanism #1 on x86
(`kernel/arch/x86_64/pci_legacy.c`), spinlocked; used only without an
MCFG. An architecture without such ports returns false and the core
runs without PCI.
