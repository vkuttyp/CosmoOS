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

## The keyboard

**`hid-arm` and `hid-keyboard`** (both architectures, whenever the
harness attached a keyboard). The harness opens QEMU's monitor protocol
socket and sends key events into the emulated device
(`tests/boot/keytest.py`, `input-send-event`), which is as close to a
person at a keyboard as a test gets: the events go through the device
model, the device reports them on its interrupt endpoint, the driver
translates and calls `tty_input`, and the test reads the lines back out
of the tty the shell reads. `cosmo Types 42!` exercises letters,
capitals through shift, a digit pair and a shifted symbol; the checks
are the exact lines, the bytes taken in while the test waited
(`tty_stats.rx_bytes`), and nothing dropped.

It is **two** tests because how long a host takes to type into an
emulated machine on a loaded build runner is not this machine's
business, and every self-test is held to a budget of 8 s. `hid-arm`
records the tty's counters, prints the ready marker and returns in no
time at all; `hid-keyboard` runs last, by which point the lines arrived
long ago, so it also takes no time. The first version was one test that
waited, and it passed everywhere except CI.

The check has **one** deadline for both lines, not one per line: the gap
between them is the runner's to decide, and AArch64 CI delivered the
second 5.0 s after the first against a five-second bound -- a failure by
a hair, for no reason of the test's own. It is 25 s for the pair now,
which costs nothing when they have already arrived and is what a real
failure costs to report.

Every key transition is its own command with a gap after it, rather than
a press and a release in one batch. A batch is one input sync, and a
guest that does not poll between the two ends of it can see a key go
down and come up without ever observing it held: on AArch64 CI the
newline that ends the second line vanished exactly that way, while the
same batching survived everywhere else in the same boot. The harness
also waits a moment after the last key before closing the socket.

The overlapping keys are spaced by a tenth of a second, not by the
20 ms the rest of the typing uses. A key state has to last long enough
for the guest to poll it, and a build runner emulating a machine
emulating a keyboard is slower than the 8 ms interval by a wide margin:
at 20 ms, CI read back an empty line where it wanted `xy` -- the two
presses had landed in one polled state. The guest's waits are bounded by
the clock rather than by a count of `thread_sleep_ms(1)`, for the same
reason: on that runner the first version waited 20 s where it meant 5.

`hid-arm` runs after the hotplug tests, not before them: unplugging the
hub takes the keyboard with it, and keys typed while it is gone are gone
too -- the first version armed first and read back `mo Types 42!`.

`hid-arm` also turns the tty's echo off and `hid-keyboard` turns it back
on. Keys arriving over the whole run would otherwise be echoed into the
middle of whatever line the console was printing, and a self-test line
with `cosmo Types 42!` through it is a boot the harness cannot parse --
which is exactly what happened, and cost two of the run's timing lines.

The guest cannot know by itself whether anything will type, so the
harness says so through `fw_cfg` (`opt/cosmo/keytest`, the shape the
network test already used). Without it the test skips; with it, silence
is a failure -- which is what makes "the driver stopped delivering keys"
a red CI run rather than a quiet skip. The guest prints
`HID-KEYTEST-READY` and the harness waits for it, so the keys cannot
arrive while an earlier test still owns the tty.

**`usb-hub-unplug`** (the `QEMU_KBD=hub` shape). The hub's root port is
taken away with a device behind it: the hub and its child both leave the
bus, both releases run, and putting the port back brings the hub and --
after its worker's debounce and reset -- the device behind it back under
the same names. What this proves is the shape of the teardown: the core
runs the hub driver's `remove` with its own lock held, and `remove`
joins the worker whose last act is to take the children down, so a child
teardown that reached for that lock again would deadlock here and
nowhere else. Nothing in an ordinary boot runs that path, which is why
it is a test and not a review note.

**`usb-enum`** also checks the keyboard: one interrupt IN endpoint, a
boot-protocol interface, a packet of at least 8 bytes, a non-zero
interval -- the first interrupt endpoint in the tree, and the first
check that the controller driver's periodic path produces a device that
works.

## Shapes

- `QEMU_USB=0 gmake test`: no controller; the modules load, `xhci`
  binds nothing, every USB test skips with a message, and the boot test
  does not require the device lines. Both architectures, as chain steps.
- `QEMU_KBD=hub gmake test`: the keyboard moves behind `-device
  usb-hub`, so the hub driver binds, its worker enumerates the device on
  its port, and the whole suite runs one tier down -- the keyboard is
  `usb0-6.1`, its `struct device` parent is the hub, and `usb-enum`
  recomputes its depth, route and root port from the hub's. Both
  architectures, as chain steps.
- `QEMU_KBD=0 gmake test`: no keyboard; `usb_hid` loads and binds
  nothing, and `hid-keyboard` skips.
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
- (CI, on the AHCI unit's PR; two findings from one symptom.) On CI's
  aarch64 QEMU the four-thread benchmark on `sda` ended with the
  controller reporting Event Ring Full (completion code 21) and being
  declared dead. The first fix closed a real but different hazard: an
  event landing between the handler's "caught up" check and its `ERDP`
  write, with `EHB` still set, raised no interrupt and waited for the
  next one, which for a serial device waiting on that event never comes
  — the handler now looks once more after clearing `EHB`, and the AHCI
  handler clears `IS` first for the same edge-triggered reason. CI
  failed again the same way, and the real mechanism was this: the
  handler wrote `ERDP` only when it had caught up, but a device model
  that finishes a transfer on the doorbell write posts the resulting
  event while the handler is still running — and every completion of the
  storage driver submits the next transfer from its callback, so one
  interrupt turned into a chain of hundreds of events, none of which the
  controller saw dequeued; after 255 it declared the ring full. The
  handler now writes `ERDP` after every event it consumes (`EHB` left
  set) and once more with `EHB` when it is done. QEMU 11 on the
  development host completes transfers on a bottom half, so each
  doorbell's events arrived in a later interrupt and the chain never
  formed there.
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

### Found by the keyboard and the hub

- **A transfer's buffer may not be a kernel stack.** The hub's first
  version read four bytes of port status into a stack array; the kernel
  stack lives in the arena and has no direct-map address, so `dma_map`
  refused it, every `GET_STATUS` failed, and the hub reported eight
  ports with nothing behind any of them -- no error, just an empty tree.
  The status buffer is now part of the hub's own allocation (U10), and
  the port-status failure is logged instead of only counted.
- **A withheld command is not a completed one.** The AHCI fault
  injector fills a slot and never writes its `PxCI` bit, which is a
  state real hardware cannot produce: the completion scan reads that bit
  clear and takes the command for finished, so the *next interrupt from
  any other command* completed the hung one successfully and
  `ahci-timeout` failed with the read returning 0 while the layer
  counted a timeout. Adding a second console sink is what slowed the
  boot enough to interleave them. The injected slot is now excluded from
  the scan (`withheld`), which is what the injection meant all along.
- **A bio submitted into a recovery the layer had only just decided
  on.** With the injection faithful, the AHCI timeout path ran for real
  on every round -- and the racing readers `ahci-timeout` has submitted
  since the AHCI unit started failing with `-EIO`. The window is in the
  block layer, not the driver: the layer increments the timeout counter
  and then calls the driver, and a bio submitted in between is accepted
  into a free slot a moment before the recovery fails everything the
  device holds. The layer now marks the device recovering across that
  call and parks arrivals in the pending queue (`deferred`), which is
  what the queue is for; the driver's own `recovering` flag still covers
  the restarts the layer knows nothing about.
- **The keyboard driver counts presses, not keys held** -- and the
  first version of the test could not tell. With the press-detection
  diff removed, `cosmo Types 42!` still arrived exactly right, because
  the harness typed one key at a time and every report between two
  presses was empty; a driver that reports what it holds and one that
  reports what changed are the same driver until two keys are down at
  once. The test now types a second line with the keys overlapping (x
  down, y down, x up, y up), and with the diff removed it reads `xxyy`
  where it wants `xy`. The lesson is about tests, not keyboards: a
  harness that types politely proves less than a person in a hurry.

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
- **Transaction translators.** A full- or low-speed device behind a
  *high-speed* hub is reached through that hub's TT, whose slot and port
  the controller must be told. QEMU offers a full-speed hub only, and a
  TT belongs to a high-speed one, so the `QEMU_KBD=hub` shape exercises
  the route string and the depth and cannot touch the TT fields. They
  are written to the specification and untested here: TT programming can
  be wrong, pass every test in this tree, and fail on the first
  high-speed dock or monitor hub. That is a known hole, not an
  oversight.
- **Hubs behind hubs**, and the depth limit (`-ELOOP` past five tiers):
  reviewed, not run. QEMU will nest hubs by hand
  (`-device usb-hub,port=2.1`) and nothing in the driver treats a hub's
  parent specially, but the harness runs one tier.
- **Port power switching and over-current recovery**: ports are powered
  once and an over-current report is a warning.
- A HID device that refuses the boot protocol, and every non-keyboard
  HID device: refused with a line, by review.
- Isochronous endpoints, full- and low-speed devices on a *root* port
  (QEMU attaches the disk to a USB3 port and the hub takes the
  full-speed path; the EP0 size update path for full speed is written to
  the specification and untested), 32-bit-DMA controllers (`AC64` = 0;
  the mask is set, the allocator honours it, no model to try it on),
  scratchpad buffers (both QEMU models ask for none: the code path is
  written and untested).
- Real hardware, as for every driver here (§61).
