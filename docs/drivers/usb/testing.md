# USB host stack: testing

## What runs on every boot

With `QEMU_USB=qemu` (the default) the boot test requires the `xhci` and
`usb_storage` modules to load, the controller to come up (`xhci0:
pci:… xHCI 1.00, 8 ports, 64 slots …`), the mass-storage device to
enumerate (`usb: usb0-1: 46f4:0001 at super speed, 1 interface(s),
class 08/06/50`) and the disk to register (`usb-storage: usb0-1 is sda`,
`blk: sda: 16384 sectors of 512 bytes`). Then the self-tests:

**`usb-enum`**: exactly one device on the `usb` bus; its parent is a PCI
function (behind a controller, not a bus root); `dev.iommu` is NULL (the
device does no DMA of its own, U1); it is the device its controller's
`port_dev[]` names for its port, with a slot; the device descriptor is
18 bytes of type 1 with `bMaxPacketSize0` right for the speed (9, an
exponent, at super speed); the configuration descriptor's
`wTotalLength` equals what was read and its `bNumInterfaces` equals
what was parsed; the one interface is `08/06/50` with exactly two
endpoints, both bulk, one in and one out, of the speed's packet size
(1024 at super speed).

**`usb-storage`**: `sda`'s geometry (16384 × 512, `max_sectors` ≥ 64,
`max_segments` ≥ 4) and that its `dev` is on the PCI bus; a 64 KiB
pattern written and read back at the start, a 64 KiB-aligned middle and
the last 64 KiB; a flush; an out-of-range read refused `-EINVAL` by the
layer without an exchange; the first write's second sector read alone;
a two-segment, four-page bio submitted asynchronously (one TD of chained
TRBs) and read back flat. Then the layer's `reads`, `writes` and
`flushes` advanced by exactly the operations issued, `errors`
unchanged, and the DMA map count advanced by exactly the unmap count
(30 segments on QEMU: every segment mapped for a transfer was unmapped,
U2).

**`usb-storage-timeout`** (debug builds; fault injection): the next
exchange's CSW read is put on the ring but the controller is never told
(`FI_USB_CSW`, decided at submit in thread context; the request sets
`debug_no_doorbell`), so a real transfer is in flight and never
completes — what a device that stops answering looks like to the driver.
`sda`'s `timeout_ns` is 200 ms for the duration; a read returns
`-ETIMEDOUT` (within about 700 ms: the layer's thread checks every
500 ms), the layer's `timeouts` counter is one higher, and after the
driver's reset recovery the same read and a write succeed. Five rounds;
in each, another thread submits one bio a chosen delay (0.3 to 4 ms)
after the layer reports the timeout — into the cancel and recovery —
and a second bio a millisecond after that. Both must complete with the
right data: the block layer queues a bio behind a pending one without
asking the driver, so only a bio that arrives when nothing is queued
reaches the driver mid-recovery, and the second bio is what a driver
that had freed its slot under the first would corrupt. About 2.6 s on
x86_64. Confirmed against the bug: with the slot freed by the cancelled
transfer's callback (the version Greptile reviewed), round 2 (800 µs)
leaves the second bio never completing.

**`usb-unplug`**: a read whose CSW is withheld the same way is in flight
when the controller driver's own disconnect path is run for the port
(`debug_port(hcd, port, false)`: the same function the port worker
calls on a `CCS` = 0 event; the hardware stays attached). Then:
`blk_find("sda")` is NULL; the `usb` bus has no device; a read through
the test's own reference to the old blkdev returns `-ENODEV`; the bio
that was in flight completed, with `-ENODEV` and not never; the
`usb_device` release ran exactly once (`hcd->released`). Then the
connect path (`debug_port(…, true)`: a port reset, as a re-plugged
device gets) enumerates the device again — `hcd->enumerated` up by one,
one device on the bus, `sda` back and readable, the same name on the
same port. 641 ms on x86_64, most of it the port reset and the storage
driver's readiness probe.

**`iommu`** (the existing test, extended): for every block device whose
driver has `debug_dma` and whose DMA device has a domain — `nvme0n1`
and `sda` — a one-block read into an unmapped address is provoked and
*that requester's* fault count (`iommu_stats.by_requester`) must rise;
for `sda` the requester is the controller's (`pci:00:08.0`, requester
`0040` on q35), which is the DMA rule made observable. Attribution is
by counting, not by ordering, so the SMMU's 256-event bursts for one
write cannot be mistaken for the next device's.

**`blk-bench`** (reports; see below).

## Shapes

- `QEMU_USB=0 gmake test`: no controller; the modules load, `xhci`
  binds nothing, every USB test skips with a message, and the boot test
  does not require the device lines. Both architectures, as chain steps.
- `QEMU_USB=nec gmake test`: the `nec-usb-xhci` model (NEC uPD720200)
  instead of `qemu-xhci`; two device models catch what one does not.
- `ARCH=aarch64`: xHCI on `virt`'s PCI behind the SMMU; the same device
  enumerates the same way; `usb-enum`, `usb-storage`, `usb-unplug` and
  the fault attribution pass with the same figures.
- `QEMU_IOMMU=0`: no domains; `iommu` skips the provoked faults; the
  disk works through the identity path.
- `QEMU_SMP=1`, release, `test-crash`, `analyze`, `reproducible`: the
  usual chain.
- `gmake fuzz`: `fuzz_usb_desc` runs the configuration parser
  (`drivers/usb/usb_desc.c`, the same object the module links) on the
  host under ASan/UBSan against the input as a configuration descriptor:
  never a read past the buffer, never more than the limits recorded,
  every accepted descriptor inside the input and at least its header
  long. Seeds: QEMU's device with and without SuperSpeed companions, a
  `bLength` past the end, a `wTotalLength` longer than what was read, a
  zero `bLength`, more interfaces than the limit.

## Confirmed against bugs

Found while building, each by a test that then guards it:

- A control transfer's data stage that moves every byte generates no
  Transfer Event (only a short packet or the IOC on the status stage
  does), so summing bytes from events reported 0 for a full 8-byte
  descriptor read. The driver now takes the TD's requested total unless
  an event cut it (`usb-enum` would fail at the first descriptor).
- A short read of a descriptor returned the byte count, a positive
  number, as success from enumeration; `usb: usb0-1: 0000:0000` was
  logged as a device. Every short read is `-EIO` now.
- At super speed `bMaxPacketSize0` is an exponent (9 → 512), not a size.
- The fault injector answers only in thread context, and the CSW was
  first requested from the data phase's completion (interrupt context),
  so the injected hang never happened and `usb-storage-timeout` saw a
  normal completion. The decision moved to `usbs_submit`.
- The lockdep class table (160) was already at ~155; the module's five
  lock classes overflowed it in the last user-mode test of the boot.
  Raised to 256.
- The module needed seven scheduler symbols no module had used before
  (`wait_event` from a module thread) and `arch_percpu_get`
  (`preemptible()`): the loader stops at the first unresolved symbol,
  which `llvm-nm -u` against the export list shows before a boot does.
- The benchmark's writes over `nvme0n1` destroyed the cosmofs the nvme
  test leaves there for the shell's snapshot test. Writes are on `sda`
  only.
- (Review, PR #51.) The timeout path freed the exchange slot the moment
  the cancelled transfer's callback ran, before the device was reset; a
  bio submitted from another CPU in that window started an exchange on
  endpoints being reset, and the path's tail then forgot it. The slot
  now stays taken (`recovering`) until the device is back; the racing
  readers above found the first fix's version of this too.
- `Reset Endpoint` on an endpoint that is not halted answers Context
  State, and `Set TR Dequeue Pointer` then refuses because the endpoint
  is running. A recovery run on a healthy endpoint (the racing readers
  provoke one) now stops the endpoint first.
- The racing readers then found a bug older than this unit, in the
  block layer: `drain_pending` pops a queued bio, the driver refuses it,
  and between the refusal and the push back to the head the queue is
  empty; a completion that drains in that window finds nothing, and when
  it was the last bio the driver held, no further completion comes and
  the pushed-back bio waits forever. NVMe and virtio-blk rarely refuse,
  so it never showed; a driver that refuses every bio while one exchange
  is in flight showed it about one run in four (`QEMU_IOMMU=0` first).
  The layer now retries at once when nothing is left in flight after the
  push (`blkdev.redrained`); `blk-queue` reproduces the window
  deterministically with two RAM-disk knobs and fails against the old
  drain. A driver that refuses often is a stress test of the layer above
  it.

## Benchmarks

`blk-bench` (reports only): sequential reads over every disk in the
boot and writes over `sda`, 64 KiB and 4 KiB bios one at a time through
`blk_read`/`blk_write`. QEMU TCG, 4 CPUs, Apple Silicon host, noisy,
indicative; the point is the USB disk *beside* the others in the same
boot:

| Disk | read 64 KiB | write 64 KiB | read 4 KiB | write 4 KiB |
|---|---|---|---|---|
| `sda` (xHCI, BOT) | 430 MiB/s, ~144 µs/req | 500 MiB/s, ~124 µs/req | 39 MiB/s, ~98 µs/req | 39 MiB/s, ~98 µs/req |
| `nvme0n1` | 440 MiB/s, ~142 µs/req | — | 29 MiB/s, ~131 µs/req | — |
| `vda` (virtio-blk) | 450 MiB/s, ~139 µs/req | — | 36 MiB/s, ~108 µs/req | — |

Three exchanges of bulk transfers per command and the USB disk is
within noise of the two DMA-queue devices: the cost of a request here
is the device model's, not the transport's. What the numbers decide
(`docs/audit/next-subsystem-usb.md`, "Benchmarks"): `max_sectors` stays
at 128 (64 KiB) — a bigger command cannot move the per-request cost
that dominates; the chained-TRB multi-segment path stays (it is the
correct shape and costs nothing measurable); no further scatter-gather
work is warranted; and one command in flight, which bulk-only transport
requires, is not what limits this path on this device model.

## Not covered

- The controller's own reaction to a physical detach: the harness runs
  `-monitor none`, so `usb-unplug` drives the driver's detach function
  and covers the kernel's removal path; the connect event from the
  controller is covered by boot (a device present when the controller
  starts is reported as the same Port Status Change Event a hotplug
  raises). A manual `QEMU_EXTRA="-qmp unix:/tmp/q,server,nowait"` and
  `device_del` covers the rest by hand.
- A controller that stops completing commands (`hcd->dead`): no
  reproducer in QEMU.
- External hubs, interrupt and isochronous endpoints, full- and
  low-speed devices (QEMU attaches the disk to a USB3 port; the EP0
  size update path for full speed is written to the specification and
  untested), 32-bit-DMA controllers (`AC64` = 0; the mask is set, the
  allocator honours it, no model to try it on), scratchpad buffers
  (both QEMU models ask for none: the code path is written and untested).
- Real hardware, as for every driver here (§61).
