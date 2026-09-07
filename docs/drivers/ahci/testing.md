# AHCI driver: testing

## What runs on every boot

With `QEMU_SATA=disk` (the default) the boot test requires the module to
load, the controller to come up (`ahci0: pci:00:1f.2: AHCI 1.0, 6
port(s) implemented of 6, 32 slots, 64-bit DMA, NCQ capable`), and the
test disk on port 1 to be identified and registered (`ahci0: port 1:
QEMU HARDDISK (QM00003) is ahci0p1: 16384 sectors of 512 bytes …`,
`blk: ahci0p1: …`). On `q35` it also requires port 0, because the boot
image is a SATA disk on the ICH9's first port: the driver drives the
medium the machine booted from, as `ahci0p0` (64 MiB), and the IOMMU
test provokes a fault through it too. Then the self-tests:

**`ahci-identify`**: `ahci0p1` has the image's geometry (16 384 × 512),
`max_sectors` ≥ 64, `max_segments` ≥ 8, its `dev` is on the PCI bus and
in an IOMMU domain when there is a unit (A1), and the driver fills
`timeout`, `debug_dma` and `debug_presence`.

**`ahci-io`**: 128 KiB (one command of 256 sectors) written and read
back at the start, a middle and the end of the disk; a flush; an
out-of-range read refused `-EINVAL` by the layer without a command; the
first write's second sector read alone; a two-segment four-page bio (a
PRDT of two entries) submitted asynchronously and read back flat. The
layer's `reads`, `writes` and `flushes` advanced by exactly the
operations issued, `errors` unchanged, DMA maps equal unmaps (10
segments on QEMU; A2).

**`ahci-timeout`** (debug builds; fault injection): the same body as
`usb-storage-timeout`, over `ahci0p1` with `FI_AHCI_CI` — a slot is
filled and its `PxCI` bit never written, so the command never starts.
Five rounds of: `timeout_ns` 200 ms, a read returns `-ETIMEDOUT` (the
layer's thread checks every 500 ms; about 470 ms each), the port is
restarted, reads and writes work; in each round another thread submits
a bio 0.3–4 ms into the recovery and a second one a millisecond later,
and both are served with the right data. About 2.4 s. This is the
second driver whose refusals exercise the pending queue's lost-wakeup
fix.

**`ahci-unplug`**: with a read in flight (withheld `PxCI`), the driver's
own disk-gone path is run for the port (`debug_presence(bd, false)`):
the bio completes `-ENODEV` and not never; `blk_find("ahci0p1")` is NULL;
the registry has one device fewer; a read through the test's held
reference returns `-ENODEV`. Then `debug_presence(bd, true)` on the old
object — which only names the port — probes it and registers a *new*
blkdev under the same name: a different object, the same geometry,
readable; the registry count is back. The test then drops the old
object's last reference. 28 ms.

**`ahci-reset`**: with a read in flight (withheld `PxCI`),
`debug_presence(bd, true)` on the live disk stops the port, fails the
command `-EIO`, issues a COMRESET, re-identifies the disk and — the same
serial, capacity and sector size — keeps the same blkdev: `blk_find`
returns the same object and a read and a write work. The recovery an
error that needs a link reset goes through.

**`iommu`** (unchanged): it walks every blkdev with `debug_dma` and now
provokes faults through `ahci0p0` and `ahci0p1` as well, each attributed
to the controller's requester (`00fa`, `pci:00:1f.2` on `q35`).

**`blk-bench`** (reports; below).

## Shapes

- `QEMU_SATA=0`: no test disk. `q35` keeps the ICH9 with the boot image
  on port 0 (`ahci0p0` exists; the port-1 tests skip); `virt` has no
  controller and the driver binds nothing.
- `QEMU_SATA=cd`: an `ide-cd` on port 1: `ahci0: port 1: an ATAPI
  device (signature 0xeb140101) is not driven`, no `ahci0p1`, the tests
  skip (A6). `q35` also has QEMU's default empty CD-ROM on port 2 in
  every shape, so the ATAPI refusal is exercised on every x86 boot.
- `ARCH=aarch64`: `-device ahci` on `virt`'s PCI behind the SMMU; the
  same disk on port 1 is `ahci0p1` and every test passes the same way;
  the fault through it is attributed to the controller.
- `QEMU_IOMMU=0`, `QEMU_SMP=1`, release, `test-crash`, `analyze`,
  `reproducible`: the usual chain; 27 steps with the three SATA shapes.
- No fuzz target: IDENTIFY is a fixed 512-byte structure read by word
  index; nothing in it is walked.

## Confirmed against bugs

- The report assumed the test disk would be the controller's only one
  and put it on port 0; on `q35` the boot image is already there (a
  plain `-drive` is a SATA disk on the ICH9), and QEMU refused a second
  unit on the port. The disk moved to port 1 on both machines, and the
  driver turned out to drive the boot medium as well — a disk nothing
  planned for and every test on `q35` now runs beside.
- A COMRESET with FIS receive off: the signature FIS the device sends
  after the reset is held back and `PxTFD` keeps `BSY`, so the driver
  waited its full second for nothing (`ahci-reset` took 4 s). The reset
  and recovery paths now stop command processing only (`PxCMD.ST`) and
  leave `FRE` on; `ahci-reset` takes tens of milliseconds.
- The timeout path stopped the port and then failed every slot, and a
  bio submitted from another thread between the two was accepted into a
  free slot and failed with the rest (`-EIO`); on x86_64 the racing
  readers never landed in the window, on aarch64 (`QEMU_EL2=0`) they did
  on the first chain run. The port is `recovering` for the whole restart
  and `submit` refuses `-EAGAIN`; the same shape the USB unit's review
  found in its storage driver, found here by the test that review
  produced.
- (Reintroduction.) With the detach path's `slots_fail` removed, the bio
  in flight at `ahci-unplug`'s detach never completes and the test fails
  at its "completed, with the right error" step; with it, the bio
  completes `-ENODEV` (A4).

## Benchmarks

`blk-bench` (reports only): sequential 64 KiB and 4 KiB bios over every
disk in the boot (writes on the USB and SATA test disks only, since the
others carry filesystems later tests mount), and — new with this unit —
four threads reading 4 KiB bios at once from disjoint regions, the
figure that says what several commands in flight buy. QEMU TCG, 4 CPUs,
Apple Silicon host, noisy, indicative:

| Disk | read 64 KiB | read 4 KiB | 4 threads × 4 KiB, aggregate |
|---|---|---|---|
| `ahci0p1` (AHCI, non-queued, 32 slots) | 550 MiB/s, ~113 µs/req | 58 MiB/s, ~66 µs/req | 16 600 req/s |
| `nvme0n1` | 440 MiB/s, ~141 µs/req | 41 MiB/s, ~94 µs/req | 19 000 req/s |
| `vda` (virtio-blk) | 620 MiB/s, ~101 µs/req | 62 MiB/s, ~62 µs/req | 27 300 req/s |
| `sda` (USB, one exchange in flight) | 270 MiB/s, ~229 µs/req | 37 MiB/s, ~105 µs/req | 9 300 req/s |

What the numbers decide (`docs/audit/next-subsystem-ahci.md`,
"Benchmarks"): with non-queued commands the AHCI disk is already the
fastest per request in the boot for a single stream, and with four
streams it reaches 87 % of NVMe's aggregate — NVMe having a queue per
CPU. The gap NCQ could close is that 13 %, on a device model that
completes a command in tens of microseconds; on TCG the cost of a
request is the device model's and the interrupt's, not the device's
turnaround, which is what NCQ hides. **NCQ is not written**: the tag
machinery (`READ`/`WRITE FPDMA QUEUED`, `PxSACT`, the set-device-bits
FIS, `READ LOG EXT` page 10h to learn which tag failed) would be the
hairiest part of the driver for a benefit this host cannot show, and
§21 says so. IDENTIFY's NCQ facts are logged so a real disk's answer is
visible when one is tried. `max_sectors` stays at 256 for the same
reason.

## Not covered

- **Task-file error recovery** (§6.2.2.1, `port_recover`): QEMU's disk
  does not fail commands, so the path is reviewed, not run; the timeout
  and reset tests run the same port restart from other entries.
- **A physical pull raising `PCS`**: the harness has no monitor; the
  worker's `PCS`/`PRCS` handling is entered by the tests' COMRESET
  (QEMU raises `PRCS` after it) and the detach is driven by
  `debug_presence`. A cable event proper is by hand with QMP.
- **BIOS/OS handoff** (`CAP2.BOH`; QEMU has none), **32-bit-only
  controllers** (`S64A` = 0), **4 Kn sectors**, **disks without LBA48**,
  **port multipliers**, **ATAPI** (refused): written to the
  specification, untested here.
- Real hardware, as for every driver here (§61).
