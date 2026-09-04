# Device infrastructure: design

## Data structures

### Device model (`kernel/include/kernel/device.h`)

```c
enum resource_type { RES_MMIO, RES_IO, RES_IRQ };
struct resource { enum resource_type type; uint64_t start; uint64_t size; unsigned flags; };
#define DEVICE_MAX_RESOURCES 8

struct bus_type {
    const char *name;                                  /* "pci", "virtio" */
    bool (*match)(struct device *dev, struct device_driver *drv);
    struct list_node devices, drivers;                 /* under g_device_lock */
    struct list_node link;
};

struct device {
    struct kobject obj;                                /* type device_type; release frees nothing */
    char name[32];                                     /* "pci:00:04.0", "virtio2" */
    struct bus_type *bus;
    struct device *parent;
    struct device_driver *driver;                      /* NULL until probed */
    void *drvdata;                                     /* driver private, cleared at remove */
    struct resource res[DEVICE_MAX_RESOURCES];
    unsigned nr_res;
    uint64_t dma_mask;                                 /* default 32 bits */
    enum device_state { DEV_UNBOUND, DEV_BOUND, DEV_FAILED } state;
    struct list_node bus_link;                         /* bus->devices */
};

struct device_driver {
    const char *name;
    struct bus_type *bus;
    const void *match_data;                            /* bus-specific id table */
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    struct list_node bus_link;
    unsigned bound;                                    /* devices currently bound */
};
```

A device embeds a `kobject` for the lifetime rules of section 10: the
owner (the bus that enumerated it) holds one reference; a bound driver
holds none because `driver_unregister` removes it synchronously; a
lookup (`device_find`) returns a referenced pointer. `release` of a
device type is a no-op in this phase because every device is a static
or bus-owned allocation that lives until shutdown; the field exists so
hot-plug can free.

Probing: `device_register` appends to the bus, then walks the bus's
drivers and calls `match` then `probe` for the first match; a probe
failure records `DEV_FAILED` and the errno in the log, and the device
stays registered (visible in `device_dump`). `driver_register` walks the
bus's unbound devices the same way. `driver_unregister` calls `remove`
on each bound device (in reverse registration order) and unbinds it.
All of this runs under the device-model lock: one mutex
(`g_device_mutex`) wrapped by `model_lock()`/`model_unlock()`, which
record the owning thread and a nesting depth so the owner may re-enter.
That recursion is deliberate: a bus driver's `probe` (virtio-pci)
registers the child devices it discovers, and its `remove` unregisters
them, all from inside the outer `driver_register`/`driver_unregister`.
Probe and remove therefore run with the model locked and must not wait
for another thread that needs the model.

Lock order: `g_modules_lock` (a module's `init` registers drivers) →
device-model lock → `kernel_space.lock`. A driver's interrupt handler
takes only its own spinlock.

### PCI (`drivers/include/drivers/pci.h`)

```c
struct pci_bar { uint64_t base; uint64_t size; bool io; bool is64; bool prefetch; };
struct pci_device {
    struct device dev;                                 /* name "pci:BB:DD.F" */
    uint8_t bus, slot, func;
    uint16_t vendor, device, subsys_vendor, subsys_id;
    uint8_t class, subclass, prog_if, revision, header_type, irq_pin;
    struct pci_bar bar[6];
    uint8_t cap_msi, cap_msix;                         /* capability offsets or 0 */
    struct pci_msix { volatile uint32_t *table; unsigned count; unsigned used; int *vectors; } msix;
    struct list_node link;                             /* g_pci_devices */
};
struct pci_id { uint16_t vendor, device; uint8_t class, subclass; unsigned flags; };  /* PCI_ANY */
```

Config access: `pci_cfg_read{8,16,32}` and writes go through ECAM when
`acpi_find_table("MCFG")` yields a segment 0 mapping (mapped once with
`vm_map_phys` UC for the bus range); otherwise through
`arch_pci_legacy_read/write` (x86: 0xCF8/0xCFC under a spinlock).
Enumeration (`pci_init`): scan bus 0, every slot, function 0 and, if
multi-function, functions 1 to 7; a header type 1 device (bridge)
recurses into its secondary bus. BAR sizing writes all-ones and reads
back with the command register's memory/IO decode bits cleared during
the probe. Every device becomes a `struct pci_device` on the `pci` bus.

The `pci` bus `match` compares the driver's `struct pci_id` table
against vendor/device (with `PCI_ANY`) and, when `PCI_ID_CLASS` is
set, class/subclass. `pci_register_driver` installs `pci_probe_thunk`
as the model-level probe; the model sets `dev->driver` to the driver
being tried before calling probe (and clears it on failure), so the
thunk recovers its `struct pci_driver` from `dev->driver`, finds the
matching id, and calls the typed `probe(struct pci_device *, const
struct pci_id *)`. The virtio bus thunk works the same way.

MSI-X: `pci_msix_enable(pdev, want)` maps the table BAR, allocates
`min(want, table_size)` vectors through `irq_request_msi` with a caller
handler per entry (`pci_msix_request(pdev, index, fn, arg, name)`
programs the entry and unmasks it), sets the function-mask off, and
enables MSI-X. MSI (single message) is the fallback when a device lacks
MSI-X. Legacy INTx is not supported (no `_PRT`).

### MSI in the interrupt layer

```c
struct irq_msi_msg { uint64_t addr; uint32_t data; };
int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu, struct irq_msi_msg *msg);
int irq_release_msi(int vector);
int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data);
```

The vector comes from `arch_vector_alloc` (the same dynamic range 48
to 238 used by GSIs); the message is the x86 format (`0xFEE00000 |
apic_id << 12`, data = vector, fixed delivery, edge). Dispatch and EOI
are the existing paths: an MSI arrives as a plain vector.

### DMA (`kernel/include/kernel/dma.h`)

```c
typedef uint64_t dma_addr_t;
enum dma_dir { DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_BIDIRECTIONAL };
void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags);   /* DMA_ZERO */
void  dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma);
dma_addr_t dma_map(struct device *dev, const void *va, size_t len, enum dma_dir dir);    /* 0 on failure */
void  dma_unmap(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);
void  dma_sync_for_device(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);
void  dma_sync_for_cpu(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir);
int   dma_set_mask(struct device *dev, unsigned bits);
```

`dma_alloc` rounds to pages, takes an order-`n` block from the PMM in
the zone the mask requires (`DMA32` for a 32-bit mask, `DMA` below 24
bits), and returns the direct-map virtual address; the bus address is
the physical address because there is no IOMMU. `dma_map` accepts
direct-map and `kmalloc` addresses (which are in the direct map),
checks the range against the mask, and returns `virt_to_phys`; any
other address (kernel arena, user) yields 0 so a driver cannot hand
the device a stack or a vmalloc buffer by accident. Sync operations
are `barrier()` plus a store fence on x86 (coherent), kept as calls so
a non-coherent port has one place to change. Every function is
non-blocking and lock-free apart from the PMM's own locks; `dma_alloc`
may not be called from interrupt context because the PMM may not.

### Block layer (`kernel/include/kernel/blk.h`)

```c
struct bio {
    struct blkdev *dev;
    uint64_t sector;                 /* in dev->sector_size units */
    uint32_t nsectors;
    enum bio_dir dir;                /* BIO_READ, BIO_WRITE, BIO_FLUSH */
    void *buf;                       /* DMA-able (dma_map succeeds) */
    void (*done)(struct bio *bio);   /* interrupt or thread context */
    void *arg;
    int status;                      /* 0 or -errno, valid once done ran */
    struct list_node link;           /* driver's use */
};
struct blkdev_ops { int (*submit)(struct blkdev *dev, struct bio *bio); };
struct blkdev {
    struct kobject obj;
    char name[16];                   /* "vda" */
    struct device *dev;
    const struct blkdev_ops *ops;
    uint32_t sector_size;            /* 512 */
    uint64_t capacity;               /* sectors */
    unsigned max_sectors;            /* per bio */
    bool read_only;
    struct list_node link;
    uint64_t reads, writes, errors;
};
```

`blk_submit` validates the range, sector count against `max_sectors`,
the direction, read-only devices (`-EROFS`), and that `dma_map` accepts
the buffer, sets `status = -EAGAIN` (in flight) and calls
`ops->submit`; the driver completes with `bio_complete(bio, status)`
which updates the statistics and runs `done`. `blk_read`/`blk_write`/
`blk_flush` are synchronous wrappers built on a stack `completion`;
they reject zero-length or NULL-buffer requests up front and split
large transfers into `max_sectors` pieces. Names are assigned by the
registrant (`blk_register` takes a prefix and appends the next letter:
`vda`, `vdb`). The registry is a list under a mutex; `blk_find` returns
a referenced pointer.

### Entropy (`kernel/include/kernel/random.h`)

A single pool: `uint8_t state[64]`, a 64-bit counter, an estimate of
entropy bits, and a spinlock. `random_add_entropy(buf, len, bits)`
mixes `state = SHA512(state || buf)`; `random_get_bytes` produces
`SHA512(state || counter++)` blocks and reseeds `state = SHA512(state
|| "reseed")` afterwards so past outputs do not reveal the state
(backtracking resistance). It is a deterministic construction seeded by
the TSC at init and by hardware sources later; it is not a certified
DRBG and the API says so. Any context; the lock is a spinlock.

### VirtIO (`drivers/include/drivers/virtio.h`, module `virtio`)

```c
struct virtio_device {
    struct device dev;                       /* on the "virtio" bus, name "virtioN" */
    uint32_t device_id;                      /* 1 net, 2 blk, 3 console, 4 rng */
    uint64_t features;                       /* negotiated */
    const struct virtio_transport *tr;       /* virtio-pci in this phase */
    void *tr_priv;
    struct virtqueue *vq[VIRTIO_MAX_QUEUES]; /* 8 */
    unsigned nr_vq;
    struct pci_device *pdev;                 /* transport-owned */
};
struct virtio_transport {
    uint64_t (*get_features)(struct virtio_device *);
    void     (*set_features)(struct virtio_device *, uint64_t);
    uint8_t  (*get_status)(struct virtio_device *);
    void     (*set_status)(struct virtio_device *, uint8_t);
    void     (*read_config)(struct virtio_device *, unsigned off, void *buf, size_t len);
    int      (*setup_queue)(struct virtio_device *, struct virtqueue *);   /* size, addresses, MSI-X vector */
    void     (*notify)(struct virtio_device *, struct virtqueue *);
};
struct virtqueue {
    struct virtio_device *vdev; unsigned index; unsigned size;
    struct virtq_desc *desc; struct virtq_avail *avail; struct virtq_used *used;  /* one dma_alloc */
    dma_addr_t desc_dma, avail_dma, used_dma;
    uint16_t free_head, num_free, last_used, avail_idx;
    void **cookies;                          /* per descriptor head */
    void (*callback)(struct virtqueue *);    /* interrupt context */
    spinlock_t lock;
    int vector;                              /* MSI-X vector or -1 */
};
```

Device bring-up (`virtio_device_init`, spec 3.1.1): reset, ACKNOWLEDGE,
DRIVER, read features, driver masks to what it supports plus
`VIRTIO_F_VERSION_1` (mandatory: legacy devices are refused),
FEATURES_OK, re-read to confirm, then the driver allocates queues with
`virtq_alloc(vdev, index, callback)` and sets DRIVER_OK. `virtq_add(vq,
sg, out_count, in_count, cookie)` fills a descriptor chain, `virtq_kick`
publishes and notifies (respecting `VIRTQ_USED_F_NO_NOTIFY`),
`virtq_pop(vq, &len)` returns the next used cookie. Each queue has its
own MSI-X vector (`irq_request_msi` through `pci_msix_request`) whose
handler calls the queue's callback; drivers finish requests from there
and wake waiters. The virtio-pci transport (spec 4.1) reads the vendor
capabilities (COMMON, NOTIFY, ISR, DEVICE), maps their BARs once, and
implements the transport operations over the common configuration
structure. The `virtio` bus `match` compares `device_id` with a
driver's id list.

`virtio_blk`: one request queue, `struct virtio_blk_req` header
(type, sector) + data + status byte as a three-descriptor chain per bio;
reads `capacity`, `blk_size` if `VIRTIO_BLK_F_BLK_SIZE`, `seg_max`; the
completion callback pops finished requests and calls `bio_complete`.
`virtio_rng`: one queue, a 64-byte buffer posted at probe; each
completion feeds `random_add_entropy` (full credit) and re-posts, up to
a cap of 4 KiB per boot in this phase so the pool is seeded without
spinning on the device. `virtio_console`: port 0 only (MULTIPORT not
negotiated), two polled queues with no MSI-X vectors: queue 0 receive
with one 256-byte buffer posted and never read, queue 1 transmit; the
console sink copies up to 2 KiB into a DMA bounce buffer, queues it,
kicks, and polls the used ring for at most 200 ms (`clock_now_ns`),
marking itself dead on timeout so the console can never wedge. The
per-driver details are in `docs/drivers/virtio/design.md`.

## Ownership and lifetime

The PCI core owns every `pci_device` (allocated at enumeration, never
freed). The virtio-pci transport owns `virtio_device` (allocated in
the pci probe, freed in remove after the virtio bus unregisters it).
Virtqueues belong to the driver that allocated them and are freed by
`virtq_free` in the driver's `remove`; the DMA ring memory is one
`dma_alloc` per queue. `bio`s belong to the submitter; a driver holds
only a pointer between submit and complete. `blkdev` objects belong
to the driver that registered them; `blk_unregister` waits for no
outstanding bios (the driver drains first; virtio_blk resets the device
which completes everything with -EIO).

## Concurrency

- `g_device_lock`, `g_blk_lock`, `g_pci_lock`: mutexes for registries.
- Per-virtqueue spinlock (IRQ-safe): `virtq_add/kick` from thread
  context, `virtq_pop` from the vector's handler; no other lock is taken
  beneath it.
- The random pool's spinlock is a leaf.
- MSI handlers run with interrupts disabled on the receiving CPU and
  end through the existing EOI path; they call `bio_complete` →
  `complete()` (a waitqueue wake, allowed in interrupt context).
- Console sink writes may run under the console's own lock and in
  panic context, so the virtio-console sink never sleeps and gives up
  after a bounded spin if the device stops consuming.

## Memory

Per PCI device ~200 bytes; the ECAM window for bus 0 to 255 is 256 MiB
of virtual space mapped UC (physical, not RAM); per virtqueue one
contiguous DMA allocation of `16*size + 6+2*size + 6+8*size` bytes
rounded up (a 128-entry queue is 3 pages) plus a cookie array; the
virtio_blk request headers are `dma_alloc`ed per queue slot at probe.
The test disk is 8 MiB of QEMU-side storage, none of it in guest RAM.

## Error handling

Probe failures are logged with the errno and leave the device visible
and unbound. A device without MSI-X and MSI is refused by virtio-pci
(`-ENODEV`, logged) rather than driven by polling. A bio that fails
validation returns `-EINVAL` from `blk_submit` without calling `done`;
every bio a driver accepted is completed exactly once, with `-EIO` when
the device reports an error or is reset. `dma_map` returns 0 for
addresses it cannot vouch for and the callers treat that as `-EINVAL`.

## Performance

One MSI-X vector per queue avoids the ISR read and shared-vector
demultiplexing. Virtqueue operations are O(chain length) under a
spinlock. `blk_read`/`blk_write` are synchronous by design in this
phase; the bio interface is what the page cache will drive
asynchronously later.

## Security

- BAR ranges are mapped uncached and only on driver request; nothing is
  mapped user-accessible.
- The device is untrusted input: every value read from virtio config or
  used rings is bounds-checked (used index wrap, descriptor index
  below queue size, capacity sanity) before use. A misbehaving device
  yields `-EIO`, never a wild pointer.
- DMA addresses always come from `dma_alloc`/`dma_map`, which refuse
  memory outside the direct map; a driver cannot expose kernel
  structures by mistake without an explicit map call.
- Modules that drive hardware declare `MODULE_CAP_DRIVER`; enforcement
  of capabilities arrives with the security phase.

## Testing strategy

Self-tests `pci` (the host bridge and every expected QEMU device are
enumerated with sane BARs and capabilities; config reads agree between
8/16/32-bit accessors), `dma` (alloc/free in each zone, map of a
kmalloc buffer, map of an arena address refused, mask enforcement),
`blk` (write a pattern to the scratch virtio disk, read it back, cross
sector boundaries, out-of-range and misaligned requests refused, stats),
`random` (entropy credited by virtio-rng, two outputs differ,
non-zero), `virtio-console` (a sink named `virtio-console` is
registered). The boot test reads the virtio-console file QEMU writes
and requires the kernel banner in it. `make run`/`make test` attach the
devices through `scripts/qemu-run.sh` (`QEMU_TESTDISK`, `QEMU_VCON`).
Host tests: the virtqueue descriptor logic is pure enough for a host
test (`test_virtq.c`) with a fake transport. Details in `testing.md`.

## Future extensibility

- IOMMU: `dma_map` becomes a real mapping; `dma_addr_t` already differs
  from `paddr_t` in the API.
- Scatter/gather: `struct dma_sg` list passed to `dma_map_sg`, and bios
  gaining a segment vector; virtqueues already take a segment array.
- virtio-net in the networking phase, NVMe and AHCI as PCI drivers, USB.
- Hot-plug: `device_unregister` plus a `release` that frees.
- Legacy INTx once an AML interpreter or a static routing table exists.
