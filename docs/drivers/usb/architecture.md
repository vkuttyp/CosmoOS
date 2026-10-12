# USB host stack: architecture

The subsystem `docs/audit/next-subsystem-usb.md` named: an xHCI host
controller driver, root-hub enumeration and class drivers, later joined
by a keyboard and external hubs. Constitution sections 25 (drivers
never touch bus layout themselves), 26 (DMA through the API), 28 (the
block layer knows no driver), 21 (no mechanism without a measured need)
and 60 (no UHCI, OHCI or EHCI driver). Spec: xHCI 1.2; USB mass storage
bulk-only transport; HID boot protocol.

## Where it sits

```text
   block layer (sda)      console tty          usb devices behind a hub
        │ blk_register       │ tty_input            │ usb_hub_port_connected
   usb_storage.c          usb_hid.c             usb_hub.c        class drivers
        │  usb_control_msg, usb_bulk_msg, usb_submit, usb_cancel, usb_dma_dev
   usb.c + usb_desc.c     the "usb" bus, struct usb_device, enumeration,
        │                 configuration parsing, class-driver binding
        │  struct usb_hcd_ops (internal to the xhci module)
   xhci.c                 rings, contexts, commands, event ring, ports,
        │                 worker thread xhci/<n>
   drivers/pci   BAR0, MSI-X/MSI     kernel/device/dma.c   rings, contexts,
                                     transfer buffers (the controller's device)
```

Four modules, built from `build/module.mk` and loaded in this order
from the boot archive, after `e1000e`:

| Module | Sources | Depends on |
|---|---|---|
| `xhci` (`modules/xhci.ko`) | `drivers/usb/usb.c`, `usb_desc.c`, `xhci.c` | nothing |
| `usb_storage` | `drivers/usb/usb_storage.c` | `xhci` |
| `usb_hid` | `drivers/usb/usb_hid.c` | `xhci` |
| `usb_hub` | `drivers/usb/usb_hub.c` | `xhci` |

Each is a `COSMO_MODULE(..., MODULE_CAP_DRIVER)` reaching the kernel
through exported symbols. The interface between the core and class
drivers is `drivers/include/drivers/usb.h`; the controller's registers
and context layouts are in `drivers/usb/xhci.h`. `scripts/qemu-run.sh`
attaches `-device qemu-xhci` with a `usb-storage` disk on both machine
types (`QEMU_USB`), and a `usb-kbd`, optionally behind a `usb-hub`
(`QEMU_KBD`).

## Purpose

Carry the first bus whose devices arrive after boot, have a parent that
is itself a device, and leave with I/O in flight; and drive the devices
on it that the system uses: a disk, a keyboard and hubs.

## Responsibilities

- xHCI controllers: PCI class `0c/03` with programming interface `0x30`
  checked in `xhci_probe`; bring-up, the command ring, one event ring
  and interrupter, transfer rings per endpoint, one interrupt (MSI-X
  entry 0, else single-message MSI), root-port events on the worker.
- The core: the `usb` bus (`usb_bus`), one `struct usb_device` per
  attached device named by its path (`usb0-1`, `usb0-6.1`),
  enumeration through `usb_hcd_ops`, configuration parsing of
  untrusted bytes (`usb_parse_config` in `usb_desc.c`, U5), matching
  `struct usb_driver` id tables, synchronous and asynchronous transfers
  and their cancellation (U2, U10).
- Topology: `parent`, `port`, `root_port`, `route` and `depth` on each
  device; up to `USB_MAX_DEPTH` (5) tiers (U8).
- DMA on every device's behalf through the controller's
  `struct device`, `usb_dma_dev` (U1).
- Children removed before their parent, by the bus (U7).
- Class drivers: bulk-only SCSI storage as a `struct blkdev` under the
  prefix `sd`; a boot-protocol keyboard feeding `tty_input` on
  `tty_console()`; hubs, whose worker (`usb-hub`) reports port changes
  through `usb_hub_port_connected` and `usb_hub_port_disconnected`.
- Freeing controller and slot DMA only after the controller
  acknowledges the stop or halt (U12, U13, U14).

## Non-responsibilities

- UHCI, OHCI and EHCI functions: refused `-ENODEV` in probe.
- Isochronous transfers, a HID report-descriptor parser, alternate
  settings other than 0, configurations other than the first, more
  than one class driver per device.
- Hub port power switching, over-current recovery, suspend, link power
  management, streams, multiple interrupters (design.md, "What is
  deliberately not here").
- Queuing and ordering for the disk (the block layer) and line
  discipline for the keyboard (the tty).

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `usb_register_driver`, `usb_unregister_driver`, `struct usb_driver`, `struct usb_id` | `drivers/usb.h` | class drivers |
| `usb_control_msg`, `usb_bulk_msg`, `usb_submit`, `usb_cancel`, `usb_clear_halt`, `usb_dma_dev` | `drivers/usb.h` | class drivers |
| `usb_hub_port_connected`, `usb_hub_port_disconnected` | `drivers/usb.h` | `usb_hub` |
| `struct usb_hcd_ops`, `usb_hcd_register`, `usb_port_connected` and the rest of the HCD side | `drivers/usb.h` | `xhci.c` within the `xhci` module |
| `struct blkdev_ops`, `blk_register`, `bio_complete` | `kernel/blk.h` | `usb_storage` |
| `tty_console`, `tty_input` | `kernel/tty.h` | `usb_hid` |
| `pci_register_driver`, `pci_map_bar`, `pci_msix_*`, `pci_msi_*` | `drivers/pci.h` | `xhci.c` |
| `dma_set_mask`, `dma_alloc/free`, `dma_map/unmap` | `kernel/dma.h` | `xhci.c`, class drivers through `usb_dma_dev` |
| `xhci_test_disable_ack`, `xhci_test_recover_disable_ack`, `xhci_test_halt_ack`, `xhci_test_recover_halt_ack`, `xhci_test_cancel_ack` | `CONFIG_SELFTEST` exports | the kernel's USB self-tests |
