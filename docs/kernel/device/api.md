# Device infrastructure: API

Every interface a driver or the kernel uses from the device model, the
DMA layer, the block layer, the entropy pool, the MSI additions to the
interrupt layer, and the console sink list. Each entry follows
constitution section 52. **ABI stability: kernel-internal and Module ABI
v1.** Every function marked *exported* is visible to modules; changing
one incompatibly bumps `COSMO_MODULE_ABI_VERSION`.

Common properties unless stated: thread context only (the registries
are mutexes), may allocate, take no lock the caller can observe, and
return negative errnos from `kernel/errno.h`.

## Device model (`kernel/include/kernel/device.h`, `kernel/device/device.c`)

Lock order: `g_modules_lock` (module `init`/`shutdown`) → the device
model lock → `kernel_space.lock`. The model lock is one mutex made
recursive on purpose (`model_lock`/`model_unlock` track an owner thread
and a depth): a bus driver's `probe` runs under it and registers the
child devices it finds; a driver's `remove` unregisters them.

### `void device_init(void)`
Purpose: initialise the model lock. Once, before any bus registers
(`kernel_main` after `module_init`). Never fails.

### `void bus_register(struct bus_type *bus)` *(exported)*
Purpose: make a bus known. Inputs: a static, immortal `bus` with `name`
and `match` set; the lists are initialised here. Failure: panics on a
duplicate pointer or name (a programming error). Sleeps (mutex).

### `struct bus_type *bus_find(const char *name)` *(exported)*
Purpose: look a bus up by name. Outputs: the static bus or NULL. Sleeps.

### `void device_setup(struct device *dev, struct bus_type *bus, struct device *parent, const char *name)` *(exported)*
Purpose: prepare caller-owned storage as a device: zeroes it, initialises
the `kobject` (type `device`, refcount 1), copies `name` (31 bytes kept),
sets `bus`, `parent`, a 32-bit `dma_mask`, state `DEV_UNBOUND`. Any
context; no allocation.

### `int device_add_resource(struct device *dev, enum resource_type type, uint64_t start, uint64_t size, unsigned flags)` *(exported)*
Purpose: record an MMIO, I/O port, or IRQ resource (`RES_MMIO`, `RES_IO`,
`RES_IRQ`); `flags` are bus-defined (PCI stores the BAR index). Fails
with `-ENOSPC` after `DEVICE_MAX_RESOURCES` (8). Call before
`device_register`; no lock.

### `const struct resource *device_resource(const struct device *dev, enum resource_type type, unsigned index)` *(exported)*
Purpose: the `index`-th resource of `type`, or NULL. Any context.

### `int device_register(struct device *dev)` *(exported)*
Purpose: put the device on its bus and probe the bus's registered
drivers in registration order; the first driver whose `match` succeeds
gets `probe`. Ownership: the bus takes one `kobject` reference; the
creator keeps reference 1 and drops it with `device_put` when its own
teardown is done. `dev->release` is mandatory (set after `device_setup`;
`device_release_static` for static objects) and its owner module is
recorded (`kobject_track_code`). Outputs: 0, `-EINVAL` without a
release, or `-EEXIST` if a device of that name is already on the bus. A probe
failure is not a registration failure: the device stays registered with
`state == DEV_FAILED` and `probe_error` set, and is logged. Sleeps; may
be called from a `probe` (recursive lock).

### `void device_unregister(struct device *dev)` *(exported)*
Purpose: `remove` it from its driver if bound, drop it from the bus,
release the bus's reference. The object lives on while `device_find`
holders and the creator hold it; `dev->release` runs from the last put.
Sleeps; callable from `remove`.

### `void device_release_static(struct device *dev)` *(exported)*
An empty release for devices in static storage (tests, immortal roots).

### `int driver_register(struct device_driver *drv)` *(exported)*
Purpose: add a driver to `drv->bus` and probe every `DEV_UNBOUND` device
on that bus (devices left `DEV_FAILED` by another driver are not
retried until that driver unregisters). Inputs: `name`, `bus`, `probe`
non-NULL; `match_data` is what the bus's `match` compares. Outputs: 0
even if nothing matched; `-EEXIST` if already registered. Sleeps.

### `void driver_unregister(struct device_driver *drv)` *(exported)*
Purpose: `remove` every device bound to `drv` (reverse enumeration
order), reset `DEV_FAILED` devices to `DEV_UNBOUND`, forget the driver.
Asserts `bound` reached 0. Sleeps. Module `shutdown` must call this
before its code disappears.

### `vaddr_t device_map_mmio(struct device *dev, const struct resource *res)` *(exported)*
Purpose: map an MMIO resource uncached (`vm_map_phys`, `VM_CACHE_UC`),
page-rounding the range and returning the address of `res->start`
itself. Outputs: 0 for a non-MMIO, empty, or unmappable resource.
Sleeps (VMM). Unmap with `device_unmap_mmio(va)` *(exported)*, which
page-aligns `va` down.

### `struct device *device_find(struct bus_type *bus, const char *name)` *(exported)*
Purpose: referenced pointer to a device by name, or NULL; drop with
`device_put`. `device_get`/`device_put` are inline `kobject` wrappers.
Sleeps.

### `int device_for_each(struct bus_type *bus, int (*fn)(struct device *, void *), void *arg)` *(exported)*
Purpose: call `fn` for every device on `bus` (all buses when NULL) under
the lock; a nonzero return stops and is returned. `fn` must not register
or unregister anything and must not sleep long. Sleeps.

### `unsigned device_count(struct bus_type *bus)` *(exported)*, `void device_dump(void)`
Count on one bus (all when NULL); dump every bus, device, state, driver
and resource to the console.

### `struct device`, `struct device_driver`, `struct bus_type`
Layouts are in `design.md`. `struct device` embeds a `kobject` and a
mandatory `release(struct device *)` that frees the memory the device is
embedded in (`pci_device_release` frees the `struct pci_device`;
`virtio_device_release` forwards to the transport's `release`, which
frees the virtio-pci private block). `drvdata` belongs to the bound
driver and is cleared at unbind.

## DMA (`kernel/include/kernel/dma.h`, `kernel/device/dma.c`)

`dma_addr_t` is a bus address. Without an IOMMU it equals the physical
address, and callers must not rely on that.

### `void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags)` *(exported)*
Purpose: coherent, physically contiguous, page-granular memory the
device can reach. Inputs: `dev` (NULL means a 32-bit mask), `size > 0`,
`flags` `DMA_ZERO`. The zone follows the mask: below 16 MiB for masks
under 32 bits, `DMA32` for exactly 32 bits, any RAM otherwise; a block
that lands above the mask is freed and NULL returned. Outputs: the
direct-map virtual address and `*dma_out`. Ownership: caller, until
`dma_free`. Sleeps (PMM); never from interrupt context. NULL on failure.

### `void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma)` *(exported)*
Purpose: release a `dma_alloc` block; `size` must be the size passed to
`dma_alloc`. Not for interrupt context.

### `dma_addr_t dma_map(struct device *dev, const void *va, size_t len, enum dma_dir dir)` *(exported)*
Purpose: translate an existing buffer for device access. Only direct-map
addresses qualify (`kmalloc`, `dma_alloc`, page frames) and the range
must fit below the device's mask; the direct map is linear so
contiguity follows. Kernel-arena, stack, and user addresses return 0,
which callers must treat as `-EINVAL`. Any context; lock-free apart
from a statistics spinlock. `dir` is `DMA_TO_DEVICE`, `DMA_FROM_DEVICE`,
or `DMA_BIDIRECTIONAL` and is currently only recorded.

### `void dma_unmap(struct device *, dma_addr_t, size_t, enum dma_dir)` *(exported)*
No-op today; the place an IOMMU tears a mapping down. Any context.

### `void dma_sync_for_device(...)`, `void dma_sync_for_cpu(...)` *(exported)*
Purpose: ordering points around device access: a compiler barrier plus
`arch_dma_barrier()` (`sfence` on x86-64, `dsb sy` on AArch64; both
targets' caches are coherent for DMA). Any context. Drivers call the
first before ringing a doorbell and the second before reading data the
device wrote.

### `int dma_set_mask(struct device *dev, unsigned bits)` *(exported)*
Purpose: declare how many address bits the device drives (24 to 64).
`-EINVAL` otherwise or for a NULL device. No lock; set before the first
allocation.

### `void dma_get_stats(struct dma_stats *out)`
`allocs`, `frees`, `maps`, `map_failures`, `bytes_allocated`
(outstanding). Any context.

## Block layer (`kernel/include/kernel/blk.h`, `kernel/block/blk.c`)

### `void blk_init(void)`
Registry mutex. Once, from `kernel_main` after `pci_init`.

### `int blk_register(struct blkdev *bd, const char *prefix)` *(exported)*
Purpose: publish a caller-owned `struct blkdev`. Inputs: `ops->submit`,
`sector_size` (power of two, at least 512), `capacity` (sectors, > 0),
`max_sectors` (per bio, > 0), optional `dev`, `read_only`, `priv`;
`prefix` such as `"vd"` gets the first free letter appended (`vda`).
Outputs: 0, `-EINVAL` for bad geometry or a long prefix, `-ENOSPC` past
`z`. Ownership: the caller keeps the object until `blk_unregister`.
Sleeps. Logs one line the boot test keys on.

### `void blk_unregister(struct blkdev *bd)` *(exported)*
Purpose: remove from the registry. The driver must have drained or
failed every outstanding bio first (virtio_blk resets the device and
completes them with `-EIO`). Sleeps.

### `int blk_submit(struct bio *bio)` *(exported)*
Purpose: validate and hand a request to the driver. Inputs: `dev`,
`done` non-NULL; for `BIO_READ`/`BIO_WRITE`: `nsectors` in
`[1, max_sectors]`, `sector + nsectors <= capacity`, `buf` DMA-able
(`dma_map` must succeed); `BIO_FLUSH` needs `sector == nsectors == 0`.
Outputs: 0 (the driver owns the bio until it calls `bio_complete`, and
`done` runs exactly once), `-EINVAL`, `-EROFS` for a write to a
read-only device, or the driver's own error (`-EAGAIN` when its queue is
full), in which case `done` never runs. Thread context. `status` reads
`-EAGAIN` while in flight.

### `void bio_complete(struct bio *bio, int status)` *(exported)*
Driver side: record `status`, bump the device's `reads`/`writes`/
`flushes`/`errors`, run `done`. Any context, typically an MSI handler.

### `int blk_read(struct blkdev *, uint64_t sector, uint32_t nsectors, void *buf)`, `blk_write(...)`, `blk_flush(...)` *(exported)*
Purpose: synchronous helpers on a stack `completion`, splitting into
`max_sectors` pieces. `-EINVAL` for zero sectors or a NULL buffer before
anything is submitted; otherwise the first failing piece's error.
Sleep; thread context only.

### `struct blkdev *blk_find(const char *name)` *(exported)*
Referenced pointer or NULL; drop with `blkdev_put`. Sleeps.
`blk_count()` and `blk_dump()` report the registry.

### `struct bio`
`dev`, `sector`, `nsectors`, `dir`, `buf`, `done`, `arg`, `status`,
`link` and `drvpriv` for the driver. The submitter owns it; nothing in
the layer allocates bios.

## Entropy (`kernel/include/kernel/random.h`, `kernel/core/random.c`)

A SHA-512 hash pool under a spinlock; any context, never blocks, never
fails. Not a certified DRBG (see `design.md`).

### `void random_init(void)`
Mixes the clock and a stack address in (uncredited). After `timer_init`.

### `void random_add_entropy(const void *buf, size_t len, unsigned bits)` *(exported)*
Mix `len` bytes in and credit `bits` (total capped at 512). `len == 0`
is ignored.

### `void random_get_bytes(void *buf, size_t len)` *(exported)*, `uint64_t random_u64(void)` *(exported)*
Fill `buf` from `SHA512(state || counter)` blocks, then advance the
state so earlier output cannot be recovered from a later state capture.

### `unsigned random_entropy_bits(void)` *(exported)*, `uint64_t random_source_bytes(void)`
Credited bits (0 to 512) and bytes received from hardware sources.

## Message-signalled interrupts (`kernel/include/kernel/irq.h`, `kernel/include/arch/irqc.h`)

### `int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu, struct irq_msi_msg *msg)`
Purpose: allocate a vector from the dynamic range (48 to 238 on x86),
register `fn` on it, and fill `msg->addr`/`msg->data` with what the
device must write to raise it on `cpu`. Outputs: the vector (>= 0),
`-EINVAL` (NULL handler or message, unknown CPU), `-ENOSPC` (no
vector). Takes the IRQ spinlock; no allocation; not for interrupt
context by convention. The handler runs like any other vector handler
(interrupts off on the target CPU, EOI by the trap path).

### `int irq_release_msi(int vector)`
Unregister the handler and free the vector. The caller must have
masked the device first (PCI does in `pci_msix_release`).

### `int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data)`
Architecture side: x86 fills `0xFEE00000 | apic_id << 12` and
`data = vector` (physical destination, fixed delivery, edge).
`-EINVAL` for a CPU without an APIC id below 256 or a vector outside the
dynamic range.

## Legacy PCI configuration access (`kernel/include/arch/pci.h`)

`bool arch_pci_legacy_available(void)`, `uint32_t arch_pci_legacy_read(bus, slot, func, off, width)`,
`void arch_pci_legacy_write(bus, slot, func, off, width, v)`: mechanism
#1 over ports `0xCF8`/`0xCFC` (`kernel/arch/x86_64/pci_legacy.c`), the
first 256 bytes of each function, under a spinlock. Used by the PCI core
only when ACPI has no MCFG. Any context.

## Console sinks (`kernel/include/kernel/console.h`)

### `void console_register(struct console_sink *sink)` *(exported)*
Prepend a sink; `name` and `write` set; `write` must be non-blocking
beyond polling its device and is called with interrupts off under the
console spinlock, and lock-free in panic mode.

### `void console_unregister(struct console_sink *sink)` *(exported)*
Unlink a sink (module unload). The list walk in `console_write` is not
serialised against this call; see invariant D12's gap.

### `bool console_has_sink(const char *name)`
True if a sink of that name is registered. Diagnostics and self-tests.

## Boot-time order (`kernel/core/main.c`)

`module_init` → `device_init` → `pci_init` → `blk_init` → `random_init`
→ `arch_irq_enable` → `smp_init` → `module_load_boot` (which loads
`virtio`, `virtio_blk`, `virtio_rng`, `virtio_console`) → self-tests →
`init`.

## QEMU and harness knobs (`scripts/qemu-run.sh`, `tests/boot/run_boot_test.py`)

| Knob | Default | Effect |
|---|---|---|
| `QEMU_TESTDISK` | `<image dir>/testdisk.img`, created as 8 MiB of zeros if missing | Backing file of the `virtio-blk-pci` scratch disk (`vda`). The boot test creates a fresh `boot-test.log.testdisk.img` every run |
| `QEMU_VCON` | `<image dir>/vcon.log`, truncated on start | File the `virtconsole` port writes to. The boot test uses `boot-test.log.vcon` and requires the `boot complete` line in it |
| `QEMU_NET_HOSTFWD`, `QEMU_FWCFG_NETTEST`, `QEMU_PCAP` | empty | Phase 8 network knobs (port forwards, the fw_cfg harness parameter, a pcap of the NIC); see `docs/kernel-services/network/testing.md` |

`qemu-run.sh` always attaches, in this order after the AHCI boot disk:
`virtio-blk-pci` (scratch disk), `virtio-rng-pci`, `virtio-serial-pci`
with one `virtconsole`, and since Phase 8 `virtio-net-pci` on a
user-mode `netdev` (MAC `52:54:00:c0:5f:05`), which replaces QEMU's
default e1000e. Under QEMU q35 these appear as `pci:00:02.0` to
`pci:00:05.0` (vendor `1af4`, transitional ids `1001`, `1005`, `1003`,
`1000`).
