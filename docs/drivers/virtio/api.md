# VirtIO: API

Header `drivers/include/drivers/virtio.h`; implemented by the `virtio`
module (`drivers/virtio/virtio.c`, `virtqueue.c`, `virtio_pci.c`).
**ABI stability: exported by the `virtio` module** (15 symbols); a
driver module lists `"virtio"` in its dependencies to resolve them. The
module ABI version covers these too: an incompatible change bumps
`COSMO_MODULE_ABI_VERSION`.

## Drivers

### `struct bus_type virtio_bus` *(exported)*
The bus; `match` compares the driver's 0-terminated `ids` list with
`vdev->device_id` (`VIRTIO_ID_NET` 1, `VIRTIO_ID_BLOCK` 2,
`VIRTIO_ID_CONSOLE` 3, `VIRTIO_ID_RNG` 4).

### `int virtio_register_driver(struct virtio_driver *vdrv)` *(exported)*
Purpose: register a device driver and probe every unbound virtio device
that matches. Inputs: `ids`, `probe` non-NULL, `drv.name` set. Outputs:
0, `-EINVAL`, `-EEXIST`. Sleeps. `virtio_unregister_driver` *(exported)*
removes every bound device (calls `remove`). A module's `init`/`shutdown`
call these.

### `int virtio_device_init(struct virtio_device *vdev, uint64_t wanted)` *(exported)*
Purpose: spec 3.1.1 steps 1 to 6: reset, ACKNOWLEDGE, DRIVER, negotiate
`(offered & wanted) | VIRTIO_F_VERSION_1`, FEATURES_OK. On the legacy
transport (`tr->legacy`): write `offered & wanted` (32 bits) once, with
no VERSION_1 and no FEATURES_OK step. Outputs: 0; `-EIO` (device did
not reset); `-ENOTSUP` (a modern device without VERSION_1, or the
device cleared FEATURES_OK; status is set to FAILED).
Afterwards `vdev->features` holds the negotiated set. Thread context;
bounded spins, no sleep.

### `void virtio_device_ready(struct virtio_device *vdev)` *(exported)*
Step 8: OR `DRIVER_OK` into the status. Call after the queues exist.

### `void virtio_device_reset(struct virtio_device *vdev)` *(exported)*
Write status 0 and wait (bounded) for the device to acknowledge. Every
buffer the device held is dropped without completion; the driver
completes its own bookkeeping. Used on probe failure and in `remove`.

### `bool virtio_has_feature(const struct virtio_device *, uint64_t bit)`
Inline test on the negotiated features.

### `void virtio_read_config(struct virtio_device *, unsigned off, void *buf, size_t len)`, `uint32_t virtio_read_config32(...)`, `uint64_t virtio_read_config64(...)` *(exported)*
Device-specific configuration, read under the generation check;
out-of-range requests return zeros. Any context.

### `void virtio_write_config32(struct virtio_device *, unsigned off, uint32_t val)` *(exported, the virtio-gpu unit)*

Write one 32-bit field of the device configuration at a 4-byte-aligned
`off` (virtio-gpu's `events_clear`). A write outside the configuration
is dropped, as is one on a transport without the operation (the test
transports).

## Virtqueues

### `int virtq_alloc_on(struct virtio_device *vdev, unsigned index, unsigned max, virtq_callback_fn callback, unsigned cpu, struct virtqueue **out)` *(exported, network unit 11)*
`virtq_alloc` with the queue's MSI-X vector routed to `cpu`
(`vq->cpu`, passed by the PCI transport to `pci_msix_request`); a
multi-queue driver binds each queue to the CPU that consumes it.
`virtq_alloc` is the CPU 0 form.

### `int virtq_alloc(struct virtio_device *vdev, unsigned index, unsigned max, virtq_callback_fn callback, struct virtqueue **out)` *(exported)*
Purpose: allocate, program and enable queue `index`. Inputs: `max` caps
the size (0 = device maximum), at most `VIRTQ_MAX_SIZE` (256), power of
two; `callback` is `unsigned (*)(struct virtqueue *vq, unsigned budget)`:
it consumes at most `budget` completions and returns how many. It runs
from the queue's MSI-X vector in interrupt context and, for what one call
left, on that CPU's `irqpoll/N` worker -- never on two CPUs at once, so
completions are consumed in the device's order
(docs/kernel/interrupt/design.md, "Bounded completion handling"). It
must not sleep either way. NULL for a polled queue with no vector. Outputs: 0 and the queue in
`vdev->vq[index]`; `-EINVAL` (bad index, already allocated, non power of
two), `-ENOENT` (the device has no such queue), `-ENOMEM`, `-ENOSPC` (no
MSI-X entry left), `-EIO` (device refused the vector). Sleeps (DMA
allocation). Ownership: the driver, until `virtq_free` *(exported)*,
which disables the queue, releases the vector, then the deferred
completion work (`irq_poll_disable`: no callback runs after it returns),
and the ring memory. `virtq_quiesce` *(exported, the virtio-gpu unit)*
is its first half, the ring kept: for a device whose reset was not
acknowledged, which may still read the ring; that queue is never freed
or used again (U14).

### `int virtq_add(struct virtqueue *vq, const struct virtq_sg *sg, unsigned out, unsigned in, void *cookie)` *(exported)*
Purpose: queue one chain: `out` device-readable segments followed by
`in` device-writable ones, each a bus address and length from
`dma_alloc`/`dma_map`. `cookie` (non-NULL) is returned by `virtq_pop`.
Outputs: 0, `-EINVAL` (empty chain, longer than the queue, NULL cookie),
`-ENOSPC` (not enough free descriptors). Any context; IRQ-safe spinlock.
Not yet visible to the device until `virtq_kick`.

### `void virtq_kick(struct virtqueue *vq)` *(exported)*
Fence and notify unless the device asked for no notifications. Any
context.

### `void *virtq_pop(struct virtqueue *vq, uint32_t *len)` *(exported)*
Purpose: the next completed chain's cookie and the bytes the device
wrote, or NULL when nothing is pending; corrupt entries from the device
are logged and skipped as NULL. Any context; the interrupt callback and
polling sinks call it.

### `int virtq_add_panic(...)`, `void *virtq_pop_panic(struct virtqueue *vq, uint32_t *len)` *(exported)*
The same from panic context: the queue's lock is tried once, untracked
(`spin_trylock_panic`), so lockdep's own lock, which a CPU the panic
stopped may own, is never waited for. `virtq_add_panic` answers `-EBUSY`
and `virtq_pop_panic` NULL when the queue's lock is held. `virtq_kick`
takes no lock: modern virtio-pci reads each queue's notify offset at
setup. The virtio-gpu panic flush is the user.

### `unsigned virtq_free_count(struct virtqueue *vq)` *(exported)*
Free descriptors right now (advisory).

## Transport side (used by `virtio_pci.c`; not exported)

### `struct virtio_transport`
`get_features`, `set_features`, `get_status`, `set_status`,
`read_config`, `queue_max_size`, `setup_queue` (program addresses and
the interrupt for an allocated queue, enable it; no vector when
`vq->callback` is NULL), `teardown_queue`, `notify`, and `release` (mandatory: the last reference to the virtio device dropped after `virtio_device_unregister`; frees the transport's memory. virtio-pci frees its private block here, not in `vpci_remove`, because a `device_find` holder may outlive the remove). Every function is
called with the device's lifetime guaranteed by the transport.

### `int virtio_device_register(struct virtio_device *vdev)`, `void virtio_device_unregister(...)`
Name and register a discovered device on the bus (from inside the
transport's PCI probe; the model lock is recursive), and remove it
(runs the device driver's `remove`).

### `void virtq_interrupt(struct virtqueue *vq)`
Called by the transport's MSI-X handler: counts and runs `callback`.

## Constants

`VIRTIO_STATUS_*` (spec 2.1), `VIRTIO_F_VERSION_1` (bit 32),
`VIRTIO_F_RING_INDIRECT_DESC`/`EVENT_IDX` (defined, never negotiated),
`VIRTIO_MAX_QUEUES` 8, `VIRTQ_MAX_SIZE` 256, `VIRTIO_MSI_NO_VECTOR`
`0xffff`, `VIRTQ_DESC_F_NEXT`/`WRITE`, `VIRTQ_USED_F_NO_NOTIFY`.

## The device drivers' outward interfaces

`virtio_blk` registers `struct blkdev` `vd<letter>` (`kernel/blk.h`);
`virtio_rng` calls `random_add_entropy`; `virtio_console` writes the log
ring so far (`klog_copy`) to its port and then registers a
`struct console_sink` named `virtio-console`; `virtio_net` registers a
`struct netif` named `eth0` (`kernel/netif.h`) and delivers frames with
`netif_rx`. None exports symbols.
