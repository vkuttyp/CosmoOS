# USB host stack: design

The subsystem `docs/audit/next-subsystem-usb.md` named, built as it said:
an xHCI host controller driver, root-hub enumeration, and USB mass
storage as the first class driver. Two modules: `xhci`
(`drivers/usb/xhci.c` and the bus-independent core `drivers/usb/usb.c`,
sharing `drivers/include/drivers/usb.h`) and `usb_storage`
(`drivers/usb/usb_storage.c`). They reach the kernel only through
exported symbols.

The report's point was less the device than the bus. Every bus the
device model had carried was flat and static; USB is the first where a
device arrives after boot, has a parent that is itself a device, and
leaves with I/O in flight. What that did to the model is recorded here
as facts, in "What the device model needed" at the end.

## The pieces and who calls whom

```text
  class driver (usb_storage)          kernel
     |  usb_control_msg / usb_bulk_msg / usb_submit          blk_register, bio_complete
     v                                                       device_register/unregister
  USB core (usb.c) ---- usb_bus ("usb"), struct usb_device, enumeration
     |  struct usb_hcd_ops: enable_device, update_ep0, configure,
     |                      disable_device, submit, cancel, reset_endpoint
     v
  xHCI (xhci.c) ------- rings, contexts, commands, the event ring, ports
     |  pci_*, dma_* (through the controller's struct device), MSI-X
     v
  controller
```

The core never touches a register and the controller driver never
parses a descriptor. The seam between them, `struct usb_hcd_ops`, is
internal to the `xhci` module today; a second host controller would
make it the module boundary.

## Objects and lifetimes

**`struct usb_hcd`** — one per controller: the controller's
`struct device *dev` (the PCI function, the only device that does DMA),
the ops, an index (`xhci0`), the number of root-hub ports and a
`port_dev[]` of the device on each. Owned by the controller driver;
lives from probe to remove.

**`struct usb_device`** — one per attached device, embedding a
`struct device` on `usb_bus`, named `usb<hcd>-<port>` (`usb0-1`), whose
`parent` is the controller's PCI device. Holds the device descriptor,
the whole configuration descriptor (raw and parsed into up to 4
interfaces of up to 8 endpoints), the speed, the root port, the HCD's
slot and per-device state. It is a kobject through its `struct device`:
`device_register` gives the bus a reference, the core keeps the
creator's, a class driver may take more. The release (`usb_device_release`)
frees it; the HCD's per-device state is freed by `disable_device`,
which runs before the release.

One `struct device` per USB device, not per interface. A class driver
binds the device through one of its interfaces (`probe` receives the
interface that matched). A composite device with two interfaces of
different classes would bind one driver and leave the other interface
unused; that is a limitation this unit accepts and names rather than
modelling interfaces as children (`docs/drivers/usb/invariants.md`, U7).

**`struct usb_request`** — a transfer: device, endpoint address, buffer
(direct-map, DMA-able), length, and on completion `actual` and
`status`. Asynchronous: `usb_submit` hands it to the HCD, which owns it
until `done(r)` runs — in interrupt context, and so not allowed to
block, the same rule as a bio's `done`. `usb_control_msg` and
`usb_bulk_msg` are the synchronous shapes over it: a completion on the
caller's stack and a bounded wait.

## The DMA rule

**Every DMA on behalf of a USB device goes through the controller's
`struct device`.** The controller is the PCI requester; the USB device
has no bus address space, no IOMMU domain and no requester id of its
own. `usb_device.dev.dma_mask` is a copy of the controller's for
information; `usb_device.dev.iommu` is NULL and stays NULL, and the DMA
API is never handed a `struct usb_device`'s device. The core exposes
`usb_dma_dev(udev)` so a class driver has one place to get the right
device from, and the storage driver's `debug_dma` provokes a fault that
the IOMMU attributes to the controller's requester id — which is the
truth, and what the `iommu-fault` test asserts.

## Enumeration

Run by the core on the HCD's port worker thread (thread context; the
worker is the only caller, so `hcd->lock` serialises it against remove).

1. The HCD reports `usb_port_connected(hcd, port, speed)` after the
   port is enabled (a USB2 port is reset first, xHCI §4.19.1.1; a USB3
   port enables itself).
2. `enable_device`: the HCD obtains a slot (`Enable Slot`), builds the
   slot and EP0 contexts (speed, root port, EP0 max packet size by
   speed: 8 for low and full, 64 for high, 512 for super) and issues
   `Address Device`. The device is now addressed and EP0 works.
3. `GET_DESCRIPTOR(device, 8)`: the real `bMaxPacketSize0`. A full-speed
   device may say 8, 16, 32 or 64; if it differs from the assumed value
   `update_ep0` (`Evaluate Context`) fixes the EP0 context before
   anything longer is read.
4. `GET_DESCRIPTOR(device, 18)`, then `GET_DESCRIPTOR(configuration,
   9)` for `wTotalLength`, then the whole configuration (at most
   `USB_CONFIG_MAX` = 512 bytes; a device whose configuration is longer
   is refused, not truncated). The parser walks the descriptor chain by
   `bLength`, refusing a length that would run past the buffer or is
   shorter than the descriptor's own header, and records interfaces and
   their endpoints up to the limits; unknown descriptor types are
   skipped by their `bLength`.
5. `configure`: the HCD adds an endpoint context for every endpoint of
   every interface's alternate setting 0 (`Configure Endpoint`), with a
   transfer ring each. Only bulk and interrupt endpoints get rings;
   isochronous endpoints are recorded in the descriptor and given no
   context (no class driver here uses one).
6. `SET_CONFIGURATION(bConfigurationValue)`.
7. `device_register`: the model probes the registered class drivers.
   A probe failure is the model's `DEV_FAILED`, not an enumeration
   failure; the device stays on the bus, visible and addressed.

A failure at any step logs which step and leaves the port alone until
its next status change; there is no retry loop, because a device that
fails enumeration the same way twice a second would fill the log with
one fact.

**Disconnect** is the reverse, in this order: `udev->gone` is set so any
new submit fails `-ENODEV`; `device_unregister` runs the class driver's
`remove` (which for storage is `blk_unregister`, waiting for submits in
progress) while EP0 and the rings still exist, so a driver that wants to
send a last request may; then `disable_device` stops every endpoint,
`Disable Slot`s, and completes every request still on a ring with
`-ENODEV` — nothing is left waiting for a device that is not coming
back; then the creator's reference is dropped and the release frees the
memory once every holder is gone.

**Controller removal** with devices attached disconnects every port
first, then tears the controller down. The model does not cascade a
parent's removal to its children (`device_unregister` knows nothing of
children); the bus does it, in the order above, which is the order the
hardware needs anyway (a slot cannot be disabled after the controller is
reset). See "What the device model needed".

## xHCI (`xhci.c`, written to xHCI 1.2)

**Binding**: PCI class `0c/03` (serial bus, USB) with `prog_if` `0x30`
checked in probe; a UHCI, OHCI or EHCI function is refused with
`-ENODEV` and the model records it as `DEV_FAILED` — there is no driver
for those and, per §60, there will not be one. Both QEMU models
(`qemu-xhci` 1b36:000d, `nec-usb-xhci` 1033:0194) bind by class; no
vendor id is named in the table.

**Bring-up** (§4.2): map BAR0; `CAPLENGTH` locates the operational
registers, `RTSOFF` the runtime registers, `DBOFF` the doorbells;
`HCSPARAMS1` gives slots, interrupters and ports; `HCCPARAMS1.AC64`
decides `dma_set_mask(64)` or 32, `HCCPARAMS1.CSZ` the context size
(32 or 64 bytes). Stop (`USBCMD.RS` clear, wait `USBSTS.HCH`), reset
(`USBCMD.HCRST`, wait for it and `USBSTS.CNR` to clear, 1 s bound).
Then `CONFIG.MaxSlotsEn`, the device context base address array
(`DCBAAP`; one page holds 256 entries), the command ring (`CRCR` with
`RCS`), scratchpad buffers as `HCSPARAMS2` demands (array at
`DCBAA[0]`, one buffer per entry of the controller's `PAGESIZE`), one
interrupter (`ERSTSZ` 1, `ERSTBA`, `ERDP`, `IMAN.IE`), one MSI-X vector
on CPU 0 (MSI if the function has no MSI-X; no INTx path), and
`USBCMD = RS | INTE | HSEE`.

**Rings** (`struct xhci_ring`): one page of 256 16-byte TRBs, the last
a Link TRB back to the first with Toggle Cycle. A producer cycle bit; a
TRB is the controller's when its cycle bit equals the ring's. A TD is
written with its first TRB's cycle bit *inverted*, the rest written,
then the first's flipped with a release fence, so the controller never
starts a TD it can see only part of. `req[i]` names the request that
owns TRB `i`, so a Transfer Event, which carries the TRB's address,
finds its request in one subtraction. The command ring is the same
structure with a single waiter.

**Contexts**: the device context (32 entries of the context size) and
one input context (33 entries) per slot, from `dma_alloc`. The slot
context carries speed, root port and "context entries"; each endpoint
context its type, max packet size, error count 3 and the ring's
dequeue pointer with the cycle state.

**Commands** are serialised by a mutex and waited for on a completion
with a 1 s bound; a command that does not complete marks the controller
dead (`hcd->dead`): every later request fails `-EIO` and the log says
why once. The completion event carries the command TRB's address; the
waiter compares it, so a stale event for an earlier command cannot be
mistaken for the current one.

**Transfers**: a control transfer is Setup (immediate data, `TRT` by
direction), Data (`ISP`, so a short answer is reported), Status (`IOC`);
a bulk transfer is a chain of Normal TRBs, one per physically
contiguous segment of the buffer (`dma_map` per segment), `CH` on all
but the last, `ISP` on all, `IOC` on the last. The handler sums bytes
from every event that names one of the TD's TRBs (`length − residual`)
and completes the request when the event names the last TRB — the
controller generates that event after a short packet too (§4.10.1.1),
which is what makes "one completion per TD" hold. Completion codes:
Success and Short Packet are 0; Stall is `-EPIPE` and halts the
endpoint; Babble `-EOVERFLOW`; Transaction Error `-EIO`; anything else
`-EIO` with the code logged.

**Cancel** (a timeout): `Stop Endpoint`, then `Set TR Dequeue Pointer`
to the ring's enqueue point, completing every request on that ring
with `-ECANCELED` (the one that timed out with `-ETIMEDOUT`). Bulk-only
transport has one exchange in flight per device, so this drops nothing
a class driver was counting on.

**Halt recovery** (`usb_clear_halt`): `Reset Endpoint`, `Set TR
Dequeue` past the halted TD (its request completed `-EPIPE` by the
event that halted it; anything behind it `-ECANCELED`), then the class
request `CLEAR_FEATURE(ENDPOINT_HALT)` to the device. The order is the
specification's (§4.6.8): the controller's view of the endpoint is
reset before the device's. An endpoint that turns out not to be halted
answers `Reset Endpoint` with Context State; it is then stopped before
its dequeue pointer is moved, because `Set TR Dequeue` refuses a
running endpoint.

**The interrupt handler** acknowledges `USBSTS.EINT` and `IMAN.IP`,
drains the event ring until the next TRB's cycle bit is not the
consumer's, and writes `ERDP` with `EHB`. Per event: a Command
Completion wakes the waiter with the code and slot; a Transfer Event
finds the request through the ring and completes it as above, calling
`done` in interrupt context; a Port Status Change sets a flag and wakes
the port worker; a Host Controller Event marks the controller dead.

**The port worker** (`xhci/<n>`, one thread per controller) scans every
port: clears the change bits it saw (write-one-to-clear), and compares
`CCS` with the device it has for the port. Connected and absent: reset
if the port is not yet enabled (USB2), wait for `PRC`, read the speed,
`usb_port_connected`. Disconnected and present: `usb_port_disconnected`.
The worker's first pass runs as soon as it starts (after waiting up to
200 ms for a port to report a change, so devices present at boot are
seen), and the module's init waits for that first pass before it
returns, so the boot's devices are on the bus before the next module
loads and before the self-tests run; after that only a Port Status
Change Event wakes it. Enumeration cannot run inside the PCI probe:
probe holds the device model's lock, and registering the USB device
needs it — which is why the worker exists at all, and why the wait is
in the module's init and not in probe.

**Removal** (`xhci_remove`): disconnect every port, stop the worker,
stop the controller (`RS` clear, wait `HCH`), release the vector and
`synchronize_irq`, free every ring and context, unmap.

## USB mass storage (`usb_storage.c`)

Interface class 08, subclass 06 (SCSI transparent command set),
protocol 50 (bulk-only transport, "BOT"). One bulk IN and one bulk OUT
endpoint. Per exchange: a 31-byte Command Block Wrapper out, data in or
out, a 13-byte Command Status Wrapper in; the CSW's tag must match the
CBW's and its status byte says pass, fail or phase error.

Bring-up on probe: `INQUIRY` (vendor, product, revision for the log);
`TEST UNIT READY` up to 5 times 100 ms apart (a fresh device answers
`NOT READY`/`UNIT ATTENTION` first; `REQUEST SENSE` clears it);
`READ CAPACITY (10)` for the last LBA and block size (512 to 4096
accepted; anything else refused); `MODE SENSE (6)` for the write-protect
bit, tolerated if the device refuses it (many do). Then a
`struct blkdev` registered under the prefix `sd` with `sector_size`,
`capacity`, `max_sectors` 128 (a 64 KiB command, the size the benchmark
decides on), `max_segments` from what one TD can chain (`XHCI_TD_MAX`),
`timeout_ns` 10 s.

The data path is asynchronous and serial: `submit` refuses with
`-EAGAIN` while an exchange is in flight (the block layer keeps order
in its pending list and resubmits), else builds the CBW and submits the
three requests as a chain — each request's `done` submits the next, in
interrupt context, and the CSW's `done` completes the bio. Nothing
sleeps on the I/O path and no thread is parked per request. `BIO_FLUSH`
is `SYNCHRONIZE CACHE (10)`.

`timeout` (the block layer's thread) runs bulk-only mass storage reset
recovery (BOT §5.3.4): cancel whatever is in flight, the class request
`Bulk-Only Mass Storage Reset`, clear both endpoints' halts, and
complete the victim `-ETIMEDOUT`. The exchange slot stays taken for the
whole of it (`recovering`): cancelling runs the transfer's callback,
which would otherwise free the slot, and a bio submitted from another
CPU then would start an exchange on endpoints being reset and be
forgotten by the recovery's tail. A CSW with a phase error runs the
same recovery. A stalled data or status phase clears the halt and reads
the CSW again, as the specification prescribes.

`remove` (disconnect or module unload): `blk_unregister` (refuses new
bios, waits for submits inside the driver), then any exchange still in
flight is completed `-EIO` — the HCD's `disable_device` will have
completed its requests `-ENODEV`, so this is the driver's own
bookkeeping — then the blkdev's creator reference is dropped.

`debug_dma`: a `READ (10)` of one block into the caller's address, so
the IOMMU fault test can make the controller DMA somewhere it may not.

## The harness

`scripts/qemu-run.sh` gives both machines `-device qemu-xhci` and a
`usb-storage` device on it backed by an 8 MiB image beside the other
disks (`QEMU_USBDISK`, default `usb.img`). `QEMU_USB=0` leaves the
controller out; `QEMU_USB=nec` uses the `nec-usb-xhci` model instead,
so both device models are exercised by the chain. The boot test
requires the module to load always, and the device to enumerate and
`sda` to register when a controller is present.

## What the device model needed

Two findings, both named in advance by the report; neither changed a
kernel interface.

- **A device that does not DMA.** `struct usb_device.dev` has a
  `dma_mask` and an `iommu` field like every device, and uses neither:
  its DMA is the controller's. The DMA API keyed by `struct device` is
  the right key — it names the requester — and a bus whose devices sit
  behind a requester passes the requester. The model needed no "DMA
  parent" pointer; the bus's header has the accessor. If AHCI ports
  need the same rule, that is the moment to move it into the model.
- **Removing a parent with children.** `device_unregister` does not
  cascade, and the controller driver removes its children first
  because the hardware order requires it regardless. A model that
  cascaded would have to know the bus's order; leaving it to the bus
  was the right call, and the pattern is written down here for the next
  bus that has children.

## What is deliberately not here

- **External hubs.** The root hub's ports are handled by the controller
  driver; a hub device (class 09) enumerates, is registered on the bus
  and binds nothing. Devices behind it are not enumerated. The report
  names the hub driver as the follow-up unit.
- **Isochronous and interrupt transfers.** No class driver here uses
  them; interrupt endpoints get a context and a ring but no client, and
  isochronous endpoints get neither.
- **Streams, multiple interrupters, per-CPU event rings.** One
  interrupter on one CPU is the shape the benchmark measures; §21
  decides whether more is warranted, and nothing here needs it.
- **Power management**: no suspend, no link power management, no
  `U1`/`U2`. The controller runs until it is removed.
- **Alternate settings** other than 0, and configurations other than
  the first.
