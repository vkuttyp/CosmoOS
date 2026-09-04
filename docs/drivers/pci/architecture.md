# PCI: architecture

Constitution section 25 (Bus, Resource, Interrupt; "driver code should
not contain generic PCI logic") and section 17 (MSI/MSI-X behind the
interrupt abstraction). Phase 6.

## Where it sits

```text
   drivers (modules)   virtio-pci transport, later NVMe/AHCI/network
                            │  struct pci_driver, pci_* services
   drivers/pci/pci.c   ───────┼──────────────────────────────────────
     config access     ECAM window (ACPI MCFG)  |  arch_pci_legacy_* (0xCF8)
     enumeration       bus 0 → functions → bridges → secondary buses
     the "pci" bus     match on vendor/device/class id tables
     BARs, caps        sizing, capability list, MSI-X table mapping
     interrupts        irq_request_msi() ← arch_irqc_msi_compose()
   kernel/device       device model (registration, probing, MMIO maps)
   kernel/memory       vm_map_phys for ECAM and BARs (uncached)
   drivers/acpi        acpi_find_table("MCFG")
```

The PCI core is kernel code (not a module): drivers are modules and
need the bus to exist before they load. It lives under `drivers/`
because it is a bus driver, with its public header in
`drivers/include/drivers/pci.h` (the `drivers/include` path is on the
kernel and module include paths).

## Purpose

Find every PCI function, describe each one as a `struct device` with
decoded resources, let drivers claim functions by id, and provide the
services a driver must not implement itself: configuration access,
BAR mapping, capability lookup, command-register control, and
message-signalled interrupts.

## Responsibilities

- Choose the configuration mechanism at init: ECAM if ACPI's MCFG lists
  segment 0 (mapped once, uncached, for the whole bus range), else the
  architecture's legacy mechanism.
- Enumerate depth first from bus 0, following type 1 headers into their
  secondary bus, recording vendor/device/class/revision/header type,
  subsystem ids, capability offsets (MSI, MSI-X), and the six BARs with
  sizes obtained by the all-ones probe (decode disabled meanwhile).
- Register each function as `pci:BB:DD.F` on the `pci` bus; `match`
  drivers by `struct pci_id` tables (vendor/device with `PCI_ANY`
  wildcards, optionally class/subclass).
- Translate the device model's `probe(struct device *)` into
  `probe(struct pci_device *, const struct pci_id *)`.
- Services: `pci_cfg_*`, `pci_enable_device` (memory/IO decode, bus
  master), `pci_map_bar`, `pci_find_capability`, `pci_msix_enable/
  request/release/disable`, `pci_msi_enable/disable`, lookup by index or
  id.

## Non-responsibilities

- Legacy INTx routing (needs ACPI `_PRT`, hence AML): not supported; a
  function without MSI or MSI-X gets no interrupt.
- Resource assignment: BARs are taken as the firmware programmed them;
  nothing is reassigned. Bridges' windows are not managed.
- Hot-plug, power management, PCIe extended capabilities (the 4 KiB
  ECAM space is reachable, the extended capability list is not walked),
  SR-IOV, ATS, multiple segments (only segment 0 of the MCFG is used).
- Device-class knowledge: the core never interprets a BAR's contents.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `pci_init`, `pci_bus` | `drivers/pci.h` | `kernel_main`, the model |
| `struct pci_driver`, `pci_register_driver/unregister_driver` | `drivers/pci.h` | virtio-pci and future drivers |
| `pci_cfg_read8/16/32`, `pci_cfg_write8/16/32` | `drivers/pci.h` | drivers, self-test |
| `pci_enable_device`, `pci_map_bar`, `pci_find_capability` | `drivers/pci.h` | drivers |
| `pci_msix_enable/request/release/disable`, `pci_msi_enable/disable` | `drivers/pci.h` | drivers |
| `pci_device_count/at`, `pci_find_device`, `pci_ecam_in_use` | `drivers/pci.h` | self-test, diagnostics |
| `arch_pci_legacy_available/read/write` | `arch/pci.h` | the core, x86 only |
| `irq_request_msi`, `irq_release_msi` | `kernel/irq.h` | the core |
