# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the third such report
(`next-subsystem.md` named the Intel NIC, `next-subsystem-usb.md` the
USB host stack; both record their outcomes). Nothing in this one is
implemented.

**Subsystem: an AHCI driver — SATA disks through the ICH9-class host bus
adapter that every x86 machine of the last fifteen years has, and that
the q35 machine the harness boots has carried, undriven, on every boot
so far.**

## Problem

§60 lists AHCI fourth, after NVMe, the Intel NIC and USB, all done. The
practical reason is the same as for the NIC: real machines. A laptop or
desktop older than the NVMe era boots from SATA and has nothing else;
many newer ones still have a SATA disk beside the NVMe one; and an
installation from a USB stick onto such a machine's disk — the thing
§60's ordering builds toward — needs this driver.

The harness already has the controller. QEMU's `q35` machine carries an
ICH9 AHCI function at `pci:00:1f.2` (`8086:2922`, class `01/06`), and
every boot log since the PCI core was written lists it, bound to
nothing. It has no disk behind it because nothing could have read one.

The architectural question this unit asks is narrower than USB's, and
it is worth stating so the unit is not oversold. The block layer has
three hardware drivers (virtio-blk, NVMe, USB storage) and one model of
each kind of device behind it: a virtqueue, a set of submission and
completion queues, a serial exchange. AHCI is a fourth model — a
per-port command list of 32 slots with a scatter-gather table per slot
and completion by bit — and the first whose *device* is a legacy
protocol (ATA) under a modern transport (SATA FIS). The USB report left
one question for this unit by name: whether the DMA-through-the-
controller rule, which lives in USB's header, needs to move into
`struct device` as a DMA-parent pointer once a second bus with devices
behind a requester arrives. AHCI ports are that second case.

## Current implementation

- **Block layer** (`kernel/include/kernel/blk.h`, `kernel/block/blk.c`):
  `struct blkdev` with `blkdev_ops { submit, release, timeout,
  debug_dma }`, multi-segment bios, a pending queue for `-EAGAIN` with
  the lost-wakeup fix from the USB unit (`redrained`), a request timeout
  thread, `blk_register` with a letter prefix. Drivers: `virtio_blk`,
  `nvme`, `usb_storage`, `ramblk`.
- **NVMe** (`drivers/nvme/nvme.c`, 905 lines): the closest precedent —
  a PCI function with rings in DMA memory, MSI-X, PRP lists built from
  bio segments, a per-command timeout and abort/reset path, namespaces
  registered as blkdevs whose `dev` is the controller.
- **The device model, PCI, DMA, interrupts**: as the previous two units
  used them; nothing new is needed to reach a PCI function's BARs, MSI
  and DMA.
- **The USB unit's rule**: a device behind a requester DMAs through the
  requester (`usb_dma_dev`), stated in `docs/kernel/device/design.md`
  with the note that a second such bus decides whether the rule moves
  into the model.
- **The harness** (`scripts/qemu-run.sh`): `q35` has the ICH9 AHCI
  built in (its ports are `ide.0` … `ide.5`); `virt` has no SATA but
  accepts `-device ahci`. QEMU 11.1 offers `ide-hd` (a SATA disk) and
  `ide-cd` (ATAPI) on either.
- `drivers/storage/` holds a README and no code.

## Why it matters

Beyond the machines it makes bootable, three things this unit can find
that nothing else has:

1. **The DMA-parent question, answered rather than deferred.** A SATA
   port has no identity of its own — no configuration space, no
   requester id, no descriptors; it is a number in a register. The
   design below models a port's disk as a blkdev whose `dev` is the
   controller, exactly as NVMe does its namespaces, and *not* as a
   `struct device`. If that holds, the rule stays where it is (in USB's
   header, for the one bus whose devices are devices), and the model
   gains nothing it does not need. If building it shows a port needs to
   be a device after all — for hotplug, say — that is the finding.
2. **A second driver that refuses.** NCQ gives a port 32 slots; when
   they are full, `submit` answers `-EAGAIN`. The block layer's pending
   queue was found wrong by the first driver that refused often (USB);
   a second one, with 32 slots rather than one and completions from a
   controller rather than a state machine, is the check on that fix.
3. **Whether NCQ pays.** §21: the tag machinery (`PxSACT`, FPDMA
   commands, the set-device-bits FIS) exists to keep several commands in
   flight per port. The benchmark below decides whether that shows on
   this device model before the machinery is kept.

## Proposed design

One module, `ahci` (`drivers/storage/ahci.c`, `ahci.h`,
`MODULE_CAP_DRIVER`), bound by PCI class `01/06` with programming
interface `01` (AHCI 1.0); written to AHCI 1.3.1 and ATA8-ACS, not to
QEMU.

**Bring-up** (§10.1). Map BAR5 (ABAR). If `CAP2.BOH`, request the
BIOS/OS handoff (`BOHC.OOS`) and wait for `BOS` to clear. `GHC.AE`,
then an HBA reset (`GHC.HR`, wait for it to clear, 1 s bound), `GHC.AE`
again. Read `CAP`: `NCS` (command slots per port), `S64A` (64-bit
addressing → `dma_set_mask(64)`, else 32), `SNCQ`; `PI` for the ports
implemented. One MSI vector (the ICH9 has single-message MSI; MSI-X if
a controller offers it; no INTx), `GHC.IE`.

**Per port** (§10.1.2, §10.3): stop it (`PxCMD.ST` clear, wait `CR`;
`FRE` clear, wait `FR`), allocate the command list (32 headers of 32
bytes, 1 KiB aligned), the received-FIS area (256 bytes) and 32 command
tables (a 64-byte command FIS, 16 bytes of ATAPI, and a PRDT of up to
`AHCI_PRDT_MAX` entries of 16 bytes each — 56 entries fits a table in
1 KiB) from `dma_alloc`; program `PxCLB`/`PxFB`, clear `PxSERR`, set
`PxIE` (D2H register, set-device-bits, PIO setup, task-file error, PhyRdy
change, port connect change), start `FRE` then `ST`. A device is
present when `PxSSTS.DET` = 3 and `PxSSTS.IPM` = 1; `PxSIG` says what:
`0x00000101` is a SATA disk, `0xEB140101` an ATAPI device (refused with
a log: no packet commands in this unit), `0x96690101` a port multiplier
(refused). Wait for `PxTFD.STS.BSY|DRQ` to clear before the first
command.

**Identify**: `IDENTIFY DEVICE` (`0xEC`) as a host-to-device register
FIS in slot 0, 512 bytes by PRDT. From the result: model and serial
(byte-swapped words 27–46 and 10–19, for the log), the LBA48 sector
count (words 100–103; refused if word 83 bit 10 says no LBA48 and the
LBA28 count is used instead), the logical sector size (words 106 and
117–118; 512 unless the device says otherwise, 4096 accepted), NCQ
support (word 76 bit 8) and queue depth (word 75, plus one), write
cache (word 85 bit 5) so `FLUSH CACHE EXT` is issued only when there is
a cache to flush. Then a `struct blkdev` registered under an exact,
controller-scoped name — `ahci<controller>p<port>` (`ahci0p0`), the
shape `nvme0n1` has — with `blk_register_named`, not under a letter
prefix: the `sd` letters are handed out in registration order, and
registration order between the USB disk (which the xHCI worker
registers, the module's init merely waiting for it with a bound) and a
SATA disk registered from probe would be a timing fact dressed up as a
name. A name that says which controller and port a disk is on is what
a machine with several disks needs anyway; `dev` is the controller,
`sector_size` and `capacity` from
IDENTIFY, `max_sectors` 256 (128 KiB at 512 bytes; a `DMA EXT`
command's count field goes to 65 536 and the benchmark decides),
`max_segments` `AHCI_PRDT_MAX`, `timeout_ns` 10 s, `nr_queues` 1.

**Data path**, in two steps the migration plan separates:

- *Step A, one command at a time*: `READ DMA EXT` / `WRITE DMA EXT`
  (`0x25`/`0x35`) in slot 0, the PRDT from the bio's segments (each
  entry an even byte count up to 4 MiB; a bio segment is page-shaped, so
  one entry each), `FLUSH CACHE EXT` (`0xEA`) for `BIO_FLUSH`. `submit`
  refuses `-EAGAIN` while the slot is taken; the pending queue holds the
  order. Completion: the interrupt reads `PxIS`, clears it, and a slot
  whose `PxCI` bit has cleared is done; `PxTFD.STS.ERR` with `PxIS.TFES`
  is a failed command.
- *Step B, NCQ, if the benchmark says so*: `READ FPDMA QUEUED` /
  `WRITE FPDMA QUEUED` (`0x60`/`0x61`) with the slot number as the tag,
  `PxSACT` set with `PxCI`, up to `min(NCS, queue depth)` in flight;
  completion when the tag's `PxSACT` bit clears (the set-device-bits
  FIS); a task-file error with NCQ outstanding needs `READ LOG EXT` page
  `0x10` to learn which tag failed, then a port restart with the rest
  reissued. `FLUSH CACHE EXT` is not queued: the driver drains the port
  before it.

**Errors and timeouts**: `PxIS.TFES` fails the command(s) in flight
with `-EIO` after a port restart (`ST` clear, wait `CR`; clear `PxSERR`;
if `PxTFD.STS.BSY|DRQ` stays set, a COMRESET through `PxSCTL.DET` = 1
for 1 ms then 0, wait for `PxSSTS.DET` = 3; `ST` set). The block
layer's `timeout` runs the same restart and completes the victim
`-ETIMEDOUT`, the rest `-EIO`.

**Hotplug**: `PxIS.PCS` (a connect-status change) and `PRCS` (PhyRdy
change) are handled on a worker thread per controller, as the USB port
worker is: the port is re-read after a 100 ms debounce; a disk that was
present and is not (`DET` ≠ 3) is `blk_unregister`ed with its commands
failed `-ENODEV`; a port that has a disk and no blkdev is identified
and registered. The harness has no monitor, so the absent-device branch
is driven the way the USB unit drives its detach: a test-only operation
on the blkdev, `debug_presence(bd, present)`, runs the worker's own
function for the port `bd` sits on as if it had read `DET` = 0 (the disk
is taken down, its commands failed `-ENODEV`, the blkdev unregistered
while the hardware stays attached) or `DET` = 3 (the port identified
again and a *new* blkdev registered — the unregistered one is only the
handle that names the controller and port; it is never re-registered,
its reference count is never touched, and it is freed when its last
holder lets go, exactly as a physically re-plugged disk gets a new
object).
The COMRESET test (below) covers the interrupt and re-identify half
through the controller's own event; a physical pull is by hand with QMP.

**Removal** (`remove`): every port's blkdev unregistered (commands in
flight `-ENODEV`), ports stopped, `GHC.IE` cleared, the vector released
and synchronised, memory freed.

**The DMA rule**: the blkdev's `dev` is the controller, as NVMe's is.
No `struct device` per port. The USB header keeps `usb_dma_dev`; the
model gains no DMA-parent pointer, and `docs/kernel/device/design.md`
records that the second case did not need one.

**Harness**: `q35` — `-drive if=none,id=sata0,format=raw,file=sata.img
-device ide-hd,drive=sata0,bus=ide.0` on the built-in controller;
`virt` — `-device ahci,id=ahci0` first. An 8 MiB image beside the
others (`QEMU_SATADISK`). `QEMU_SATA=0` leaves the disk out (`q35`
keeps its controller, so the driver binds and finds no device — a
shape worth having); `QEMU_SATA=cd` attaches an `ide-cd` instead, so
the refusal of an ATAPI device is exercised.

## Affected files

- New: `drivers/storage/ahci.c`, `drivers/storage/ahci.h`,
  `docs/drivers/ahci/{design,api,invariants,testing}.md`;
  `drivers/storage/README.md` rewritten.
- `build/module.mk` (the module, after `usb_storage` in the archive),
  `scripts/qemu-run.sh` (`QEMU_SATA`, the image), `tests/boot/
  run_boot_test.py` (markers), `kernel/core/selftest.c` and
  `kernel/include/kernel/selftest.h` (the tests), `kernel/device/
  devtest.c` (the tests live beside the NVMe and USB ones; `blk-bench`
  gains `ahci0p0` and a concurrent variant), `kernel/include/kernel/
  faultinject.h` (one kind: a command whose `PxCI` bit is never set),
  `kernel/include/kernel/blk.h` (`debug_presence`).
- `README.md`, `docs/README.md`, `docs/kernel/device/design.md` (the
  DMA-parent answer), `docs/audit/next-subsystem-usb.md` (the pointer).
- The chain gains `x86-nosata-test`, `x86-atapi-test` and
  `a64-nosata-test`.

## New APIs

One test-only operation, and otherwise none kernel-facing; the
hypothesis held twice and is tested a third time. The one:
`blkdev_ops.debug_presence(bd, bool present)`, optional, the same shape
as `debug_dma` — the kernel's tests reach a module's code only through
function pointers the module fills in, and the absent-device path needs
reaching. The module otherwise reaches the kernel through
`blk_register_named`, `bio_complete`, `pci_*`, `dma_*`, the interrupt
and thread APIs, and `faultinject_should_fail` — all exported already.
The two places it
could fail, named now:

- **A port as a device.** If hotplug or the removal order turns out to
  need `device_register`/`unregister` semantics for a port's disk (a
  holder that must be told before the memory goes), a `sata` bus and
  per-port devices follow, and the DMA-parent question reopens. The
  design bets they are not needed: a blkdev's reference counting is the
  holder protocol the block layer already has.
- **Interrupt sharing.** The ICH9's single MSI carries every port; if a
  controller offers only INTx, this driver does not bind it (no INTx
  path exists in the tree), and the report says so rather than adding
  one for a case the harness cannot exercise.

## Migration plan

Additive; the existing suite stays green at every step.

1. Bring-up, port init, IDENTIFY, the blkdev registered read-only:
   `ahci0: pci:00:1f.2: AHCI 1.3, 6 ports, 32 slots, 64-bit` and
   `ahci0: port 0: QEMU HARDDISK, 16384 sectors of 512 bytes` in the
   log; `ahci-identify` and `blk-bench` over `ahci0p0` (reads).
2. Writes, flush, the timeout and restart path, `debug_dma`; `ahci-io`,
   `ahci-timeout`, the IOMMU fault test finds a third disk.
3. The port worker, `PCS`/`PRCS`, the in-guest COMRESET test.
4. The benchmark's concurrent variant; NCQ only if it pays; the `nosata`
   and `atapi` shapes; the docs.

The USB unit's lessons apply from step 1: `llvm-nm -u` against the
export list before the first boot; enumeration and registration happen
on the module's own thread, not in the PCI probe, if anything about them
needs the device model's lock (a blkdev registration does not, so step 1
may well run in probe — the design chooses when it knows).

## Tests

- **`ahci-identify`**: the controller bound; exactly one port with a
  device; its signature is a SATA disk; IDENTIFY gives a model string
  starting `QEMU` and the image's sector count; `ahci0p0` exists with that
  geometry and its `dev` is the controller's PCI function.
- **`ahci-io`**: as `usb-storage`: 128 KiB round trips at the start, a
  middle and the end; a flush; a refused out-of-range read; a
  two-segment four-page bio (a PRDT of two entries); the layer's
  counters advanced by exactly the operations; DMA maps balanced by
  unmaps.
- **`ahci-timeout`** (fault injection): one command's `PxCI` bit is
  never set, so it never starts; the layer's timeout fires; the port is
  restarted; the victim is `-ETIMEDOUT`; reads and writes work after —
  and a bio submitted from another thread into the restart (the USB
  unit's racers, reused) is served, in each of several rounds.
- **`ahci-reset`**: with reads in flight, a COMRESET is issued through
  `PxSCTL` from inside the guest; the in-flight commands complete with
  an error and not never; the port raises `PRCS`, the worker
  re-identifies the disk, `ahci0p0` is readable; the disk's blkdev is
  the same object (the disk never left) unless `DET` was seen at 0, in
  which case it was unregistered and registered anew — the test accepts
  either and asserts which happened. This covers the controller's event
  and the re-identify branch.
- **`ahci-unplug`**: the absent-device branch, as `usb-unplug` does it:
  with a read in flight (its `PxCI` bit withheld by the same fault
  injection `ahci-timeout` uses, so it *is* in flight),
  `debug_presence(bd, false)` runs the worker's function for the port as
  if `DET` had read 0. Then: the in-flight bio completed with `-ENODEV`
  and not never; `blk_find("ahci0p0")` is NULL; a read through the
  test's own reference returns `-ENODEV`. Then `debug_presence(bd,
  true)` — the old object only naming the port — identifies the disk
  again and registers a *new* blkdev under the same name; the test finds
  it by name, reads through it, and checks it is not the old object;
  then it drops its reference to the old one and the old one's release
  runs (a counter the driver keeps, read by the test) — once, and only
  then, because nothing re-registers a live kobject. Together with
  `ahci-reset` this covers both branches of the handler;
  what neither covers is the controller raising `PCS` on a physical
  pull, which is by hand.
- **`iommu`**: unchanged; it walks every blkdev with `debug_dma` and
  now finds `ahci0p0`, attributed to `pci:00:1f.2`.
- **Shapes**: `QEMU_SATA=0` (controller, no disk: the driver logs the
  empty ports and the tests skip), `QEMU_SATA=cd` (an ATAPI device is
  refused with one log line and no blkdev), aarch64 (`-device ahci`,
  behind the SMMU), `QEMU_IOMMU=0`, `QEMU_SMP=1`, release, crash,
  analyze, reproducible.
- No fuzz target: IDENTIFY is a fixed 512-byte structure read by word
  index; nothing in it is walked.

## Benchmarks

`blk-bench` already reports sequential 64 KiB and 4 KiB bios over every
disk in the boot; `ahci0p0` joins the table, which puts AHCI beside NVMe,
virtio-blk and USB on the same device-model host. Two additions, per
§21:

- **A concurrent variant**: four threads each reading 1 MiB in 4 KiB
  bios from disjoint regions of the same disk, reported as aggregate
  requests per second. With one command in flight (step A) the four
  threads serialise in the pending queue; with NCQ they overlap. **NCQ
  is kept only if this figure moves** — on TCG, where the device model
  completes a command in a few microseconds, it may not, and then the
  tag machinery is complexity for a benefit that does not exist on this
  host and is written down as such.
- **`max_sectors`**: 256 to start; 512 and 1024 tried once, the figure
  written beside the choice.

## Risks

- **QEMU's ICH9 is not an ICH9.** The mitigation as before: write to the
  specification and list what only QEMU has exercised (the BIOS/OS
  handoff, port multipliers, 4 Kn sectors, INTx).
- **Names.** `ahci0p0` says where a disk is, not what it is; a disk
  moved to another port changes name. Stable naming by identity (serial,
  label) is a later concern for every disk here, and the report says so
  rather than inventing a scheme. The first draft shared USB's `sd`
  letters and pinned them by module order, which review showed to be a
  timing fact (the USB disk registers from the xHCI worker) dressed up
  as a name.
- **Hotplug's absent-device branch is driven, not observed.** The
  COMRESET raises the same interrupt a cable event does and runs the
  same handler, but the disk never leaves; `ahci-unplug` runs the
  absent-device branch through a test-only operation, as the USB unit
  does. The controller raising `PCS` on a physical pull is exercised by
  hand only.
- **ATAPI, port multipliers, INTx-only controllers** are refused, each
  with one log line; a machine whose only SATA controller is INTx-only
  gets no disk from this unit.
- **NCQ error recovery** (`READ LOG EXT` to find the failed tag) is the
  hairiest part of any AHCI driver and only matters if NCQ is kept. The
  plan puts it last for that reason.
- **Size**: about 1 000 lines for the driver, near NVMe's 905, plus 400
  of tests. Step 1 is the check.

## Outcome (2026-09-08)

Built as one unit on `drivers/ahci` (`docs/drivers/ahci/`). The
hypothesis held: no kernel interface changed shape; the additions are
the test-only `debug_presence` op this report named and one
fault-injection kind. The DMA-parent question is answered as proposed:
a port's disk is a blkdev whose `dev` is the controller, there is no
`struct device` per port, and hotplug did not change that.

What the report did not know: on `q35` a plain `-drive` is a SATA disk
on the ICH9's port 0, so the boot image was on the controller all along
and the driver now drives the medium it booted from (`ahci0p0`); the
test disk moved to port 1 on both machines. Every test in the plan
exists and passes on both architectures; the racing readers found
nothing new this time, and the second refusing driver exercised the
pending queue's fix without incident.

The benchmark answered the NCQ question the way the report allowed for:
four concurrent streams through non-queued commands reach 87 % of
NVMe's aggregate on this host, so the tag machinery is not written
(`docs/drivers/ahci/testing.md`, "Benchmarks").

Size: about 900 lines for the driver, 350 of tests — inside the
estimate.

The next report in this shape is `next-subsystem-console.md`, which
leaves §60's hardware list (complete through AHCI) for the thing every
unit so far has assumed: a console that is not a serial cable.

## Alternatives considered

- **A `sata` bus with per-port devices** — models the DMA-parent
  question the way USB did. Rejected for now: a port has nothing a
  `struct device` would name, and the blkdev's own reference counting
  is the holder protocol the disk needs. Named as the fallback if
  hotplug proves otherwise.
- **Legacy IDE (PIO/PATA)** — §60 says not to implement every
  historical interface; every machine with IDE ports also has AHCI
  mode.
- **An external hub driver or HID for USB** — the USB report named
  them as follow-ups; §60 puts AHCI first, and neither exercises
  anything the tree has not seen.
- **The AArch64 follow-ups** (GICv3, ASID allocation, FP/SIMD at EL0) —
  still worth doing; still not new capability.
