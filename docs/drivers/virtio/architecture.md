# VirtIO: architecture

Constitution section 27: VirtIO as a normal device subsystem, a generic
transport abstraction, virtqueues reusable by every device type,
independent of the virtualization implementation (invariant 9). Spec:
VirtIO 1.1 (OASIS), sections 2.1 (status), 2.6 (split virtqueues),
3.1.1 (initialisation), 4.1 (PCI transport), 5.2 (block), 5.3 (console),
5.4 (entropy).

## Where it sits

```text
   modules      virtio_blk.ko      virtio_rng.ko      virtio_console.ko
                (struct blkdev)    (random_add_entropy) (console sink)
                      │                  │                  │
                virtio.ko:  virtio.c     the "virtio" bus, device init, drivers
                            virtqueue.c  split rings: alloc/add/kick/pop/free
                            virtio_pci.c the virtio-pci modern transport, module entry
                      │
   kernel       drivers/pci (pci bus, BARs, capabilities, MSI-X)
                kernel/device (device model, DMA), kernel/block, kernel/core/random, console
```

All of VirtIO is outside the kernel image: `virtio` is a boot module,
the three device drivers are boot modules depending on it (`deps =
"virtio"`), and everything they use from the kernel is an exported
symbol. `drivers/include/drivers/virtio.h` is the shared header.

## Purpose

Drive paravirtualised devices through one implementation of the virtio
device model and one implementation of virtqueues, so a device driver
is only the device-specific protocol (a block request header, an
entropy buffer, a console byte stream).

## Responsibilities

- **The `virtio` bus**: `struct virtio_device` embedding a `struct
  device`, named `virtioN`; `match` by device id list; `struct
  virtio_driver` with typed `probe`/`remove`.
- **Device initialisation** (spec 3.1.1): reset, ACKNOWLEDGE, DRIVER,
  feature negotiation (`VIRTIO_F_VERSION_1` mandatory, legacy devices
  refused), FEATURES_OK confirmation, then queues, then DRIVER_OK.
- **Virtqueues**: split rings in one DMA allocation per queue,
  descriptor free list, `virtq_add` (out then in segments), `virtq_kick`
  (honouring `NO_NOTIFY`), `virtq_pop` (validating what the device
  wrote), interrupt callback or polled operation.
- **The virtio-pci modern transport**: capability walk (COMMON, NOTIFY,
  ISR, DEVICE), BAR mapping, feature/status/config/queue registers, one
  MSI-X vector for configuration changes (entry 0) and one per
  interrupt-driven queue, device id from the PCI id (modern) or the
  subsystem id (transitional).
- **Device drivers**: `virtio_blk` (a block device `vda`), `virtio_rng`
  (feeds the entropy pool up to a per-boot budget), `virtio_console`
  (port 0 transmit as a console sink).

## Non-responsibilities

- `virtio_net`: the transport handles the device; the driver arrives
  with the networking phase.
- Indirect descriptors, event index suppression, packed virtqueues, the
  legacy (pre-1.0) transport, virtio-mmio: not implemented; features
  other than the ones each driver lists are never accepted.
- Multiport console, console receive path, console size/emergency
  write: port 0 transmit only; the receive queue has one buffer posted
  and unread.
- Anything about how the hypervisor implements the device.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `virtio_bus`, `virtio_register_driver/unregister_driver` | `drivers/virtio.h` | device drivers |
| `virtio_device_init/ready/reset`, `virtio_has_feature`, `virtio_read_config*` | `drivers/virtio.h` | device drivers |
| `virtq_alloc/free/add/kick/pop/free_count` | `drivers/virtio.h` | device drivers |
| `struct virtio_transport`, `virtio_device_register/unregister`, `virtq_interrupt` | `drivers/virtio.h` | transports (virtio-pci) |
| `blk_register`, `random_add_entropy`, `console_register` | kernel headers | the three drivers |
