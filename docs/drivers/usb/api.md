# USB host stack: API

Two modules and one header. A class driver sees `drivers/include/drivers/usb.h`
and nothing of the controller; the kernel sees only the `usb` bus and the
block device the storage driver registers.

## Binding

| | |
| --- | --- |
| Module `xhci` | `modules/xhci.ko`, `MODULE_CAP_DRIVER`, no dependencies; holds the USB core (`usb.c`) and the controller driver (`xhci.c`) |
| Module `usb_storage` | `modules/usb_storage.ko`, `MODULE_CAP_DRIVER`, depends on `xhci` |
| Module `usb_hid` | `modules/usb_hid.ko`, `MODULE_CAP_DRIVER`, depends on `xhci` |
| Module `usb_hub` | `modules/usb_hub.ko`, `MODULE_CAP_DRIVER`, depends on `xhci` |
| Controller | bus `pci`, class `0c/03` (serial bus, USB) with programming interface `0x30` checked in probe; UHCI/OHCI/EHCI functions are refused `-ENODEV` (the model logs `probe failed`) |
| Devices | bus `usb`, one `struct usb_device` per attached device, named `usb<controller>-<port>` on a root port (`usb0-1`) and `<hub>.<port>` below one (`usb0-6.1`); `parent` = the controller's PCI device, or the hub |
| Class driver | `struct usb_driver` with a `struct usb_id` table: `USB_ID_CLASS` matches an interface's class/subclass/protocol, `USB_ID_VENDOR` the device's `idVendor`/`idProduct` |
| Storage | interface `08/06/50`; registers a `struct blkdev` under the prefix `sd` (`sda`), `dev` = the controller |
| Keyboard | interface `03/01/01` (HID, boot protocol, keyboard); no device of its own -- characters go to the console tty through `tty_input`. A HID interface with another subclass is refused with a line |
| Hub | interface `09/00/xx`; enumerates the devices on its ports as children of itself |
| Interrupt | one MSI-X vector (entry 0) on CPU 0; single-message MSI when the function has no MSI-X; no INTx |

## For a class driver (`drivers/include/drivers/usb.h`)

**`int usb_register_driver(struct usb_driver *)`,
`void usb_unregister_driver(struct usb_driver *)`** — as
`driver_register`: probes attached devices; `probe(udev, intf, id)`
receives the interface the id matched, with the model's lock held;
`remove(udev)` runs on disconnect or unload with the device's rings
still there.

**`struct device *usb_dma_dev(const struct usb_device *)`** — the device
to hand the DMA API and to name as a blkdev's `dev`: the controller
(invariant U1). Never `&udev->dev`.

**`int usb_control_msg(udev, request_type, request, value, index, buf,
len, timeout_ns)`** — a control transfer on EP0, thread context; returns
the bytes moved or `-errno`. **`int usb_bulk_msg(udev, ep, buf, len,
&actual, timeout_ns)`** — one bulk transfer; 0 with `*actual`, or
`-errno`. `timeout_ns` 0 means `USB_TIMEOUT_NS` (1 s). On the bound the
request is cancelled (`-ETIMEDOUT`).

**`int usb_submit(struct usb_request *)`** — asynchronous, any context.
Two fields are for tests only: `debug_dma` (a bus address used unmapped
for one segment: the IOMMU fault test) and `debug_no_doorbell` (the TRBs
go on the ring and the controller is never told: a request that stays in
flight until cancelled).
The request names the device, the endpoint (`ep`, a
`bEndpointAddress`; 0 for control with `setup` filled), the buffer
(`buf`/`len`, direct-map memory) or, for bulk, `sgs`/`nr_sgs`
(physically contiguous segments), and `done`, which runs in interrupt
context and must not block. The HCD owns the request until `done`;
`status` is 0, `-EPIPE` (stall: the endpoint is halted), `-EOVERFLOW`,
`-EIO`, `-ETIMEDOUT`/`-ECANCELED` (cancelled) or `-ENODEV` (the device
is gone). Refusals (`-ENODEV`, `-EINVAL`, `-ENOSPC`) mean `done` will
not run.

**`int usb_cancel(struct usb_request *, int status)`** — thread context:
stops the endpoint, empties its ring; every request on it completes
(`status` for this one, `-ECANCELED` for the rest) before it returns;
`-ENOENT` if the request had already completed.

**`int usb_clear_halt(udev, ep)`** — after `-EPIPE`: the controller's
`Reset Endpoint` and `Set TR Dequeue`, then `CLEAR_FEATURE(ENDPOINT_HALT)`
to the device.

Descriptors: `udev->desc` (device), `udev->config`, `udev->intf[i]` with
`.desc` and `.ep[k].desc` for alternate setting 0 of every interface,
`udev->raw_config`/`raw_len` for the bytes as read. Limits:
`USB_MAX_INTERFACES` 4, `USB_MAX_ENDPOINTS` 8 per interface,
`USB_CONFIG_MAX` 512.

**`int usb_hub_port_connected(struct usb_device *hub, unsigned port, enum usb_speed speed, struct usb_device **out)`,
`void usb_hub_port_disconnected(struct usb_device *child)`** — what
`usb_port_connected` and `usb_port_disconnected` are for a root port,
one tier down. Thread context, because enumeration is control transfers.
The new device's `parent` is `hub`, its name is the hub's plus `.port`,
its `depth` is the hub's plus one, and its `route` is the hub's with
this port in the hub's nibble. `-ELOOP` past `USB_MAX_DEPTH` (five
tiers, which is what a route string holds). On success `*out` holds a
reference the caller gives back by passing it to
`usb_hub_port_disconnected`.

**`struct usb_device` fields for where a device is** — `parent` (the
hub, or NULL on a root port; a reference is held), `port` (the port on
that parent), `root_port`, `route` and `depth`. A driver has no reason
to read them; the controller driver does, and the self-test checks they
agree with each other.

## The controller seam (`struct usb_hcd_ops`, within the module)

`enable_device` (slot and address; EP0 usable), `update_ep0` (the
descriptor's `bMaxPacketSize0`), `configure` (contexts and rings for the
configuration's endpoints), `disable_device` (stop, disable the slot,
complete what was in flight `-ENODEV`), `submit`, `cancel`,
`reset_endpoint`, and `debug_port(hcd, port, connected)` — tests only:
the port worker's own disconnect or connect path on demand. The core
calls `usb_port_connected`/`usb_port_disconnected` from the controller's
worker; `usb_hcd_register` names the controller, `usb_hcd_unregister`
disconnects every port first. `hcd->enumerated` and `hcd->released`
count devices enumerated and `usb_device` releases run, for the tests.

## Log lines

```text
[ INFO] xhci0: pci:BB:DD.F: xHCI 1.00, 8 ports, 64 slots, 32-byte contexts, 0 scratchpad(s), 64-bit DMA
[ INFO] usb: usb0-1: 46f4:0001 at super speed, 1 interface(s), class 08/06/50
[ INFO] usb-storage: usb0-1 is sda: QEMU QEMU HARDDISK 2.5+, 16384 blocks of 512 bytes
[ INFO] blk: sda: 16384 sectors of 512 bytes (8 MiB)
[ INFO] usb: usb0-1: disconnected
[ INFO] usb-storage: usb0-1: sda removed (N exchanges, N failures, N recoveries)
[ WARN] usb-storage: sda: command timed out in phase P; resetting
[ WARN] usb: usb0-1: bulk transfer on ep 0x81 timed out
[ERROR] usb: usb0-1: <enumeration step>: <errno>
[ERROR] xhci0: command type T did not complete in 1000 ms; the controller is dead
```

The first four are what the boot test requires when the controller is
present (`QEMU_USB` not `0`); the `module: loaded xhci` and `loaded
usb_storage` lines always.

## Counters

`struct blkdev` counts reads, writes, flushes, errors and timeouts for
`sda` as for every disk; the driver adds nothing to them. The storage
driver keeps `exchanges`, `failures` and `recoveries` (reported at
removal); the controller keeps events, transfers, commands and
completion-code errors (reported at removal). IOMMU faults provoked
through `sda` are attributed to the controller's requester id
(`iommu_stats.by_requester`).

## QEMU

The keyboard is `-device usb-kbd`, on a root port by default and behind
`-device usb-hub` with `QEMU_KBD=hub`; `QEMU_KBD=0` leaves it out.
Nothing types at it by hand: the boot test opens QEMU's monitor protocol
socket (`QEMU_QMP`) and sends key events (`input-send-event`), and tells
the guest to expect them through `fw_cfg` (`opt/cosmo/keytest`), which
is why the self-test can tell "nobody typed" from "no keyboard".

`scripts/qemu-run.sh` adds `-device qemu-xhci,id=xhci0` and
`-device usb-storage,bus=xhci0.0,drive=usbdisk` over an 8 MiB image
(`QEMU_USBDISK`, default `usb.img` beside the others for a hand-run
`make run`; the boot test hands every run a fresh zeroed file beside its
log) on both machine types. `QEMU_USB` selects: `qemu` (default), `nec` (the NEC uPD720200
model, `nec-usb-xhci`), `0` (no controller: the USB tests skip and the
boot test does not require the device lines). QEMU attaches the disk to
a USB3 port, so it enumerates at super speed with 1024-byte bulk
packets; a `usb-storage` on a USB2-only controller would be high speed
with 512.

## Kernel exports added for these modules

`thread_exit`, `thread_join`, `sched_block_current`, `waitqueue_init`,
`waitqueue_prepare`, `waitqueue_finish`, `waitqueue_wake_all` (a module
thread that waits on a queue), `arch_percpu_get` (`preemptible()` from
a module), `faultinject_should_fail` (debug builds), and -- with the
keyboard -- `tty_console` and `tty_input`, so an input driver can hand
its bytes to the console tty exactly as the UARTs do. No kernel
interface changed shape; `struct iommu_stats` gained the per-requester
tally and `errno.h` the values `ECANCELED` and `EOVERFLOW`.
