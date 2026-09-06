# VirtIO: design

## Data structures (`drivers/include/drivers/virtio.h`)

```c
struct virtio_device {
    struct device dev;                       /* "virtioN" on virtio_bus */
    uint32_t device_id;                      /* 1 net, 2 blk, 3 console, 4 rng */
    uint64_t device_features, features;      /* offered, negotiated */
    const struct virtio_transport *tr; void *tr_priv;
    struct virtqueue *vq[VIRTIO_MAX_QUEUES]; unsigned nr_vq;   /* 8 */
    struct device *hw;                       /* the PCI function behind it */
    void *priv;                              /* device driver private */
};
struct virtio_transport {                    /* implemented by virtio_pci.c */
    get_features, set_features, get_status, set_status, read_config,
    queue_max_size, setup_queue, teardown_queue, notify
};
struct virtqueue {
    struct virtio_device *vdev; unsigned index, size;
    struct virtq_desc *desc; struct virtq_avail *avail; struct virtq_used *used;
    dma_addr_t desc_dma, avail_dma, used_dma;
    void *ring_mem; dma_addr_t ring_dma; size_t ring_bytes;      /* one dma_alloc */
    uint16_t free_head, num_free, last_used;
    void **cookies;                          /* per head descriptor */
    void (*callback)(struct virtqueue *);    /* NULL: polled queue, no vector */
    spinlock_t lock; int vector; unsigned msix_index;
    uint64_t kicks, interrupts;
};
struct virtio_driver { struct device_driver drv; const uint32_t *ids; uint64_t features;
                       int (*probe)(struct virtio_device *); void (*remove)(struct virtio_device *); };
```

Ring layouts (`virtq_desc` 16 bytes, `virtq_avail` and `virtq_used`
with flexible rings) are the spec's, packed, little endian (x86).

## The bus (`virtio.c`)

`virtio_bus.match` compares `device_id` against the driver's
0-terminated id list. `virtio_register_driver` fills the embedded
`device_driver` with probe and remove thunks that recover the
`virtio_driver` from `dev->driver`, which the model sets to the driver
being tried before calling probe (and clears if probe fails). `virtio_device_register` names the
device `virtio<N>` from a counter, copies the transport device's DMA
mask, and calls `device_register`; because that runs inside the PCI
probe, the device model lock is re-entered (recursive by design).

`virtio_device_init(vdev, wanted)`: write status 0 and spin (bounded)
until the device reads 0; ACKNOWLEDGE; DRIVER; read the 64 feature bits;
refuse without `VIRTIO_F_VERSION_1` (`-ENOTSUP`, status FAILED);
`features = (offered & wanted) | VERSION_1`; write them; set
FEATURES_OK; re-read and refuse if the device cleared it. The driver
then allocates queues and calls `virtio_device_ready` (DRIVER_OK).
`virtio_device_reset` writes 0 and waits; every in-flight buffer is then
dropped by the device (drivers complete their bookkeeping themselves).

## Virtqueues (`virtqueue.c`)

`virtq_alloc(vdev, index, max, callback, &vq)`: the size is the device's
maximum for that queue (`queue_max_size`), capped by `max` and
`VIRTQ_MAX_SIZE` (256), and must be a power of two. One `dma_alloc`
holds the descriptor table (16 × size), the available ring (6 + 2 ×
size, immediately after, 16-aligned) and the used ring (6 + 8 × size,
on the next page boundary), zeroed. Descriptors are chained into a free
list through `next`; `free_head = 0`, `num_free = size`. The transport's
`setup_queue` programs addresses and the interrupt and enables the
queue; on failure everything is freed.

The whole allocation is device-writable, the descriptor table included,
so the driver never reads a decision out of it. The chain structure
lives in driver-private arrays: `shadow_next` (the free list and every
chain link), `chain_len` (per head, descriptors in flight; 0 = free) and
`in_bytes` (per head, device-writable bytes). `desc[].next` is written
from the shadow for the device's benefit and never read back.

`virtq_add(vq, sg, out, in, cookie)`: under the queue's IRQ-safe
spinlock, refuse when `num_free < out + in` (`-ENOSPC`), take
descriptors from the shadow free list, fill `addr`/`len`/`flags`/`next`,
set `WRITE` on the `in` segments and `NEXT` on all but the last, record
`cookie`, `chain_len` and `in_bytes` at the head, write the head into
`avail->ring[idx % size]`, release-fence, bump `avail->idx`.
`virtq_kick`: `dma_sync_for_device` (a full fence), then `notify` unless
the device set `VIRTQ_USED_F_NO_NOTIFY`. `virtq_pop`: under the lock,
compare `last_used` with an acquire load of `used->idx`; read the
element; skip and count (`bad_used`) an `id >= size`, a head that is not
in flight (never posted, already completed, a duplicate); clamp and
count a `len` larger than the chain's `in_bytes`; return the chain to
the free list by walking the shadow for exactly `chain_len` steps; return
the cookie and `len`. Traversal is bounded by the driver's own record,
so no descriptor content can make it loop or index outside its arrays.
`virtq_interrupt` (called by the transport's MSI-X handler) counts and
runs `callback`.

## The virtio-pci transport (`virtio_pci.c`)

A `struct pci_driver` matching vendor `0x1af4`, any device id. Probe:
walk vendor capabilities (`pci_find_capability(PCI_CAP_ID_VENDOR)`),
reading `cfg_type` at `+3`, BAR at `+4`, offset at `+8`, length at
`+12`, and for NOTIFY the multiplier at `+16`; `cap_window` maps each
referenced BAR once (`pci_map_bar`) and bounds-checks offset + length
against the BAR size. Without COMMON and NOTIFY the device is legacy
only and refused (`-ENODEV`). Then `pci_enable_device(bus master)`,
`pci_msix_enable(VIRTIO_MAX_QUEUES + 1)` (at least 2 vectors required),
entry 0 bound to the configuration-change handler and written to
`msix_config`, `device_id = pci device >= 0x1040 ? pci device - 0x1040 :
subsystem id`, and `virtio_device_register`.

Transport operations write the common configuration structure
(`virtio_pci_common_cfg`, offsets `0x00` to `0x30`) through volatile
accessors under a per-device spinlock because `queue_select` and the
feature `select` registers are shared state. `read_config` re-reads
under `config_generation` until stable (up to 8 attempts) and zero-fills
out-of-range requests. `setup_queue`: for an interrupt-driven queue
request MSI-X entry `index + 1` (`pci_msix_request` → `irq_request_msi`,
handler `virtq_interrupt`), then select the queue, write size and the
three ring addresses, write the vector (or `VIRTIO_MSI_NO_VECTOR` for a
polled queue), read it back to confirm the device accepted it, and set
`queue_enable`. `notify` writes the queue index (16-bit) at
`notify_base + queue_notify_off × multiplier`, bounds-checked against
the NOTIFY window. Remove: unregister the virtio device (which runs the
device driver's remove), status 0, `pci_msix_disable`, unmap BARs.

## Device drivers

**virtio_blk** (`virtio_blk.c`): features asked for `SEG_MAX`, `RO`,
`BLK_SIZE`, `FLUSH`, `SIZE_MAX`; reads `capacity` (512-byte units) and
`blk_size` (512 unless negotiated; 512 to 4096, power of two). One
request queue with `vblk_done` as callback. A slot pool of
`queue size / 4` (at least 4) 32-byte DMA slots, each a 16-byte request
header plus a status byte; a bio takes a free slot (spinlock), fills the
header (`type` IN/OUT/FLUSH, `sector` in 512-byte units), maps the bio
buffer with `dma_map`, and adds a chain header → data → status (data
omitted for FLUSH; direction decides which side of the chain the data
is on), then kicks. The completion callback pops cookies (bios), reads
the status byte (OK → 0, UNSUPP → `-ENOTSUP`, else `-EIO`), frees the
slot and calls `bio_complete`. `max_sectors` is 128 × 512 / block size.
Remove: unregister, reset the device, complete leftovers with `-EIO`,
free the queue and pool.

**virtio_rng** (`virtio_rng.c`): no features; one queue; a 64-byte DMA
buffer posted device-writable. Each completion credits `len × 8` bits
via `random_add_entropy` and re-posts until 4096 bytes have been
collected in this boot. Remove resets and frees.

**virtio_console** (`virtio_console.c`): no features (MULTIPORT is not
accepted, so port 0 uses queues 0/1); queue 0 receive with one 256-byte
buffer posted and never read, queue 1 transmit; both polled
(`callback == NULL`, no vectors). The console sink copies up to 2 KiB
into a DMA bounce buffer, adds it, kicks, and polls `virtq_pop` for up
to 200 ms (`clock_now_ns`); if the device stops consuming, the sink
marks itself dead and drops output rather than wedging the console. A
single sink instance is allowed (`-EBUSY` for a second console device).
Remove unregisters the sink, resets, frees.

**virtio_net** (`virtio_net.c`, Phase 8): features `MAC` and `STATUS`
(a device without `MAC` is refused with `-ENODEV`), and since network
unit 11 `CSUM` and `GUEST_CSUM` when offered (QEMU's user-mode backend
offers neither): with `CSUM` the interface has `NETIF_CAP_TXCSUM` and a
packet carrying `NET_CSUM_TCP` gets `NEEDS_CSUM`, `csum_start` and
`csum_offset` in its header; with `GUEST_CSUM` a received `DATA_VALID`
frame is marked `M_CSUM_OK` and a `NEEDS_CSUM` one is finished with
`m_csum_complete` first. No `MRG_RXBUF`, so each received frame is one
buffer with a 12-byte `virtio_net_hdr` in front. Queue 0 receive: 32 mbuf clusters posted
whole (`data = buf`), `dma_map`ped device-writable; the completion
callback drops frames shorter than header plus 14 bytes, sets the
length, strips the header with `m_adj` and hands the mbuf to
`netif_rx`, then re-posts. Queue 1 transmit: the header is prepended
into the mbuf's headroom, a chain of more than four buffers is
linearised first, each buffer is one descriptor, and completions free
the chain; a full ring is `-ENOBUFS`. Both queues are interrupt driven.
The driver registers `struct netif` `eth0` (MTU 1500, MAC from the
configuration space) and marks it up; remove takes it down,
unregisters, resets the device and frees every posted mbuf. Design of
the stack side: `docs/kernel-services/network/design.md`.

## Ownership and lifetime

The transport owns `struct vpci` (with the embedded `virtio_device`) from
PCI probe to PCI remove. A device driver owns its `priv`, its queues
(freed in `remove`) and its DMA pools. Virtqueue ring memory is one
`dma_alloc` per queue. The console sink object lives inside `struct
vcon` and is unregistered before the memory goes away.

## Concurrency

Per-queue spinlock (IRQ-safe) around ring mutation; `virtq_add`/`kick`
from thread context, `virtq_pop` from the MSI handler (or the polling
sink). Per-transport spinlock around the shared select registers.
virtio_blk's slot lock is a leaf. The console sink holds its own lock
while polling with interrupts disabled, which is why the spin is
bounded. Probe/remove run under the device model lock and inside a
module `init`/`shutdown`, so they must not load or unload modules.

## Memory

A 256-entry queue is 3 pages (4 KiB descriptors, the available ring on
the first page's tail, the used ring on its own page). virtio_blk adds a
64-slot × 32-byte DMA pool (one page) plus an inflight array;
virtio_rng one 64-byte DMA buffer (one page); virtio_console 2 KiB + 256
bytes (two pages); virtio_net no pool of its own but 32 mbuf clusters
(64 KiB) held by the device while posted.

## Error handling

Every device-facing value is validated: feature negotiation confirmed
by re-reading FEATURES_OK, queue vector confirmed by re-reading, used
ids bounds-checked, chain walks length-guarded, config reads
generation-checked and range-checked, NOTIFY writes bounds-checked.
Driver probe failures unwind fully (reset, free queues, free DMA) and
leave the virtio device `DEV_FAILED` on the bus. I/O errors surface as
`-EIO` through `bio_complete`.

## Security

The device is untrusted input; a hostile used ring cannot make the
driver dereference beyond its arrays. Bus mastering is on, so the
device can DMA anywhere physical (no IOMMU); the driver only ever hands
it `dma_alloc`/`dma_map` addresses. All modules declare
`MODULE_CAP_DRIVER`.

## Future extensibility

virtio_net over the same transport; indirect descriptors and event index
as negotiable features inside `virtqueue.c`; a virtio-mmio transport for
AArch64 implementing the same `struct virtio_transport`; multiport
console and a receive path once a TTY layer exists; interrupt affinity
per queue.
