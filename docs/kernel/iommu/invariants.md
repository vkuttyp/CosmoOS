# IOMMU: invariants

Rules the DMA-remapping layer and its two unit drivers must keep. Each
has a **Check** (what verifies it today) and a **Gap** (what does not).
Changing a rule means changing this file and the code together.

**IOM1. Translation is on before any device is attached.** A unit
driver enables translation at the end of its probe, with no device in a
domain: from that instant a DMA by a device the unit covers and has no
entry for is refused (VT-d: no context entry once `GSTS.TES` is set;
SMMUv3: `GBPA` leaves an unmatched stream aborting, and `Config` in an
absent STE is not bypass). There is no window in which a device masters
the bus untranslated. Check: `iommu_init` runs after `pci_init` and
before `module_load_boot`, so no driver has enabled bus mastering yet;
the boot marker `iommu: <unit> ...; translation on` is required on both
machines by `tests/boot/run_boot_test.py`, and `QEMU_IOMMU=0` covers the
other side (no unit, no marker, the identity path). Gap: firmware-programmed
DMA before the kernel starts is not stopped (no
"IOMMU-enabled-by-firmware" handoff); nothing verifies the absence of a
context entry from the kernel's side.

**IOM2. A bus-mastering device has a domain, or its DMA faults.**
`pci_enable_device(p, true)` calls `iommu_attach_device` **before** it
sets `PCI_COMMAND_MASTER`; a failure is logged at `ERROR` and the
device is still enabled — the isolation guarantee then does the work of
the failure path, since every DMA that device attempts faults instead of
reaching memory. A failure that leaves the unit's entry published
(`-EIO`, an unconfirmed invalidation) keeps the domain and the device's
pointer to it: destroying a domain whose tables the hardware still
names would be worse than the fault it was reporting. Check: the boot marker for the per-device attach line
(`iommu: <unit>: pci:bb:ss.f (requester xxxx) in domain N`) and 5 (x86)
and 6 (AArch64) such lines in a normal boot; every device self-test
(virtio-blk, virtio-net, virtio-rng, NVMe, cosmofs on NVMe) runs
through translation. Gap: no test forces an attach failure at boot;
non-PCI bus-mastering devices do not exist yet, and nothing would
attach them.

**IOM3. A device reaches exactly the pages its driver mapped, for as
long as they are mapped.** Every bus address a driver holds comes from
`dma_alloc`/`dma_map`, which map with the direction's permission
(`DMA_TO_DEVICE` read-only), and `dma_unmap`/`dma_free` remove the
mapping. Nothing else maps into a device's domain except the unit's
reserved ranges (IOM7). Check: the `iommu` self-test walks the tables
back (`iommu_lookup`) for every mapping it makes through the DMA API,
including the page offset; the provoked fault shows that an address
outside the mappings is refused by the hardware. Gap: the walker's view
and the hardware's are compared only indirectly (a lookup is software);
no test reads a mapped page from the device side to prove permissions
(a read-only mapping written by a device is not exercised).

**IOM4. Nothing in the mapping path sleeps.** `iommu_map`,
`iommu_unmap`, `iommu_lookup`, `iommu_dma_map` and `iommu_dma_unmap`
take the domain's IRQ-safe spinlock and allocate page-table pages from
the PMM, which is IRQ-safe; they are called from completion handlers
(virtio-net reposts receive buffers from its callback). No kmalloc, no
mutex, no wait. Check: lockdep records the edges
(`iommu-domain -> DMA32/Normal/vtd`) and would report a sleep under a
spinlock; the network and NVMe tests drive the path from interrupt
context on every boot. Gap: the SMMU's command queue is polled under
the domain lock with a bounded spin — an unresponsive unit spends that
bound with interrupts off.

**IOM5. Lock order: the device-model lock → the domain lock → the
unit's own lock → the PMM zone locks.** A domain's lock is taken by the
DMA API and by the unit driver's map/unmap; the driver's lock (VT-d's
register lock, the SMMU's command-queue lock) is taken under it; the
IOMMU registry lock (`g_lock`) is a leaf taken only for the unit and
domain lists. No path takes them the other way; a fault handler takes
none of them. Check: lockdep on every debug boot (the edges above);
`iommu_note_fault` touches only atomics. Gap: the registry lock and a
domain lock are never held together, which lockdep can only confirm for
paths that ran.

**IOM6. Nothing is reused until the unit has confirmed that it stopped
translating it.** `iommu_dma_unmap` calls the driver's `unmap`, which
clears the leaves **and** invalidates (VT-d: domain-selective
`IOTLB_REG`; SMMUv3: `CMD_TLBI_S2_IPA` per page plus `CMD_SYNC`), under
the domain's lock, before `iova_free` returns the range to the
allocator. When the unit does *not* confirm — a VT-d invalidation that
never clears its pending bit, an SMMU command queue that stays full or
a `CMD_SYNC` that never drains — the operation returns `-EIO` and
nothing is recycled: the IOVAs are retired for the life of the domain
(`iommu_stats.retired`), `dma_free` leaks the frames instead of
returning them to the PMM (`dma_stats.leaked`), a failed detach keeps
the domain and its id rather than freeing tables a device may still be
walking, and a failed attach keeps the domain the unit's live entry
names. Leaking is the cheap half of that trade; handing a live
translation to the next allocation is the expensive half. `dma_unmap`
is the one case with nothing to withhold — the buffer is the caller's
and goes back to `kmalloc` or the page cache immediately — so there it
is a panic: the kernel cannot uphold IOM3 for that memory and will not
pretend to. Check:
the `iommu` self-test requires `retired` to be unchanged by a boot that
maps and unmaps thousands of times (the network and cosmofs workloads
recycle the low pages of the window constantly), and every failure path
logs at `ERROR`. Gap: the failure itself is not injectable — no test
makes a unit refuse to invalidate, so the retire-and-leak path is
reviewed, not exercised. Neither unit is asked whether it caches
not-present entries (both are configured so it does not matter: VT-d
`CAP.CM = 0`, the SMMU walks on a miss).

**IOM7. The unit's reserved ranges are never handed out, and an
identity range translates to itself in every domain.** MSI doorbells are
device writes: the virt machine's GICv2m frame (`0x0802_0000`) is
identity-mapped in every SMMU domain, and the x86 APIC window
(`0xFEE0_0000` + 1 MiB) is kept out of the allocator although VT-d
interprets it rather than translating it. `iova.reserved` counts those
pages, and "the domain leaked mappings" is a comparison against it.
Check: the `iommu` self-test asks the unit for its ranges and requires
each to be unmapped or identity-mapped as declared; interrupts arriving
at all on AArch64 is the end-to-end check. The SMMU takes the frame's
address from the MADT (the VT-d window is architectural). Gap: the
DMAR's `RMRR` regions and the IORT's reserved memory ranges are not
read — no test machine declares any, and a machine that did (a legacy
USB controller, a graphics aperture) would have its device fault.

**IOM8. IOVA 0 is never a valid bus address, and every IOVA fits every
device's DMA mask.** The window is `[1 MiB, 4 GiB)`: 0 stays the DMA
API's failure value and a 32-bit-only device can address every address
the allocator produces. Check: the `iommu` self-test asserts every
address the DMA API returns is inside the window; `iova_alloc` returns
0 only when full. Gap: a device with a mask below 32 bits would get an
address it cannot reach — no such device exists and nothing checks
`dev->dma_mask` against the window.

**IOM9. A fault never changes memory and never floods the log.** The
unit reports the transaction as refused; the kernel counts it
(`iommu_stats.faults`), logs requester, address and reason at `WARN`
for the first eight per boot, clears the record and continues. There is
no recovery attempt and no device reset. Check: the `iommu` self-test
provokes a fault through `blkdev_ops::debug_dma` and requires the
counter to rise, the log to stay bounded (the SMMU produces 256 events
for one command) and the device to work afterwards. Gap: a device that
faults continuously is not cut off (no per-device fault budget);
`iommu_note_fault`'s bound is per boot, not per device.

**IOM10. A domain outlives every device attached to it, and its tables
outlive every mapping.** `iommu_domain_destroy` asserts `nr_devices ==
0`; `iommu_detach_device` clears the unit's entry and invalidates
before it destroys the domain, and `device_unregister` calls it after
the driver's `remove` has returned, when no DMA can be in flight.
Intermediate page tables are freed only by `iommu_pt_free`, at domain
destruction. Check: `KASSERT` in debug builds; the `iommu` self-test
creates and destroys a domain of its own and requires the allocator to
be back at its reserved count. Gap: no boot unregisters a real
bus-mastering device, so `iommu_detach_device` runs only in shutdown
paths that are not exercised, and nothing forces a detach while a
driver still holds mappings (the warning path is untested).
