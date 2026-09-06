# IOMMU: Design

The decision record for the DMA-remapping layer (`architecture.md` for
the shape and scope). Written before the code; the "What was found"
notes at the end record what changed while it was built.

## 1. The problem

`dma_map` returned the physical address and `dma_unmap` did nothing
(`kernel/device/dma.c` before this unit): every device could read and
write all of memory, and the descriptor-table finding of the audit (a
device rewriting `desc[].next` to make the driver write a megabyte past
the ring) was one instance of the general hazard. The audit's §10.3
lists what was missing: a mapping layer behind the naming layer, a
domain per device, a gate at bus-master enable, and interrupt-remapping
readiness. Both QEMU test machines offer an IOMMU (`-device
intel-iommu` on q35, `-machine virt,iommu=smmuv3`), so the layer can be
tested end to end, which is why it is built now rather than only
designed.

## 2. Domains and the device model

```c
struct iommu_domain {
    const struct iommu_unit *unit;     /* the hardware that translates for it */
    unsigned id;                       /* domain id (VT-d) / VMID (SMMU), from the unit's bitmap */
    paddr_t root;                      /* the top-level page table */
    spinlock_t lock;                   /* IRQ-safe: dma_map runs in completion handlers */
    struct iova_space iova;            /* bitmap over [IOVA_LO, IOVA_HI) */
    unsigned nr_devices;
    uint64_t maps, unmaps, pages;
};
struct device { ...; struct iommu_domain *iommu; uint32_t iommu_sid; };
```

- **One domain per device.** `pci_enable_device(pdev, bus_master=true)`
  calls `iommu_attach_device(&pdev->dev, sid)` before it sets the
  bus-master bit; the layer finds the unit covering the requester id
  (VT-d: the DRHD whose scope names it or has `INCLUDE_PCI_ALL`; SMMU:
  every PCI device of the root complex), allocates a domain and attaches
  the id. A device no unit covers keeps `iommu == NULL` and the identity
  path. Sharing a domain between devices is possible in the API
  (`iommu_attach_device` with an existing domain) and unused: isolation
  is the point.
- **Requester id.** `sid = bus << 8 | slot << 3 | func` on both
  architectures (VT-d source id; SMMUv3 stream id under the virt
  machine's identity RID mapping). Bridges are not enumerated behind
  today; a device behind a PCIe switch would use its own id, a device
  behind a PCI bridge the bridge's — recorded, not handled.
- **Detach.** `device_unregister` calls `iommu_detach_device` after the
  driver's `remove` returned (no DMA is in flight): the context entry is
  cleared and invalidated, outstanding mappings are counted as a leak
  and logged (they cannot be freed for the driver), the domain's tables
  and id are released.

## 3. Translation and the IOVA space

- **One page-table format walker, two encodings.** Both units walk a
  4-level, 4 KiB-granule tree of 512-entry tables over an input the unit
  chooses (VT-d AGAW 48; SMMU stage-2 with `T0SZ = 64 - OAS` from
  `IDR5.OAS`, `SL0 = 2`). The
  entries differ: VT-d second-level PTEs carry `R` (bit 0) and `W`
  (bit 1); SMMU stage-2 descriptors are LPAE: `valid` (bit 0), `table`
  or `page` (bit 1), `S2AP` (bits 6–7), `AF` (bit 10), `SH` (bits
  8–9), `MemAttr` (bits 2–5). `kernel/iommu/pt.c` (header `kernel/iommu_pt.h`) implements the walk
  (allocate intermediate tables from the PMM on demand, map a page,
  unmap a page, look a page up, free the tree) and takes a
  `struct iommu_pt_fmt` of encoders from the driver. Intermediate tables
  are never freed while the domain lives (the walker keeps the shape
  simple; the tree is bounded by the IOVA window).
- **The IOVA window** is `[IOVA_LO = 1 MiB, IOVA_HI = 4 GiB)`: below every
  device's DMA mask (the 32-bit default included), above the zero page
  so `dma_map`'s 0 stays "failure". A bitmap of 2^20 − 256 pages
  (128 KiB per domain) allocates it, first fit from the bottom; a
  mapping of `n` pages takes `n` consecutive bits. Exhaustion is
  `dma_map` returning 0 (the caller's `-EINVAL`/`-ENOMEM` path). The
  allocator is under the domain's spinlock and IRQ-safe.
- **Reserved ranges.** A unit may name ranges no domain must hand out
  (`ops->reserved`), each either simply removed from the allocator or
  identity-mapped so the device keeps reaching it: the MSI doorbell is
  the case that matters (x86-64 `0xFEE0_0000` + 1 MiB, which VT-d
  interprets itself and must never be a translation; the virt machine's
  GICv2m frame at `0x0802_0000`, which the SMMU does translate and so
  has to map to itself). They are applied at domain creation and counted
  in `iova.reserved`, so "the domain leaked mappings" stays a comparison
  against that number rather than zero.
- **Permissions.** `DMA_TO_DEVICE` maps read-only; `DMA_FROM_DEVICE` and
  `DMA_BIDIRECTIONAL` read-write (write-only pages are not used: VT-d
  second-level write-only is conditional on a capability and the gain
  is nil). `dma_alloc` maps read-write.
- **Invalidation.** Neither unit caches not-present entries in the
  tested configuration (VT-d `CAP.CM = 0`; SMMU walks on a miss), so a
  new mapping needs no invalidation; an unmapping is followed by a
  domain-selective IOTLB invalidation (VT-d `IOTLB_REG` domain-selective
  after a page-selective range when `CAP.PSI` allows, else domain; SMMU
  `CMD_TLBI_S2_IPA` per page plus `CMD_SYNC`). Table writes are made
  visible with `arch_dma_barrier` before the invalidation command.
- **Faults.** Both units raise an interrupt (VT-d: the fault event as an
  MSI composed by `irq_request_msi`; SMMU: the event queue's wired SPI)
  and the driver decodes the record (VT-d fault recording registers;
  SMMU event queue entries), counts it (`iommu_stats.faults`), and logs
  the requester, address and reason at most eight times per boot. No
  recovery: the device's request fails as its own protocol says.

## 4. The DMA API over domains

```c
dma_alloc:  pages from the PMM (zone per mask as before) → iommu_map(dom, iova, pa, n, RW) → *dma_out = iova
dma_free:   iommu_unmap(dom, iova, n) → pages back
dma_map:    direct-map check as before → iova = iova_alloc(pages(va, len)) → map → return iova + (va & 0xfff)
dma_unmap:  unmap the pages of [iova & ~0xfff, len) → iova_free
```

Everything else is untouched: `dma_mappable` (the direct-map and mask
predicate the block layer uses), the sync barriers, the statistics
(`maps`/`unmaps` count as before, plus `iommu_stats`). A device without
a domain takes exactly the old path. `dma_map` may run in interrupt
context (virtio-net reposts receive buffers from its completion
callback): the domain lock is IRQ-safe and the PMM's zone locks already
are, so page-table pages can be taken there.

## 5. Bring-up order and the units

`iommu_init()` runs in `kernel_main` after `pci_init()` (the requester
ids exist) and before `module_load_boot()` (the first `pci_enable_device`):
it lets each driver probe (`intel_vtd_init` on x86-64, `arm_smmuv3_init`
on AArch64), and a probe registers a `struct iommu_unit { ops, name,
covers(sid), ... }`. A unit enables translation with **no** device
attached: from that moment every DMA by a device without a context entry
faults, which is the isolation guarantee, and every driver attaches
through `pci_enable_device` before it programs a single bus address.
Without a unit (a machine without an IOMMU, or QEMU without the device)
the kernel logs `iommu: none` and runs as before.

**Intel VT-d (`drivers/iommu/intel_vtd.c`).** `DMAR` → each DRHD's
register base (`vm_map_phys`, uncached), `CAP`/`ECAP` read (AGAW 48
required, `ECAP.C` coherence noted, `CAP.ND` domain ids, `CAP.FRO`
fault-recording offset, `ECAP.IRO` IOTLB register offset); a 4 KiB root
table of 256 entries, context tables per bus allocated on the first
attach (256 entries of 16 bytes: present, translation type 0, the
domain's root, `AW = 010` for 48 bits, the domain id); `RTADDR` and
`GCMD.SRTP` (wait `GSTS.RTPS`); the fault event MSI (`FEDATA`,
`FEADDR`, `FEUADDR`, `FECTL.IM` cleared); `GCMD.TE` (wait `GSTS.TES`).
Invalidation is register based (`CCMD` context, `IOTLB_REG`
domain-selective) — QEMU implements it and it needs no queue; the
queued-invalidation interface is the follow-up for hardware that
requires it. Faults: `FSTS.PPF` → walk the fault records (`F` bit,
type, reason, source id, address), clear each with `F`, clear `FSTS`.

**ARM SMMUv3 (`drivers/iommu/arm_smmuv3.c`).** The unit comes from the
ACPI **IORT**, which is a static table and needs no AML: the first
SMMUv3 node (type 4) gives the register base and the event and
global-error interrupts, and the MSI doorbell to identity-map comes from
the MADT's GIC MSI frame. Discovery has to be by table rather than by
probing a fixed address, because an unassigned MMIO read is an external
abort on this architecture, not a bus value of ones (the virt machine's
defaults — `0x09050000`, INTIDs 106–109 — remain only as the fallback
for a GSIV the table leaves zero). `IDR0`/`IDR1`/`IDR5` read (stage 2 supported, stream id size,
queue sizes, output size 48); `CR1` memory attributes (inner-shareable,
write-back); a linear stream table of 256 entries (stream ids below
256: every function of bus 0, which is all the virt machine has; a
larger id fails `iommu_attach_device` with `-ERANGE`, recorded); the
command queue and event queue (256 entries each, `CMDQ_BASE`/
`EVENTQ_BASE` with `LOG2SIZE`, `PROD`/`CONS` with the wrap bit,
`EVENTQ_PROD/CONS` in the second 64 KiB page); `IRQ_CTRL` enabling the
event and global-error interrupts; `CR0` `EVENTQEN|CMDQEN|SMMUEN` each
acknowledged in `CR0ACK`; `GBPA` left at bypass-disabled abort so an
unattached stream faults. An attach writes an STE: `V`, `Config = 0b110`
(stage 2 only), `S2VMID`, `S2VTCR` (`T0SZ 16`, `SL0 2`, `IRGN0/ORGN0`
write-back, `SH0` inner, `TG0` 4 KiB, `PS 48-bit`), `S2AA64`, `S2R`
(record faults), `S2TTB`, then `CMD_CFGI_STE` + `CMD_SYNC`. Unmapping
issues `CMD_TLBI_S2_IPA` per page and one `CMD_SYNC`; the command queue
is polled (`CMDQ_CONS` reaching `CMDQ_PROD`), which is the only wait in
the layer and bounded by a spin count. Faults: the event queue's SPI →
drain events (type, stream id, address), count and log.

## 6. What is not done

- Interrupt remapping (QEMU: `intremap=off` on the command line, so the
  DMAR's `INTR_REMAP` flag is clear and nothing is asked for).
- AMD-Vi, huge pages, an IOVA cache, PASID/SVA, ATS.
- Freeing intermediate page tables while a domain lives.
- Stream ids above 255 on the SMMU (a 2-level stream table).
- Devices behind PCI-to-PCI bridges (requester-id aliasing).

## 7. Tests

`iommu` (kernel self-test, both machines, skipped with a note when no
unit exists): a domain of its own, `iommu_map` of pages at chosen IOVAs
and `iommu_lookup` back, an overlapping map refused, `iommu_unmap` then
lookup fails, the IOVA allocator hands out non-overlapping ranges,
reuses freed ones and fails when the window is full (a test window of
a few pages), `dma_map`/`dma_unmap` through a test device attached to
the domain return IOVAs inside the window and count, the reserved
ranges are out of the allocator and translate to themselves, and the
fault counter is unchanged by all of this. The boot markers require
`iommu: <unit> ... translation on` on both machines, and every existing
device test (virtio-blk, virtio-rng, virtio-net, NVMe, the network
harness, cosmofs on NVMe) now runs through translation — the strongest
evidence the mappings are right. Fault provocation: the block layer
gains an optional `ops->debug_dma(bd, addr)` (tests only) and the NVMe
driver implements it as an Identify into `addr`; the test picks the
last page of the controller's own IOVA window, checks that nothing maps
it, issues the command, and requires the unit's fault counter to rise
and a following `blk_read` to succeed. The command's own status is not
asserted: QEMU's controller reports success for an Identify whose data
the IOMMU dropped.

## 8. What was found while building it

- **The SMMU's input size is not free.** `T0SZ 16` (a 48-bit input,
  what the design assumed) is `C_BAD_STE` on the virt machine, whose
  `IDR5.OAS` is 44 bits: stage-2 requires the input to fit the output
  size. `T0SZ` and `S2PS` are now derived from `IDR5.OAS`
  (`64 - oas`, so 20 here) and the walk still starts at level 1
  (`SL0 = 2`), which covers the 4 GiB window with room to spare. The
  failure was silent except for an aborted transaction; `QEMU_EXTRA="-d
  guest_errors -trace smmuv3_*"` named it in one line.
- **MSI doorbells are device writes.** The first attach on AArch64 killed
  every interrupt: the GICv2m frame the devices write to is a DMA write
  like any other and the SMMU translated it into a fault. Identity
  mapping it (the `reserved` mechanism above) fixes it. VT-d does not
  translate `0xFEE0_0000` — it interprets it — so there the range is
  only kept out of the allocator, but keeping the two cases in one
  mechanism made both obvious.
- **The IOVA allocator's shape shows up in the page tables.** A rolling
  cursor (the design's first sketch) walks the window and makes the
  domain allocate a new leaf table every few hundred mappings for the
  life of the boot; first fit from the bottom reuses the same tables and
  the tree stops growing once the working set has been seen. It also
  made the `vmm` self-test's page accounting reproducible: that test
  took its baseline before an arena allocation whose page-table page the
  IOMMU's tree had made non-deterministic, and now warms the arena
  first.
- **Every device is attached, and nothing faults in normal operation.**
  x86-64 puts 5 devices in domains 1–5 and AArch64 6, and a boot with
  translation on and every self-test passing reports 0 faults — the
  mappings the drivers ask for are the ones they use. The provoked
  fault is the only one in a run: one record on VT-d (QEMU compresses
  the rest), 256 events on the SMMU, since the controller retries the
  4 KiB Identify write in 16-byte pieces and each is refused; the log
  is bounded to eight lines per boot, which is what that bound is for.
- **An absent unit must be recognised without touching it.** The first
  version probed the virt machine's fixed SMMU address and checked for
  all-ones, the x86 habit; on AArch64 that read is an external abort and
  a machine started without `iommu=smmuv3` panicked in `iommu_init`.
  The IORT (a static table, no AML) both names the unit and answers
  whether there is one. `QEMU_IOMMU=0` is now a step of the verification
  chain on both architectures, which is what caught it.
- **A device need not notice.** QEMU's NVMe controller completes an
  Identify whose payload the IOMMU dropped with status 0. The isolation
  guarantee is about memory, not about error reporting: the test
  asserts the fault and the survival of the device, not the status.
