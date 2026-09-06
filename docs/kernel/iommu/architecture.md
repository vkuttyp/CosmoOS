# IOMMU: Architecture

The unit the audit's plan names after networking
(`docs/audit/2026-09-post-roadmap-audit.md` §10.3 "IOMMU readiness",
§19 "After these"; the specification's Prompt #2 §25). It gives the
kernel a DMA-remapping layer: every bus-mastering device gets its own
address space, the buffers a driver maps are the only memory the device
can reach, and a stray or hostile DMA faults instead of writing kernel
memory.

## Where it sits

```text
   drivers (virtio-pci, nvme)     dma_alloc / dma_map / dma_unmap / dma_free   (unchanged API)
                                              │
   kernel/device/dma.c            bus address = IOVA when the device has a domain, else physical
                                              │
   kernel/iommu/iommu.c           domains, per-device attach, the IOVA allocator, fault counters
   kernel/iommu/pt.c              the 4-level, 4 KiB-granule page table walker (format per driver)
                                              │
   drivers/iommu/intel_vtd.c      Intel VT-d: DMAR, root/context tables, second-level paging,
                                  register-based invalidation, fault recording (x86-64, q35)
   drivers/iommu/arm_smmuv3.c     ARM SMMUv3: stream table, stage-2 translation, command and
                                  event queues (AArch64, the unit the ACPI IORT names)
```

The IOMMU layer is kernel code, initialised after ACPI and the PCI
enumeration and before the boot modules load, so that the first driver
to enable bus mastering finds translation already on. Drivers see
nothing new: the DMA API they used since Phase 6 returns a bus address
that is now an I/O virtual address, and the discipline milestone 9
imposed (every map has its unmap, every alloc its free) is what makes
the mappings' lifetimes correct.

## Purpose

- **Isolation.** A device addresses only what its driver mapped for it,
  for the duration of the mapping. Kernel text, page tables, other
  devices' rings and user memory are unreachable. This is the
  structural answer to the class of bugs the audit found in the virtqueue
  (a device rewriting descriptors the driver trusted) and the
  precondition for driver modules with less than full trust, for device
  assignment to virtual machines, and for user-space drivers.
- **Fault visibility.** A translation fault is recorded and counted with
  the requester and the address; it never corrupts memory.
- **Portability.** One abstraction over two very different pieces of
  hardware, both present in the test machines QEMU provides.

## Responsibilities

- Discover the IOMMU units (VT-d from the ACPI `DMAR` table; the virt
  machine's SMMUv3 at its fixed address), map their registers, bring
  them up with translation enabled for every device they cover.
- Domains: allocation, an identifier, a root page table, an IOVA
  allocator; attach a device (its requester id) to a domain; detach.
- Map and unmap pages with read/write permission; invalidate the
  IOTLB after unmapping; a lookup for tests and diagnostics.
- Serve `dma_alloc`/`dma_map`/`dma_unmap`/`dma_free` for a device with
  a domain, in any context the DMA API allows (interrupt handlers
  included), and leave the identity path for devices without one.
- Report faults through the unit's interrupt: decode, count, log at a
  bounded rate.

## Non-responsibilities

- Interrupt remapping (QEMU is run with `intremap=off`; the MSI
  composition hook the audit asked for stays a note).
- Device assignment to guests and user-space drivers: enabled by this
  layer, built later.
- Shared virtual addressing, PASIDs, nested translation, ATS/PRI,
  huge-page mappings, an IOVA cache: none are needed by the tested
  devices at the tested scale.
- AMD-Vi: designed for (the abstraction is the same shape), not
  implemented; no test machine offers it.
- Bounce buffers: every buffer the drivers map is direct-map memory
  the IOMMU can address.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `iommu_init`, `iommu_attach_device`, `iommu_detach_device`, `iommu_domain_*`, `iommu_map/unmap/lookup`, `iommu_get_stats` | `kernel/iommu.h` | `dma.c`, `pci.c`, the drivers, tests |
| `struct iommu_ops`, `iommu_register_unit` | `kernel/iommu.h` | `intel_vtd.c`, `arm_smmuv3.c` |
| `iommu_pt_*` | `kernel/iommu_pt.h` | the two drivers |
| `dma_*` (unchanged) | `kernel/dma.h` | every DMA driver |
