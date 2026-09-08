# USB host stack: invariants

**U1. Every DMA on a USB device's behalf goes through the host
controller's `struct device`, and no other.** The controller is the PCI
requester; the USB device has no bus address space, no IOMMU domain and
no requester id. `usb_dma_dev(udev)` is the one accessor; a class
driver's blkdev names the controller as its `dev`; `usb_device.dev.iommu`
is NULL for life.

Check: `usb-enum` asserts `udev->dev.iommu == NULL` and that the blkdev's
`dev` is on the PCI bus; `iommu-fault` provokes a fault through `sda` and
requires the unit to attribute it to the controller's requester id.

**U2. A request completes exactly once, and only through
`usb_request_complete`.** The HCD owns a submitted request until its
`done` runs; a Transfer Event, a cancel, a halt recovery or a disconnect
completes it, and the ring's ownership table (`req[]`) is cleared before
`done` runs so no second event can find it. `usb_submit` returning an
error means `done` will not run.

Check: `usb-storage` requires the block layer's counters to advance by
exactly the operations issued and the DMA map/unmap counts to balance;
`usb-unplug` requires the bio in flight at the detach to complete once,
with an error.

**U3. The controller never sees a TD it can start before the TD is
whole.** The first TRB of a TD is written with its cycle bit inverted
and flipped last, behind a release fence; the link TRB's cycle bit is
handed over before the producer's flips. A doorbell is rung only after
the flip.

Check: by construction (`ring_put`/`ring_commit`); the bulk path's
chained TDs of up to 16 segments round-trip in `usb-storage`.

**U4. Nothing waits for a device that is not coming back.** Disconnect
sets `gone` (new submits fail `-ENODEV`), runs the class driver's
`remove` while the rings still exist, then stops every endpoint,
disables the slot and completes everything still on a ring with
`-ENODEV`. A bio in flight at the moment of an unplug completes with an
error, not never. A command that does not complete within its bound
marks the controller dead rather than being retried.

Check: `usb-unplug` (a bio in flight at the detach completes with an
error; `blk_find("sda")` is NULL afterwards; the release runs once);
`usb-storage-timeout` (an exchange whose CSW never comes completes
`-ETIMEDOUT` within the block layer's bound and the device works
afterwards).

**U5. Bytes from the device are data, never trusted structure.** The
configuration descriptor is walked by `bLength` with every descriptor
required to fit the bytes read and to be at least its own header long;
one longer than `USB_CONFIG_MAX` is refused, not truncated; a
`bMaxPacketSize0` that is not valid for the speed is refused; a CSW with
the wrong length, signature or tag fails the exchange and runs the
reset recovery. Interfaces and endpoints beyond the static limits are
dropped and counted.

Check: `usb-enum` checks the parsed descriptors against each other and
the raw length; the descriptor parser's refusals are exercised by the
host fuzz target (`fuzz/usb_desc`).

**U6. One command in flight; a command that never completes ends the
controller.** Commands are serialised by a mutex and waited for with a
1 s bound; the completion event is matched to the TRB it names, so a
stale one cannot be taken for the current one. On the bound the
controller is `dead`: every later request fails `-EIO`, the log says so
once, and no further command is issued to a controller whose state is
unknown.

Check: by construction; `usb-storage-timeout` exercises the transfer
timeout path, not this one — a controller that stops answering commands
has no reproducer in QEMU.

**U7. The bus removes its children before itself.** `device_unregister`
does not cascade; the controller driver disconnects every port before
stopping the controller, because a slot cannot be disabled once the
controller is reset. One `struct device` per USB device, not per
interface: a composite device binds one class driver through one of its
interfaces and the others go unused, which is a named limitation and
not a bug.

Check: `xhci_remove` runs `usb_hcd_unregister` first; the module's
shutdown path is covered by `remove` (not exercised at boot: the
controller lives until shutdown).

**U8. Where a device is, is one description, and the controller is told
the same one.** `parent`, `depth`, `route` and `root_port` on `struct
usb_device` are set once, when the device arrives, and agree by
construction: the depth is the parent's plus one, the root port is the
parent's, and the route is the parent's with this port in the parent's
nibble. Nothing else in the kernel is told about hubs -- not `struct
device` beyond its ordinary parent pointer, not the DMA rule (U1: every
transfer still goes through the controller), not the block layer.

Check: `usb-enum` recomputes all four from the parent and compares, on
whichever shape the harness runs (`QEMU_KBD=root` gives depth 0 and
route 0; `QEMU_KBD=hub` gives depth 1 and a route naming the hub's
port).

**U9. A driver's `remove` never waits for a thread that touches the
device model.** `device_unregister` holds the model's lock across the
driver's `remove`, so a `remove` that joined such a thread would
deadlock the moment the thread was inside `device_register`. The hub
driver therefore does not join its worker: `remove` marks it stopping,
cancels its request and returns, and the worker -- which holds a
reference to the hub's device and owns the hub's state from then on --
tidies up and frees it afterwards. For the same reason
`usb_hub_port_connected` and `usb_hub_port_disconnected` take no
`hcd->lock`: the hub driver's `remove` runs with it held too.

Check: `usb-hub-unplug` takes a hub's root port away with a device
behind it, which is the path that deadlocks if any part of this is
broken, and waits for the two releases the rule makes asynchronous.

Known: `xhci_remove` does join its port worker, which touches the model
the same way. It is unreachable today (a controller is removed only by
unloading the module, which nothing does at boot) and is named here
rather than left implicit.

**U9b. A hub's completion never enumerates, and children go before
parents.** The status-change completion runs in interrupt context and
does two things: record the changed ports and wake the worker. Every
step that follows -- port status, reset, feature clearing, enumeration
-- is a control transfer and runs in the worker thread. A child holds a
reference to its hub, so the hub's memory cannot go while a device
behind it exists, and the core removes a device's children before the
device itself, so the order holds even though `remove` cannot wait.

Check: by construction (the completion's only calls are
`waitqueue_wake_all` and `usb_submit`); `usb-hub-unplug` (both devices
leave, the child first, and both come back); the `QEMU_KBD=hub` shape
runs the whole suite with a device one tier down.

**U10. A transfer's buffer is memory the controller can reach.** Every
buffer handed to `usb_control_msg`, `usb_bulk_msg` or `usb_submit` is
direct-map memory (`kmalloc`, `kzalloc`, `dma_alloc`) and never a kernel
stack, which lives in the arena and has no direct-map address.

Check: by review, and by what happens without it -- the hub's first
version read port status into a stack buffer, `dma_map` refused it, and
the hub found no devices behind it at all.
