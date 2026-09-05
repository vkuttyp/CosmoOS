# Device infrastructure: invariants

Rules the device model, DMA layer, block layer, entropy pool, and the
console sink list must keep. Each has a **Check** (what verifies it
today) and a **Gap** (what does not). Changing a rule means changing
this file and the code together.

**D1. Probe and remove run under the device-model lock, which is
recursive only for the owning thread.** `driver_register`,
`device_register`, `driver_unregister`, `device_unregister` all take
`model_lock()`; a `probe` may register children and a `remove` may
unregister them because the owner re-enters. No other thread can enter
the model while a probe runs. Check: the `device` self-test (a bus with
a synthetic driver) and every boot (virtio-pci's probe registers virtio
devices from inside the pci probe). Gap: `probe` and `remove` are never
timed; a probe that blocks forever wedges every later registration.

**D2. Lock order: `g_modules_lock` → device-model lock →
`kernel_space.lock`.** A module's `init` calls `driver_register`; a
probe calls `vm_map_phys` (`device_map_mmio`). Nothing takes them in
another order; a driver's interrupt handler takes only its own
spinlock. Check: review; the mutex panics on recursion by a different
path (`mutex_lock: recursive lock`). Gap: no lockdep.

**D3. A device is freed by its release and by nothing else, after the
last reference.** `device_register` refuses a device without a release
and takes the bus's reference; `device_unregister` drops it; the creator
drops its own after its teardown; `device_find` returns a referenced
pointer. No path frees a `struct device` while it is on a bus or held.
`vpci_remove` therefore unregisters, tears the transport down, and only
then `device_put`s; `vpci_release` frees the block when the last holder
is gone. Check: `device` self-test (`-EINVAL` without a release; refcount
`init + bus + find` = 3; 1 after unregister). Gap: no hot-unplug driver
exercises `vpci_remove` at run time (module unload of `virtio` does).

**D4. A probe failure never leaves a half-bound device.** On a nonzero
`probe` return the model records `DEV_FAILED` and `probe_error`, clears
`drvdata`, and does not set `driver`. Drivers unwind their own
allocations before returning an error (every virtio driver's `fail`
path resets the device and frees queues and DMA memory). Check: `device`
self-test (`-EIO` probe), `module-fail` style review of each driver's
failure path. Gap: no fault-injection test drives a virtio probe failure.

**D5. Every bus address comes from `dma_alloc` or `dma_map`, `dma_map`
accepts only direct-map memory below the device's mask, and every
`dma_map` has its `dma_unmap`.** Kernel-arena, stack, and user addresses
yield 0. `blk_submit` checks bio buffers with `dma_mappable` (no side
effect) and the driver maps and unmaps for real; virtio-blk unmaps at
completion, virtio-net when the device returns a buffer, NVMe at
completion, abort or reset. Devices that address 64 bits say so
(`dma_set_mask(64)`: the virtio-pci transport, NVMe), so buffers above
4 GiB are not refused (audit finding #27). Check: `dma` self-test
(kmalloc maps, arena and stack do not, the predicate agrees with the
map, `unmaps` counts, and a burst of I/O on `vda` leaves `maps − unmaps`
unchanged), `nvme` self-test (the same on the NVMe namespace), `blk`
self-test (stack buffer refused). Gap: a driver can still pass an
arbitrary integer as a bus address; the API is a discipline boundary,
not enforcement (no IOMMU).

**D6. `dma_alloc` memory is physically contiguous, page granular, and
inside the mask; `dma_free` gets the same `size`.** Zone selection by
mask; a block above the mask is freed and NULL returned. Check: `dma`
self-test and `dma_get_stats` balance (`bytes_allocated` returns to its
starting value). Gap: `dma_free` cannot detect a wrong `size`.

**D7. A bio that `blk_submit` accepted completes exactly once through
`bio_complete`, and a bio it rejected never runs `done`.** Validation
(range, count against `max_sectors`, segment rules and count against
`max_segments`, DMA-ability, read-only) happens before `ops->submit`; a
driver that returns an error from `submit` has not queued it. virtio_blk
completes every in-flight bio with `-EIO` on remove. Check: `blk`
self-test (rejections return `-EINVAL`/`-EROFS` without completion;
statistics count exactly the accepted bios), `blk-segments` (five
malformed segment lists refused). Gap: no test unloads `virtio_blk` with
requests in flight.

**D7c. A segment list is a run of whole pages between its ends.** Every
segment but the first starts on a page boundary, every segment but the
last ends on one, the lengths sum to the transfer, the count is within
`max_segments`; so a driver turns segments into descriptors or PRP
entries without bouncing. Check: `blk-segments` (three-segment write
read back flat and in two pages on the RAM device), `nvme` (a
two-segment, four-page transfer through a PRP list).

**D7d. No accepted bio waits for ever.** Every accepted bio is on its
device's in-flight list; the `blk-timeout` thread reports one older than
`timeout_ns` once and hands it to the driver's `timeout`, which completes
it (`-ETIMEDOUT`) after making the device forget it; a driver without the
operation gets the warning and the counter. The driver never
dereferences the bio before finding it in its own records. Check:
`blk-timeout` (a stalled RAM device: `blk_read` returns `-ETIMEDOUT`
after the 300 ms the test set, `timeouts` +1, the device recovers).
A driver's completion path decides ownership under its lock by pointer
before touching the bio (virtio-blk scans its slot table; NVMe its
command slots), so a request the timeout path already completed is
neither dereferenced nor completed twice (Greptile on PR #24). Gap:
virtio-blk's and NVMe's timeout paths run only by review (QEMU
answers).

**D7a. A queue-full driver never fails a caller.** `-EAGAIN` from
`ops->submit` parks the bio in `blkdev.pending`; `bio_complete` drains the
list in order with no lock held across `ops->submit` (a driver may
complete synchronously and re-enter); a refused resubmission goes back
to the head. `blk_unregister` completes what is still pending with
`-ENODEV`. Check: `blk-queue` (eight writes against two slots on the RAM
device in deferred mode: all complete, in order, six requeued).

**D7b. Bio flags are the block layer's, not the driver's.** `BIO_PREFLUSH`
and `BIO_FUA` are implemented as flush, write, flush chained by
completions; a driver only ever sees `BIO_READ`, `BIO_WRITE`, `BIO_FLUSH`.
Check: `blk-queue` (the RAM device's recorded stream shows flush, write,
flush); review of the drivers (none reads `bio->flags`).

**D8. The block layer knows no driver and no filesystem.** `blk.c`
includes no driver header; drivers see only `struct blkdev`/`struct
bio`. Check: review (the include list). Gap: none.

**D9. MSI vectors are ordinary vectors from the dynamic range, dispatched
and EOI'd by the existing trap path.** `irq_request_msi` allocates with
`arch_vector_alloc`, registers with `interrupt_register`, and only the
message composition is architecture specific. A device is masked
before its vector is released (`pci_msix_release`). Check: every boot
(three virtio devices, one config vector each plus queue vectors) and
the `irq: MSI vector N` debug lines. Gap: vectors are always targeted at
CPU 0; no balancing, no affinity test.

**D10. MMIO is mapped uncached and never user-accessible.**
`device_map_mmio` uses `VM_PROT_RW | VM_CACHE_UC` in the kernel space
only. Check: review; the `pci` self-test verifies BAR geometry, not the
mapping. Gap: no test reads back the cache attribute of a mapped BAR.

**D11. The entropy pool never blocks and never fails.**
`random_add_entropy` and `random_get_bytes` take one spinlock and hash;
callers in interrupt context (virtio_rng's completion) are the normal
case. Credited entropy is capped at 512 bits. Check: `random` self-test
(outputs differ, non-zero, credit capped, hardware bytes present when a
virtio-rng exists). Gap: no statistical test of the output; the pool is
explicitly not a certified DRBG.

**D12. A console sink never sleeps and gives up after a bounded spin.**
`console_write` runs under a spinlock with interrupts off and lock-free
in panic mode; the virtio-console sink polls its used ring for at most
200 ms per chunk and marks itself dead instead of wedging the console.
Check: every boot (the console file carries the whole log); the
`virtio-console` self-test checks the sink exists. `console_unregister`
unlinks under the console spinlock, so a writer on another CPU never
walks a sink that is being removed. Gap: panic mode (which bypasses the
lock) racing an unload is not handled; unloading a console driver during
a panic is not a supported operation.

**D13. Drivers contain no generic bus logic.** Configuration space
layout, BAR decoding, capability walking and MSI-X table programming are
in `drivers/pci/pci.c`; virtio device drivers see `struct virtio_device`
and virtqueues, never a PCI register. Check: review (no driver includes
`x86/io.h` or reads `PCI_*` offsets). Gap: none.

**D14. Boot modules that drive hardware declare `MODULE_CAP_DRIVER`.**
`virtio`, `virtio_blk`, `virtio_rng`, `virtio_console` do. Check:
review. Gap: nothing enforces the capability yet (security phase).

**D15. The PCI core and the device model are initialised before boot
modules load, and the entropy pool before drivers can feed it.**
`kernel_main`: `device_init` → `pci_init` → `blk_init` → `random_init`
precede `module_load_boot`. Check: every boot; a driver registering
before `device_init` would trip `KASSERT(g_initialized)`. Gap: none.
