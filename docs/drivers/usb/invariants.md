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
