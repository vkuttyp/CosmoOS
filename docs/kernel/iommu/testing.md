# IOMMU: testing

## What runs where

| Level | What | Command |
|---|---|---|
| Target self-test (debug builds, 4 CPUs) | `iommu`: the IOVA allocator, a domain's mappings, the DMA API through an attached device, the reserved ranges, a provoked fault | `make test`, `make ARCH=aarch64 test` |
| Every other device test, through translation | `dma`, `blk`, `pci`, `virtio-console`, `nvme`, the network and cosmofs tests — all of them now run with the devices in domains | `make test` |
| Boot markers | `iommu: <unit> at <base>: ...; translation on` and one `... in domain N` line per attached device | `make test` |
| Release | same drivers, no self-tests: the mappings alone carry the boot | `make BUILD=release test` |
| Single CPU | `QEMU_SMP=1`: the same mappings with completions on one CPU | `QEMU_SMP=1 make test` |
| Without an IOMMU | `QEMU_IOMMU=0`: no unit, the identity path, `iommu` self-test skips with a note | `QEMU_IOMMU=0 make test` |

There is no host test: the layer is register and page-table work whose
only meaningful oracle is the hardware. The page-table walker is
covered through the two drivers rather than separately.

## The QEMU configuration the tests assume

`scripts/qemu-run.sh` adds, unless `QEMU_IOMMU=0`:

- x86-64: `-device intel-iommu,intremap=off` on the q35 machine. QEMU
  builds a `DMAR` table with one DRHD (`INCLUDE_PCI_ALL`) at
  `0xfed90000`, `CAP.ND` for 4096 domain ids, one fault-recording
  register, `CAP.CM = 0`, no coherence bit. Interrupt remapping is off
  on purpose: the MSI path is unchanged by this unit.
- AArch64: `-machine virt,iommu=smmuv3,gic-version=2`. QEMU then puts an
  SMMUv3 node in the ACPI IORT, which is where the driver finds the unit
  (at `0x0905_0000`, event queue on INTID 106, global error on 109);
  without the option there is no such node and the driver returns before
  touching anything — on this architecture a read of an unassigned MMIO
  address is an external abort, so probing a fixed address is not an
  option. `IDR5.OAS` is 44 bits, which is what the stage-2 configuration is
  derived from, and stream ids are 16 bits (a linear stream table of
  256 entries covers bus 0, which is all the machine has).

Both machines therefore boot with **every** PCI device — virtio-blk,
virtio-net, virtio-rng, virtio-console, NVMe — translating. That is the
strongest test in the unit: a wrong mapping, a missed invalidation or a
lost page offset shows up as a device that no longer works, and every
existing device, filesystem and network test is now also an IOMMU test.

## The `iommu` self-test (`kernel/iommu/iommutest.c`)

Skipped with `selftest: iommu: no unit (QEMU without an IOMMU);
skipping` when nothing registered. Otherwise, in order:

1. **The IOVA allocator** over a window of 8 pages: three allocations
   that do not overlap and rise, a fourth refused because the window is
   full, the middle one freed and handed out again at the same address
   (first fit from the bottom), the window emptied and taken whole.
2. **A domain of its own**: `iommu_map` of three pages then a fourth,
   `iommu_lookup` back including a page offset inside the range and a
   byte past the end, an overlapping map refused with `-EEXIST`, an
   unaligned map refused with `-EINVAL`, `iommu_unmap` of all four and
   the lookups failing, `maps`/`unmaps`/`pages_mapped` as expected.
3. **The DMA API** through a `struct device` attached to that domain:
   `dma_map` of an unaligned buffer returns an address in the window
   with the page offset preserved and translating to the right physical
   pages at both ends, a second mapping does not collide, `dma_unmap`
   removes it, `dma_alloc`/`dma_free` map and unmap a two-page ring,
   and the allocator ends at exactly its reserved count.
4. **The unit's reserved ranges**: each range inside the window is
   either unmapped or, when the unit declared it identity, translates
   to itself (the GICv2m doorbell on AArch64).
5. **Nothing faulted**: the counter and the domain count are what they
   were before the test — the boot so far, with every driver mapping
   through its domain, produced no fault at all.
6. **A provoked fault**: `blk_find("nvme0n1")` and, when the driver has
   the `debug_dma` hook and the device has a domain, an Identify
   Controller aimed at the last page of the controller's own IOVA
   window (nothing maps it; the allocator hands out the lowest
   addresses). The unit's fault counter must rise within 500 ms (the
   fault interrupt is asynchronous), and a following `blk_read` of
   sector 0 must succeed: the device survives its own refused DMA. The
   command's status is deliberately not asserted — QEMU's controller
   completes an Identify whose payload the IOMMU dropped with status 0.

A passing run prints, e.g.

```text
[ WARN] iommu: intel-vtd0: fault: requester 00:03.0 writing 0x00000000fffff000 (reason 0x5)
[ INFO] selftest: iommu: intel-vtd0, 5 domains, 11 maps, 5 unmaps, 1 faults (1 provoked)
```

(the map and unmap counts are whatever the drivers have done by then and
vary between runs; the domain count and the faults do not)

On AArch64 the same command produces 256 events — the controller
retries the 4 KiB write in 16-byte pieces and the SMMU refuses each —
of which eight are logged; that bound is exactly what `iommu_note_fault`
exists for. The burst also fills the event queue faster than the handler
drains it, so the unit raises a global error (`iommu: arm-smmuv3: global
error 0x4`, an event-queue overflow) that the handler acknowledges: one
expected `WARN` line in a passing run.

## Boot markers (`tests/boot/run_boot_test.py`)

```text
^\[ INFO\] iommu: (intel-vtd0|arm-smmuv3) at .*; translation on$
^\[ INFO\] iommu: (intel-vtd0|arm-smmuv3): pci:..:...\. \(requester ....\) in domain \d+
```

The first fails the boot test if a machine that should have an IOMMU
comes up without translation (a silently missing `-device
intel-iommu`, a driver that gave up in its probe); the second if no
device was attached. Both are `INFO` lines produced by the drivers.

## Debugging notes

- `QEMU_EXTRA="-d guest_errors,unimp -D log"` prints the units'
  complaints, which is how a malformed stream-table entry is diagnosed
  (`SMMUv3 bad STE S2T0SZ = 16`); add `-trace smmuv3_*` or
  `-trace vtd_*` for the transaction-level view (`vtd_iova_to_sspte:
  detected sspte permission error` is a refused translation).
- QEMU compresses repeated VT-d faults (`New fault is not recorded due
  to compression of faults`): a device faulting in a loop produces one
  record, not thousands, so the kernel's counter can lag what the
  device attempted.
- `QEMU_IOMMU=0` is the fastest way to tell a mapping bug from a
  driver bug: if the failure survives with translation off, it is not
  this layer. It is also a step of the verification chain on both
  architectures — the path with no unit has to keep working.
