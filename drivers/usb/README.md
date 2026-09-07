# drivers/usb

The USB host stack (`docs/drivers/usb/`): the `xhci` module — the
bus-independent core `usb.c` (the `usb` bus, enumeration, transfers,
class-driver binding) and the xHCI controller driver `xhci.c` — and the
`usb_storage` module (bulk-only mass storage as a `struct blkdev`,
`sda`). The shared header is `drivers/include/drivers/usb.h`.

The first bus here whose devices arrive after boot, have a parent that
is a device, and leave with I/O in flight; the device model was built for
those cases and had never met one. Two rules that came out of it: every
DMA on a USB device's behalf goes through the controller's `struct
device` (`usb_dma_dev`; invariant U1), and a bus removes its children
before itself, because the model does not cascade (U7).

`QEMU_USB=0 gmake test` boots without the controller; `QEMU_USB=nec`
uses QEMU's other xHCI model. External hubs, interrupt and isochronous
transfers, and a HID driver are not here (design.md, "What is
deliberately not here").
