# Device infrastructure: architecture

Constitution sections 25 (driver architecture: Device, Driver, Bus,
Resource, DMA, Interrupt), 26 (DMA abstraction), 27 (VirtIO as a normal
device subsystem with a generic transport and reusable virtqueues), 28
(the block layer is independent of filesystems), 17 (MSI/MSI-X behind the
architecture-neutral interrupt abstraction), and the Phase 6 roadmap
entry: PCI, DMA, VirtIO, block devices, console.

## Where it sits

```text
   modules (boot archive)      virtio_blk.ko   virtio_rng.ko   virtio_console.ko
                                    │               │               │
                               virtio.ko  (core, virtqueues, virtio-pci transport)
                                    │
   kernel ───────────────────────────┼──────────────────────────────────────────
   kernel/device/    device.c   device model: bus, device, driver, resources, probing
                     dma.c      dma_alloc/free/map/unmap/sync, per-device DMA mask
   drivers/pci/      pci.c      ECAM (ACPI MCFG) or legacy config, enumeration,
                                BARs, capabilities, MSI/MSI-X, the "pci" bus
   kernel/interrupt/ irq.c      irq_request_msi(): a vector plus an MSI message
   kernel/block/     blk.c      block devices, bio submission, synchronous helpers
   kernel/core/      random.c   entropy pool fed by hardware sources
                     console.c  sinks (serial, virtio-console)
   kernel/arch/x86_64/          pci_legacy.c (0xCF8/0xCFC), MSI message composition
```

The device model and the buses are kernel code: they are what a driver
is written against. The VirtIO stack and every VirtIO device driver are
kernel modules loaded from the boot archive, in dependency order; they
reach the kernel only through exported symbols (Module ABI v1 grows by
the device, PCI, DMA, IRQ, block, console, and random exports listed in
`docs/kernel/module/api.md`). That split is deliberate: Phase 5 built
the module loader so that Phase 6 could put the first real drivers
outside the image.

## Purpose

Discover hardware, describe it uniformly (a device on a bus with
resources), bind drivers to it, give drivers the three things they need
that the core must own (interrupt vectors, DMA-able memory, MMIO
mappings), and expose what the drivers produce (block devices, a
console sink, entropy) to the rest of the kernel through interfaces
that know nothing about PCI or VirtIO.

## Responsibilities

- **Device model** (`kernel/include/kernel/device.h`): `struct bus_type`,
  `struct device` (a `kobject`, a name, a parent, a bus, resources, the
  bound driver and its private data), `struct device_driver` with
  `probe`/`remove` and a bus-specific match. Registration of a device
  probes the registered drivers of its bus; registration of a driver
  probes the bus's unbound devices. Unregistration of a driver removes
  it from every device it bound (module unload). One mutex serialises
  it all.
- **Resources**: `struct resource` (`RES_MMIO`, `RES_IO`, `RES_IRQ`)
  per device, filled by the bus, mapped by the core on request
  (`device_map_mmio` uses `vm_map_phys` with `VM_CACHE_UC`).
- **PCI** (`drivers/include/drivers/pci.h`, `drivers/pci/pci.c`):
  configuration access through ECAM when ACPI provides an MCFG, else
  through the architecture's legacy mechanism; recursive enumeration
  over bridges; `struct pci_device` with vendor/device/class, six decoded
  BARs, capability offsets; enable memory/IO/bus-master; MSI-X (and MSI
  fallback) allocation that hands the driver kernel IRQ handles.
  Driver code never touches config space layout (section 25: "driver
  code should not contain generic PCI logic").
- **MSI/MSI-X in the interrupt layer**: `irq_request_msi()` allocates a
  vector on a CPU, registers the handler, and returns the address/data
  message the bus programs. Generic code sees a vector; only
  `arch_irqc_msi_compose()` knows the APIC format.
- **DMA** (`kernel/include/kernel/dma.h`): `dma_alloc`/`dma_free`
  (coherent, physically contiguous, zone chosen by the device's DMA
  mask), `dma_map`/`dma_unmap` (direct-map addresses only in this
  phase), `dma_sync_for_device`/`dma_sync_for_cpu` (barriers), and
  `dma_set_mask`. No driver computes a bus address itself.
- **Block layer** (`kernel/include/kernel/blk.h`): `struct blkdev`
  registered by a driver with capacity, sector size, and a `submit`
  operation; `struct bio` for asynchronous requests with a completion
  callback; `blk_read`/`blk_write` synchronous helpers on top; lookup
  by name (`vda`). Filesystems (Phase 7) sit above this and never see a
  driver.
- **Console and entropy**: `console_register()` already accepts extra
  sinks; the virtio-console driver adds one. `random.c` keeps a
  SHA-512-based pool: `random_add_entropy()` from virtio-rng,
  `random_get_bytes()` for everyone else.
- **VirtIO** (`drivers/include/drivers/virtio.h`, `drivers/virtio/`, module
  `virtio`): the device abstraction (features, status, config space,
  queues), split virtqueues reusable by every device type, the
  virtio-pci modern transport (vendor capabilities, MSI-X per queue),
  and the "virtio" bus that the device drivers match on by device id.
  Drivers: `virtio_blk` (a `blkdev`), `virtio_rng` (entropy),
  `virtio_console` (a console sink). `virtio_net` is deferred to the
  networking phase; the transport already handles it.

## Non-responsibilities

- ACPI interpretation beyond static tables: no AML, so no `_PRT`
  routing of legacy INTx. Every PCI driver uses MSI/MSI-X; a device
  without either is enumerated but gets no interrupt.
- IOMMU, scatter/gather, bounce buffers, non-coherent architectures:
  designed for (the API takes a device and a direction, addresses are
  `dma_addr_t` not `paddr_t`, the sync points call `arch_dma_barrier`),
  not implemented. Both supported platforms (QEMU `q35` and `virt`) are
  DMA-coherent; cache maintenance for a non-coherent bus is out of
  scope. `dma_map` refuses anything not physically contiguous.
- Hot-plug and power management: devices are enumerated once at boot;
  `device_unregister` exists for module unload, not for surprise
  removal.
- NVMe, AHCI, USB, network: later phases. The boot disk stays on the
  firmware's AHCI controller and is untouched.
- A page cache, partitions, or a filesystem: Phase 7.
- Console input from virtio-console (no reader exists yet) and
  serial input.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `bus_register`, `device_register/unregister`, `driver_register/unregister`, `device_map_mmio`, `device_for_each`, `device_dump` | `kernel/device.h` | buses, drivers, self-tests |
| `dma_alloc/free/map/unmap/sync_*`, `dma_set_mask` | `kernel/dma.h` | drivers |
| `irq_request_msi`, `irq_release_msi` | `kernel/irq.h` | PCI core |
| `arch_irqc_msi_compose` | `arch/irqc.h` | irq.c |
| `arch_pci_legacy_read/write` | `arch/pci.h` | PCI core |
| `pci_*` (config, BARs, capabilities, MSI-X, enable) | `drivers/pci.h` | virtio-pci transport, self-tests |
| `blk_register/unregister/find`, `blk_submit`, `blk_read/write` | `kernel/blk.h` | virtio_blk, self-tests, Phase 7 |
| `random_add_entropy`, `random_get_bytes`, `random_entropy_bits` | `kernel/random.h` | virtio_rng, everyone |
| `virtio_*`, `virtq_*` | `drivers/virtio.h` | virtio device drivers |
| `console_register` | `kernel/console.h` | virtio_console |

Tools and tests: `scripts/qemu-run.sh` now attaches a scratch virtio-blk
disk, virtio-rng, and a virtio-console whose output the boot test reads
back; self-tests `pci`, `dma`, `blk`, `random`, `virtio-console`.
