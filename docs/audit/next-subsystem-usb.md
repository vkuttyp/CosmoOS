# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the second such report
(the first, `next-subsystem.md`, named the Intel NIC and records its
outcome). Nothing in this one is implemented.

**Subsystem: a USB host stack — an xHCI host controller driver, root-hub
enumeration, and USB mass storage (bulk-only transport) as the first
class driver, appearing as a block device.**

## Problem

The system has no USB. §60 lists it third, after NVMe and the Intel NIC,
both of which are done. A machine that cannot read a USB disk cannot be
installed from one, and cannot take a keyboard that is not on a serial
line, which on real hardware means every keyboard.

There is also an architectural reason, of the same kind that made the
NIC unit worth doing. Every bus the device model has ever carried is
flat and static: PCI is enumerated once at boot, and the virtio bus
sits on PCI with one child per function. A device has never appeared
after boot, has never had a child, and has never gone away while a
driver held it. `device_register`, `device_unregister`, the parent
pointer and the kobject lifetime were all built by the audit for
exactly those cases, and none of them has been exercised by a bus that
does them for real. USB does all three: devices arrive and leave at run
time, a hub has children, and a disk can be unplugged with I/O in
flight.

A device model that has only seen static buses is a guess about what
a device model should be — the same sentence the NIC report wrote about
the network interface, and it was right then (the interface held; the
benchmark that the report said did not exist found two bugs).

The NIC report's "Alternatives considered" said USB "exercises no
interface this project claims to have generalised". That was written
with the network interface in view and it undersold the device model;
this report argues the opposite, for the reasons above, and the NIC
report's outcome now says so rather than leaving the two to disagree.

## Current implementation

- **Device model** (`kernel/device/`, `docs/kernel/device/`):
  `struct bus_type { match }`, `struct device { obj, parent, driver,
  res[], dma_mask, iommu, iommu_sid }`, `device_setup`,
  `device_register` (probes registered drivers; sleeps),
  `device_unregister` (unbinds, drops the bus's reference; the creator's
  reference remains), `driver_register`, `device_map_mmio`. Two buses
  exist: `pci_bus` and `virtio_bus` (the virtio module registers its
  own bus and a PCI driver that populates it). `kernel/device/devtest.c`
  exercises the model with a fake bus.
- **PCI** (`drivers/pci/`, `drivers/include/drivers/pci.h`):
  `pci_register_driver` with an id table (vendor/device or class),
  `pci_enable_device`, `pci_map_bar`, `pci_find_capability`,
  `pci_msix_enable/request/release` with per-CPU routing, `pci_msi_enable`.
- **DMA** (`kernel/include/kernel/dma.h`): `dma_alloc/free`,
  `dma_map/unmap`, `dma_sync_*`, `dma_set_mask`, all keyed by the
  `struct device` whose IOMMU domain the address belongs to.
- **Block layer** (`kernel/include/kernel/blk.h`): `struct blkdev` with
  `blkdev_ops { submit, release, timeout, debug_dma }`, multi-segment
  bios (`max_segments`), a request timeout thread, `-EAGAIN`
  requeueing, and `blk_unregister` that refuses new bios and waits for
  submits in progress. Three drivers: `virtio_blk`, `nvme`, `ramblk`.
- **Modules**: `COSMO_MODULE(...)`, `MODULE_CAP_DRIVER`, signing,
  `build/module.mk`; the loader stops at the first unresolved symbol, so
  every kernel symbol a module needs must be exported.
- **Console input** is a serial line only: `kernel/arch/x86_64/serial.c`
  and `kernel/arch/aarch64/pl011.c` call `tty_input` from their receive
  interrupts. Nothing else produces input.
- **The harness** (`scripts/qemu-run.sh`) gives both machines the same
  PCI devices (two virtio-blk, nvme, virtio-rng, virtio-serial, the two
  NICs, an IOMMU) and no USB controller. QEMU 11.1 offers `qemu-xhci`
  and `nec-usb-xhci` as controllers and `usb-storage`, `usb-kbd`,
  `usb-hub` among the devices, on both `q35` and `virt`.
- `drivers/usb/` does not exist; `drivers/README.md` lists `storage/`
  as a later phase and does not mention USB.

## Why it matters

§60 puts USB before AHCI, and the reason survives inspection: AHCI is a
third block driver on a bus the system already handles (a third
implementation of an interface with two), while USB is a new kind of
bus. The audit's device model was designed for removable, hierarchical
devices and has never met one. The unit either confirms that design or
finds where it bends — in the parent-child removal order, in how a
device that cannot DMA on its own (a USB device DMAs through its host
controller) fits a DMA API keyed by `struct device`, and in how the
block layer behaves when the device under it is pulled rather than
reset. Each of those is a finding about an interface this project
claims to have generalised, and none can be had from AHCI.

Mass storage is the class to start with because it makes the whole
stack observable through interfaces that already exist and already
have tests: a new `struct blkdev` named `sda`, readable by `blk_read`,
mountable by cosmofs, visible to the storage self-tests, and
benchmarkable beside `nvme0n1` and `vda` in the same boot. A keyboard
would exercise less (interrupt transfers only, no DMA of consequence,
no block layer) and its only observable effect is a byte in a tty.

## Proposed design

Three pieces, two modules, no kernel change expected.

**1. The USB core** (`drivers/usb/usb.c`, header
`drivers/include/drivers/usb.h`; in the `xhci` module, since nothing
else can host it yet):

- `struct bus_type usb_bus` ("usb"); `struct usb_device { struct device
  dev; struct usb_hcd *hcd; uint8_t address, port, speed; struct
  usb_device_descriptor desc; configuration and interface descriptors;
  endpoints[]; }` named `usb<bus>-<port>` ("usb0-1"). Its `dev.parent`
  is the host controller's PCI device. **All DMA for a USB device goes
  through the controller's `struct device`**, because the controller is
  the PCI requester; the USB device has no IOMMU identity of its own.
  This is the first bus where "the device" and "the device that does
  DMA" differ, and the design says so in one place rather than in
  every driver.
- `struct usb_driver { struct device_driver drv; const struct usb_id
  *ids; probe(struct usb_device *, const struct usb_interface *);
  remove(...) }`, matched by interface class/subclass/protocol or
  vendor/product; `usb_register_driver`.
- Transfers, synchronous with a timeout, the shape every class driver
  needs first: `usb_control_msg(udev, request_type, request, value,
  index, buf, len, timeout_ns)` and `usb_bulk_msg(udev, ep, buf, len,
  &actual, timeout_ns)`; asynchronous submission
  (`usb_submit(struct usb_request *)` with a completion callback) beneath
  them, so the storage driver's bio path does not sleep a thread per
  request. `usb_clear_halt(udev, ep)` for recovery.
- Enumeration, run by the core on a port event from the controller:
  reset the port, `Enable Slot`, `Address Device`, `GET_DESCRIPTOR`
  (device, then the full configuration), `SET_CONFIGURATION`, then
  `device_register`, which probes class drivers per interface.
  Disconnect: `device_unregister` (the class driver's `remove` runs
  first), then the controller's `Disable Slot`, then the creator's
  `device_put`.

**2. The xHCI host controller driver** (`drivers/usb/xhci.c`, `xhci.h`;
module `xhci`, `MODULE_CAP_DRIVER`), matched by PCI class
`0c/03/30` so both QEMU models (`qemu-xhci` 1b36:000d, `nec-usb-xhci`
1033:0194) and real controllers bind. Written to the xHCI 1.2
specification, not to QEMU; the docs say which registers only QEMU has
seen (the lesson recorded from the e1000e RXDCTL bit).

- Bring-up (§4.2): `CAPLENGTH`, `HCSPARAMS1-2`, `HCCPARAMS1` (`AC64`
  decides `dma_set_mask(64)` or 32; `CSZ` decides the context size),
  `USBCMD.HCRST` and wait for `USBSTS.CNR` to clear, `CONFIG.MaxSlots`,
  the device context base address array (`DCBAAP`), the command ring
  (`CRCR`; 256 TRBs with a link TRB and toggle cycle), one event ring
  segment (`ERSTBA`/`ERSTSZ`/`ERDP`; 256 TRBs), scratchpad buffers as
  `HCSPARAMS2` demands, one MSI-X vector (MSI if the controller has no
  MSI-X; no legacy INTx path — §60's "not every historical interface"),
  `USBCMD.RS`. Doorbells from the mapped `DBOFF`.
- Rings: a `struct xhci_ring` (TRBs, enqueue/dequeue, cycle state) used
  for the command ring and every endpoint's transfer ring; a normal
  transfer is a chain of Normal TRBs, one per segment of the buffer,
  so a multi-segment bio maps to one TD without a bounce.
- The interrupt handler drains the event ring: command completions
  wake the waiting command; transfer events complete the
  `usb_request` (on the worker, not in the handler, so a class driver's
  completion may take a mutex); port status changes are queued to a
  worker that runs enumeration or disconnect.
- Contexts: slot and endpoint contexts in a `dma_alloc`'d device
  context per slot, input contexts for `Address Device` and
  `Configure Endpoint`.
- Root hub ports are handled here (`PORTSC`); external hubs are not
  this unit (see Risks).

**3. USB mass storage** (`drivers/usb/usb_storage.c`, module
`usb_storage`): interface class 08, subclass 06 (SCSI transparent),
protocol 50 (bulk-only transport). Per device: `INQUIRY`, `TEST UNIT
READY` (with the `UNIT ATTENTION` retry a fresh device answers first),
`READ CAPACITY (10)` for sector size and count, then a `struct blkdev`
registered under the prefix `sd` (`sda`), `max_sectors` 128 (64 KiB
per command; the benchmark decides whether more pays), `max_segments`
from what one TD can chain, `timeout_ns` 10 s, `read_only` from the
`WRITE PROTECT` bit of `MODE SENSE`. Bios become `READ (10)`/`WRITE
(10)`; flush is `SYNCHRONIZE CACHE (10)`. Bulk-only transport is
serial by nature — one CBW/data/CSW exchange in flight per device — so
`submit` refuses with `-EAGAIN` while one is in flight and the block
layer's pending list keeps order. `timeout` runs the BOT recovery
sequence (`Bulk-Only Mass Storage Reset`, clear both halts) and
completes the bio `-ETIMEDOUT`. `remove` (disconnect) calls
`blk_unregister`, completes any bio still held with `-EIO`, and drops
its reference. `debug_dma` issues a `READ (10)` into the caller's
address, so the IOMMU fault test covers this driver as it covers NVMe.

**Harness.** Both machines gain `-device qemu-xhci -drive
if=none,id=usbdisk,format=raw,file=$usbdisk -device
usb-storage,bus=…,drive=usbdisk` with an 8 MiB image created beside the
others; `QEMU_USB=0` leaves the controller out, and the suite skips
its USB tests when no controller is present. Mirroring `QEMU_NIC`, the
knob is a chain step, not a change to the default.

## Affected files

- New: `drivers/usb/xhci.c`, `drivers/usb/xhci.h`, `drivers/usb/usb.c`,
  `drivers/usb/usb_storage.c`, `drivers/include/drivers/usb.h`,
  `drivers/usb/README.md`, `docs/drivers/usb/{architecture,design,api,
  invariants,testing}.md`.
- `build/module.mk` (two modules and their signing), the boot archive
  list, `drivers/README.md`.
- `scripts/qemu-run.sh` (the controller, the disk, `QEMU_USB`).
- `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` (the
  tests below); `kernel/iommu/iommutest.c` (the fault test walks every
  blkdev with `debug_dma` instead of naming `nvme0n1`);
  `kernel/include/kernel/iommu.h` and `kernel/iommu/iommu.c` (the last
  fault's requester id and address in `iommu_stats`).
- `README.md`, `docs/README.md`, `docs/kernel/device/design.md` (a
  paragraph on the DMA-through-the-controller rule, if the model
  accepts it unchanged).
- The verification chain gains `x86-nousb-test` and `a64-nousb-test`.

## New APIs

Driver-facing, in `drivers/include/drivers/usb.h` and exported from the
`xhci` module: `usb_register_driver`, `usb_unregister_driver`,
`usb_control_msg`, `usb_bulk_msg`, `usb_submit`, `usb_clear_halt`,
`usb_device_put`, and the descriptor types.

Kernel-facing: one small, known addition, and otherwise **none is
planned — that is the hypothesis being tested**, as it was for the NIC.

The known one: `struct iommu_stats` gains `last_fault_sid` and
`last_fault_addr`. Today `iommu_note_fault` bumps the count with a
lock-free atomic and takes no lock at all; it will instead take the
stats lock `g_lock` — the IRQ-safe lock `iommu_get_stats` already
copies under — and write the two fields and the count in that one
critical section, so a reader that sees the count advance sees the
fields that belong to it. One slot, not a ring, because the test that
reads it provokes faults one device at a time (below), every event a
single operation produces comes from the same requester, and nothing
else faults during a boot that passes. Without this the `usb-iommu` check would be
an assertion the test cannot make. Observability, not an interface
change; NVMe's existing check gains the same assertion.

Two places where the hypothesis proper may fail, named now so a change
there is a finding and not a surprise:

- `device_unregister` of a device that has children (a controller
  removed with devices enumerated). The model does not cascade;
  whether it should, or whether the bus must remove its children first,
  is decided when the case is reached.
- The DMA API is keyed by `struct device`; a USB device uses its
  controller's. If a second bus of this kind arrives (AHCI ports are
  one), the rule moves from the USB core into `struct device` as a
  "DMA parent" pointer. Not before.

## Migration plan

Nothing migrates; USB is additive. The order is the one that keeps the
existing suite green at every step and makes each step observable:

1. `xhci` module: bring-up, rings, one interrupt, root-hub port events,
   enumeration through `GET_DESCRIPTOR`, `usb_bus` and
   `device_register`. Observable: the boot log names the device
   (`usb0-1: QEMU USB HARDDRIVE, class 08/06/50`), and the `usb-enum`
   self-test checks it.
2. Control and bulk transfers with timeouts; `usb_storage` read path;
   `sda` registered. Observable: `usb-storage` reads the image and the
   IOMMU fault test finds a fourth blkdev.
3. Writes, flush, the timeout and recovery path (fault injection), and
   disconnect through `blk_unregister`. The in-guest unplug test.
4. `QEMU_USB=0` shape, both architectures in the chain, the benchmark,
   the docs.

The module-symbol lesson applies from step 1: `llvm-nm -u xhci.ko`
against the export list before the first boot.

## Tests

- **`usb-enum`**: a controller was found; exactly one device on the
  root hub; its descriptors are consistent (device descriptor length
  18, one configuration, one interface of class 08/06/50 with one bulk
  IN and one bulk OUT endpoint of the maximum packet size the speed
  allows); the device is named on the `usb` bus with the controller as
  parent. Skips with a message under `QEMU_USB=0`.
- **`usb-storage`**: `blk_find("sda")`; capacity equals the 8 MiB image
  in 512-byte sectors; a pattern written at three offsets (start, a
  64 KiB-aligned middle, the last sector) reads back; a multi-segment
  bio of 16 pages round-trips; `blk_flush` returns 0; the blkdev's
  `reads/writes/flushes` counters advanced by exactly the operations
  issued (the NIC benchmark found double counting; this checks for it
  from the start).
- **`usb-storage-timeout`**: under fault injection the driver drops the
  next CSW; the block layer's timeout fires within `timeout_ns`, the
  bio completes `-ETIMEDOUT`, `timeouts` is 1, recovery runs, and the
  next read succeeds.
- **`usb-unplug`**: the removal path, driven *from inside the guest*
  because the harness runs `-monitor none`. Disabling the port in
  `PORTSC` is not that: xHCI 1.2 §5.4.8 says a software write to `PED`
  leaves the device connected and sets no change bit, so no Port Status
  Change Event arrives and nothing would notice. The test instead calls
  the controller driver's own detach entry (`xhci_debug_detach(port)`,
  debug builds), which runs *exactly the code the port worker runs when
  an event reports `CCS` = 0* — the shared function, not a copy — and
  then asserts: `remove` ran, `blk_find("sda")` is NULL, a bio submitted
  after that returns `-ENODEV`, a bio in flight at the moment of the
  detach completes with an error and not never, and the `usb_device`
  release runs once. Then the same entry replays a connect (`CCS` = 1),
  the device re-enumerates, `sda` is back and readable. What this covers
  is the kernel's removal and re-enumeration path, which is the part the
  device model has never had; what it does not cover is the controller
  generating the event on a physical detach. That half is exercised at
  boot — a device present when the controller starts is reported to the
  driver as a Port Status Change Event, the same event a hotplug raises
  — and by hand with a QMP socket (`QEMU_EXTRA="-qmp unix:…"`,
  `device_del`), which the docs describe and the suite does not depend
  on.
- **`usb-iommu`**: `iommu-fault` today does `blk_find("nvme0n1")` and
  provokes the fault on that device alone, so a `debug_dma` on `sda`
  would be dead code until the test changes. The test is extended to
  walk every registered blkdev whose driver has `debug_dma` and a domain
  (`nvme0n1` and `sda`; `vda` has neither), provoking one fault per
  device and checking that the fault the unit reports carries *the
  controller's* requester id for `sda` — the disk has none — which is
  the DMA-through-the-controller rule made observable. Today the test
  cannot see that: `iommu_get_stats` has only counts, and
  `iommu_note_fault` puts the requester id in the log alone. So
  `struct iommu_stats` gains `last_fault_sid` and `last_fault_addr`,
  recorded with the count under the stats lock — the one kernel change
  this report knows it needs, listed under New APIs. The test is
  serial on purpose: provoke one device, wait until the fault count has
  advanced and then stopped moving (the fault interrupt is asynchronous,
  and one operation is not one event: VT-d reports it once, the SMMU
  256 times because the controller retries — `docs/kernel/iommu/
  testing.md` records both; the existing test already waits for the
  count), read the two fields, then the next device. Every event of the
  burst carries the same requester id, so the last one written is the
  one asked about; a fault from another device cannot land in the slot
  because none has been provoked yet, and the count's not moving is what
  says the burst is over.
- **Shapes**: `QEMU_USB=0` (skips), `QEMU_IOMMU=0`, `QEMU_SMP=1`,
  release, aarch64 (xHCI on `virt`'s PCI with the SMMU in front),
  `test-crash`, `analyze`, `fuzz` (the descriptor parser gets a host
  fuzz target: descriptors come from the device and a malformed one
  must not walk off the buffer), `reproducible`.

## Benchmarks

§21: no complexity without a measured benefit, and the complexity
candidates here are named up front so the numbers can accept or refuse
them. `blk-bench` does not exist as such; the storage self-tests time
nothing. This unit adds **`blk-bench`**: sequential read and write of
4 MiB in 4 KiB and 64 KiB bios, requests per second and MiB/s, over
`sda`, `nvme0n1` and `vda` in the same boot, reported and not compared
(TCG on a shared host, as before).

What it gates:

- **`max_sectors`** for the storage driver: 64 KiB is the starting
  point; the benchmark says whether 128 KiB moves the number.
- **TRB chaining for multi-segment bios**: if a bounced single-segment
  transfer is within noise of a chained one on this device model, the
  chaining stays (it is the correct shape) but no further scatter-gather
  work is done.
- **More than one command in flight** is *not* on the table: bulk-only
  transport forbids it, and the benchmark exists to show what that
  costs beside NVMe, not to remove it.

## Risks

- **Size.** xHCI is the largest single driver this project will have
  written: rings, contexts, the command and event machinery and
  enumeration before a single byte moves. The estimate is 1 500–2 000
  lines for `xhci` plus the core, 500 for storage, against 582 for
  e1000e and 905 for NVMe. The migration plan's step 1 is the size
  check: if bring-up plus enumeration does not fit a unit on its own,
  the unit splits there.
- **QEMU's xHCI is not hardware.** Same mitigation as e1000e: write to
  the specification, list the registers and paths only QEMU has
  exercised, and keep `nec-usb-xhci` as a second model to boot against
  once — two device models catch what one does not, as the second NIC
  did.
- **Hotplug without a monitor.** The harness runs `-monitor none`, and
  the controller offers no software way to fake a detach (`PED` leaves
  the device connected and raises no event), so the suite's `usb-unplug`
  drives the driver's detach function directly and covers the kernel's
  removal path, not the controller's event on a physical pull. The boot-
  time connect event and a manual QMP `device_del` cover that half.
- **External hubs are deferred.** The root hub's ports give the
  parent-child and removal cases; a hub driver (class 09, interrupt
  endpoint for status changes, per-port power and reset through class
  requests) is a follow-up unit of its own. Any machine with a keyboard
  behind a hub needs it before USB input is useful, so it is named here
  rather than forgotten.
- **Boot time.** A USB device in the default boot adds the controller's
  reset and one enumeration, a few tens of milliseconds; the storage
  test's I/O is bounded like `nvme`'s. The budget has tripped this week
  on host load alone, so the numbers are measured in step 1 and the
  `QEMU_USB=0` shape exists from step 1.
- **The device model may need to change** (the two places named under
  New APIs). As with the NIC: a finding, not a failure, and the reason
  this unit is worth more than AHCI.

## Alternatives considered

- **AHCI (§60 #4)** — a third block driver on a static bus. It proves
  the block layer again and the device model not at all. It follows
  this unit.
- **A HID keyboard as the first class driver** — smaller (interrupt
  transfers, a report parser, `tty_input`), and real machines need it,
  but it exercises no DMA of consequence, no block layer, and no
  existing test can see it. It is the natural second class driver once
  the core exists (a few hundred lines), and it needs the hub driver
  on most machines.
- **EHCI/UHCI/OHCI** — historical; xHCI drives every speed and every
  machine since 2012. §60 says not to implement every historical
  interface, and this is what it means.
- **USB as a kernel-image driver rather than modules** — the controller
  could be built in, but every other PCI driver is a signed module and
  the loader's rules (exports, capabilities) are part of what a new
  driver tests.
