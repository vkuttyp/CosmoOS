# AHCI driver: design

The subsystem `docs/audit/next-subsystem-ahci.md` named, built as it
said: SATA disks through an AHCI host bus adapter — the ICH9-class
controller the `q35` machine has carried, undriven, at `pci:00:1f.2` on
every boot until this unit. The module `ahci` (`drivers/storage/ahci.c`,
`ahci.h`) binds PCI class `01/06` with programming interface `01` and
registers one `struct blkdev` per port that has a SATA disk, named
`ahci<controller>p<port>` (`ahci0p1` for the harness's test disk; on
`q35`, `ahci0p0` is the boot image, which a plain `-drive` puts on the
ICH9's first port — the driver drives the medium it booted from). It
reaches the kernel through exported symbols only. Written to AHCI 1.3.1 and ATA8-ACS, not to QEMU;
`testing.md` lists what only QEMU has exercised.

**Nothing in the kernel changes shape for it.** One optional, test-only
operation joins `blkdev_ops` (`debug_presence`, the shape `debug_dma`
already had) and one fault-injection kind joins the debug enum; no
interface a driver or a filesystem uses moved.

## What this unit answers

The USB report left one question for this one: whether the
DMA-through-the-controller rule, which lives in USB's header, should move
into `struct device` as a DMA-parent pointer once a second bus with
devices behind a requester arrived. **It does not.** A SATA port has no
identity of its own — no configuration space, no requester id, no
descriptors; it is a number in a register — so a port's disk is a
`struct blkdev` whose `dev` is the controller, exactly as an NVMe
namespace is, and there is no `struct device` per port. The rule stays
in USB's header for the one bus whose devices are devices;
`docs/kernel/device/design.md` records that the second case did not need
it. Hotplug did not change the answer either: a disk that arrives or
leaves is a blkdev registered or unregistered, and the blkdev's own
reference counting is the holder protocol it needs.

## Objects

**`struct ahci`** — one per controller: the PCI device, the mapped ABAR,
`CAP` facts (ports, slots, 64-bit, NCQ), the vector, a worker thread, and
`ports[32]`.

**`struct ahci_port`** — one per implemented port: the command list (32
headers), the received-FIS area, 32 command tables (one 1 KiB table
each: a 64-byte command FIS, 16 bytes of ATAPI, and a PRDT of
`AHCI_PRDT_MAX` = 56 entries), a spinlock, the `active` slot bitmap with
a `struct bio *` per slot (or a synchronous waiter), the mapped DMA
segments per slot to unmap at completion, and `disk`, the blkdev of the
disk currently attached (NULL when none). A port outlives its disks.

**`struct ahci_disk`** — one per detected disk, embedding the
`struct blkdev`: the IDENTIFY facts (model, serial, LBA48, sector size,
write cache), a pointer to its port. Allocated at detection, freed by the
blkdev's release when the last holder lets go — and the release touches
nothing but the disk's own memory, because a holder's reference can
outlive `remove`, which has freed the port and controller by then.

## Bring-up (AHCI 1.3.1 §10.1)

1. `pci_enable_device(pdev, true)`; map BAR5 (ABAR).
2. If `CAP2.BOH`, request the BIOS/OS handoff: set `BOHC.OOS`, wait for
   `BOHC.BOS` to clear (2 s bound); a firmware that never lets go fails
   the probe rather than sharing the controller.
3. `GHC.AE`, then an HBA reset: `GHC.HR`, wait for it to clear (1 s),
   `GHC.AE` again (reset clears it).
4. `CAP`: `NP` + 1 ports, `NCS` + 1 slots, `S64A` (→ `dma_set_mask(64)`,
   else 32), `SNCQ`; `PI` for the ports implemented; `VS` for the log.
5. One interrupt: MSI-X entry 0 where the function has MSI-X, else a
   single-message MSI (the ICH9's); on CPU 0. No INTx path: a controller
   with neither is refused and says so.
6. Per implemented port (§10.1.2, §10.3): stop it — `PxCMD.ST` clear,
   wait `PxCMD.CR` clear (500 ms), `PxCMD.FRE` clear, wait `PxCMD.FR`
   clear — allocate and program `PxCLB`/`PxFB`, clear `PxSERR` (write
   ones), `PxIS` (write ones), set `PxIE` to the bits the handler wants
   (D2H register, set-device-bits, DMA setup, task-file error, PhyRdy
   change, port connect change), `PxCMD.SUD|POD`, then `FRE`, then `ST`.
7. Detection: `PxSSTS.DET` = 3 and `IPM` = 1 (or the port has no device
   and is left running for hotplug). `PxSIG` says what: `0x00000101` a
   SATA disk (identified below); `0xEB140101` an ATAPI device and
   `0x96690101` a port multiplier are logged once and left alone — no
   packet commands and no multiplier fan-out in this unit. Wait for
   `PxTFD.STS.BSY|DRQ` to clear (1 s) before the first command.
8. `GHC.IE`.

Detection and identification run in the PCI probe: registering a blkdev
does not take the device model's lock, so nothing here needs the worker
that USB enumeration needed. The boot's disks exist when probe returns.

## Identify

`IDENTIFY DEVICE` (`0xEC`), a host-to-device register FIS in a slot
with a one-entry PRDT of 512 bytes, waited for synchronously (5 s). From
the words: model (27–46) and serial (10–19), byte-swapped and trimmed,
for the log; LBA48 (word 83 bit 10) and the sector count from words
100–103, else words 60–61 (a disk without LBA48 is accepted with the
28-bit count); the logical sector size from words 106 and 117–118 (512
unless bit 12 of word 106 says otherwise; 512 to 4096 accepted, a power
of two); NCQ (word 76 bit 8) and queue depth (word 75 + 1), recorded
and reported but not used (see "Data path"); write cache enabled (word
85 bit 5), so `FLUSH CACHE EXT` is sent only when there is a cache.

Then the blkdev: exact name `ahci<c>p<p>` through `blk_register_named`
— the `sd` letters go out in registration order, and an order between
this driver and the USB disk would be a timing fact dressed up as a
name — `dev` the controller, `sector_size` and `capacity` from IDENTIFY,
`max_sectors` 256 (128 KiB at 512 bytes; the count field of a `DMA EXT`
command goes to 65 536 sectors and the benchmark decides whether more
would move anything), `max_segments` `AHCI_PRDT_MAX`, `timeout_ns` 10 s,
`nr_queues` 1.

## Data path

`READ DMA EXT` (`0x25`) and `WRITE DMA EXT` (`0x35`), 48-bit LBA and a
16-bit count, one command per bio; `FLUSH CACHE EXT` (`0xEA`) for a
`BIO_FLUSH` on a disk with a write cache (completed at once without
one). The PRDT is built from the bio's segments, one entry each (a
segment is page-shaped; an entry takes up to 4 MiB with an even byte
count), mapped with `dma_map` through the controller's device and
unmapped at completion.

**Slots.** Every command takes a free slot of the port's `NCS`; the
header's `PRDTL`, `W` and `CFL` are set, `PxCI`'s bit for the slot is
written, and the HBA issues the commands in its own order — for these
non-queued commands it hands the device one at a time and the device
answers each with a D2H register FIS, so several bios can be outstanding
at the HBA without NCQ's tag machinery. `submit` refuses `-EAGAIN` only
when every slot is taken; the block layer's pending queue holds the
order, and this is the second driver whose refusals exercise that queue
(the USB unit fixed it).

**NCQ is not used.** The benchmark (`testing.md`) compares four threads
reading concurrently through this path against the same through NVMe
and virtio-blk on the same device-model host; §21 says the tag machinery
(`READ`/`WRITE FPDMA QUEUED`, `PxSACT`, the set-device-bits FIS, `READ
LOG EXT` page 10h to learn which tag failed) is written only if that
figure shows device-side parallelism paying, and the figures say it does
not on this host. IDENTIFY's NCQ facts are logged so a real disk's
answer is visible.

**Completion.** The handler reads `IS`, and for each port whose bit is
set reads `PxIS` and writes it back (write-one-to-clear). A slot whose
`active` bit is set and whose `PxCI` bit has cleared is done: its
segments are unmapped, the slot freed, the bio completed with 0 — or, if
the port reported a task-file error, with `-EIO` (see "Errors"). The
synchronous waiter (IDENTIFY, the tests' one-off commands) is a
completion on the caller's stack.

## Errors, timeouts, resets

**A task-file error** (`PxIS.TFES`; `PxTFD.STS.ERR` with the error
byte) stops the port's command processing; §6.2.2.1's recovery is done
on the worker thread: `PxCMD.ST` clear, wait `CR`; `PxSERR` cleared;
if `PxTFD.STS.BSY|DRQ` are still set, a COMRESET (`PxSCTL.DET` = 1 for
1 ms, then 0, wait for `PxSSTS.DET` = 3); `ST` set. The handler notes
the error, `PxCMD.CCS` (the slot that was executing) and a snapshot of
`PxCI`, and wakes the worker, because the recovery waits on registers
and because `PxCI` is gone once `ST` is cleared. The worker then sorts
the active slots by that snapshot: the executing one fails `-EIO`;
those whose `PxCI` bit had already cleared completed before the error
and are completed now with success; those still set were never issued
and are written to `PxCI` again once the port runs. (Reissuing every
active slot would have run the completed ones twice and left their bios
waiting — review, PR #53.)

A synchronous command (IDENTIFY, the tests' one-offs) waits, bounded,
while the port is restarting: a slot taken during a restart would be
absent from the recovery's `PxCI` snapshot and sorted wrongly.

Every restart that fails what the port holds — the block layer's
timeout and a synchronous command's — goes through one function
(`port_restart`): mark the port `recovering`, stop command processing,
fail what is held, clear `PxSERR`, COMRESET if the device is stuck,
start, unmark. The error recovery (which sorts the held slots by the
`PxCI` snapshot instead of failing them) and the reset on demand (which
always resets the link) keep their own sequences and mark the port the
same way. Review found three paths that restarted on their own and each
forgot something.

**The block layer's timeout** (`blkdev_ops.timeout`, its thread) runs the
same port restart synchronously: the victim completes `-ETIMEDOUT`, and
every other outstanding command completes `-EIO` — a port that has
stopped answering does not get its queue replayed on the guess that
only one command was the problem. For the whole of a restart — timeout,
error recovery, a reset on demand — the port is `recovering` and
`submit` refuses `-EAGAIN`: a command accepted while the port is stopped
would be failed with the rest, and the block layer's queue holds it
instead until the victim completes (the racing readers in `ahci-timeout`
found this on aarch64, the same shape the USB unit's review found in
its storage driver). Fault injection (`FI_AHCI_CI`, debug
builds): a slot is filled and its `PxCI` bit never written, so the
command never starts; this is how `ahci-timeout` makes a disk stop
answering.

**COMRESET on demand** (`debug_presence(bd, true)` on a live disk; tests):
the port is stopped, the outstanding commands fail `-EIO`, a COMRESET is
issued, the disk re-identified, and the same blkdev keeps serving if
the serial matches — as after an error recovery that had to reset the
link. `ahci-reset` uses it with reads in flight.

## Hotplug

`PxIS.PCS` (connect status change) and `PRCS` (PhyRdy change) wake the
controller's worker (`ahci/<n>`), which re-reads the port after a 100 ms
debounce. Attach, detach, probe and the reset on demand are serialised
per port by a mutex (`hotplug`) held by the worker, the test-only
operations and removal, so a probe cannot run against a detach in
progress. The worker's rule: `DET` = 3 with a SATA signature and no disk → identify and
register; `DET` ≠ 3 with a disk → the disk is gone. Gone means: the
port stopped, every outstanding command completed `-ENODEV`,
`blk_unregister` (which refuses new bios and waits for submits in
progress), the creator's reference dropped; the `struct ahci_disk` is
freed by the release when the last holder lets go, and the port counts
it. A disk that returns gets a new blkdev under the same name; the old
object is never re-registered.

The harness has no monitor, so `debug_presence(bd, false)` runs the
gone path for a port as if `DET` had read 0 while the hardware stays
attached, and `debug_presence(bd, true)` on the unregistered object —
which only names the controller and port — probes the port and
registers a new disk (`ahci-unplug`). The controller raising `PCS` on a
physical pull is by hand with QMP.

## Removal

`remove`: the worker stopped and joined *first* — a probe still in
flight could otherwise attach a disk behind the detach pass and leave a
blkdev registered over freed memory (review, PR #53) — then every
port's disk gone (as above, `-ENODEV`), every port stopped, `GHC.IE`
cleared, the vector released and synchronised, memory freed, ABAR
unmapped.

## The harness

`q35`: a SATA disk on the built-in ICH9's first port (`-device
ide-hd,drive=sata0,bus=ide.0`) over an 8 MiB image beside the others
(`QEMU_SATADISK`). `virt`: `-device ahci,id=ahci0` first, the disk on
`ahci0.0`. `QEMU_SATA=0` leaves the disk out (q35 keeps its controller:
the driver binds, finds six empty ports and says so; `virt` has no
controller and the driver binds nothing); `QEMU_SATA=cd` attaches an
`ide-cd` instead, so the ATAPI refusal is exercised.

## What is deliberately not here

- **NCQ** — see "Data path"; the benchmark's figures are the reason.
- **ATAPI** (no packet commands), **port multipliers**, **INTx-only
  controllers**: refused, each with one log line.
- **Power management**: no partial/slumber, no staggered spin-up beyond
  `SUD`; the controller runs until removal.
- **Stable naming by identity** (serial, label): `ahci0p0` says where a
  disk is, not what it is; the same concern as every disk here.
- **48-bit counts above 65 536 sectors per command**: `max_sectors` is
  256 and the benchmark saw no reason to raise it.
