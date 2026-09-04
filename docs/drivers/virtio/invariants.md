# VirtIO: invariants

**V1. A device is driven only after `VIRTIO_F_VERSION_1` was offered
and FEATURES_OK was confirmed by re-reading.** Legacy devices and
feature rejection end in status FAILED and `-ENOTSUP`; no queue is set
up. Check: every boot logs the negotiated features for three devices.
Gap: no test presents a legacy-only or feature-rejecting device.

**V2. Nothing the device writes is used unchecked.** Used ids below the
queue size and backed by a cookie, chain walks length-guarded, config
reads bounded by the DEVICE window and generation-stable, NOTIFY writes
inside the NOTIFY window, queue vectors confirmed by read-back. Check:
review of `virtq_pop`, `vpci_read_config`, `vpci_notify`,
`vpci_setup_queue`. Gap: no fault injection of a hostile used ring.

**V3. Every bus address in a descriptor came from `dma_alloc` or
`dma_map`.** Rings and per-request headers are `dma_alloc`ed; bio data
is `dma_map`ed by `blk_submit` and again by virtio_blk. Check: review;
`blk` self-test rejects a stack buffer before it reaches the driver.
Gap: `virtq_add` cannot verify the addresses it is given.

**V4. `virtq_add` never publishes a partial chain, and `avail->idx` is
bumped after a release fence.** Free-list bookkeeping and the ring write
happen under the queue spinlock. Check: every boot (block I/O
completes correctly under `blk`). Gap: no host test drives the ring
logic without a device (planned `test_virtq`).

**V5. One MSI-X vector per interrupt-driven queue, entry 0 for
configuration changes; a polled queue has `VIRTIO_MSI_NO_VECTOR`.**
Check: boot log (`virtio-cfg`, `virtio-vq` vectors; console queues
"polled"). Gap: none.

**V6. The console sink never sleeps and never spins longer than 200 ms
per chunk; on timeout it goes dead instead of wedging the console.**
Its queues are polled so `virtq_pop` in the sink cannot race a
callback. Check: every boot (the whole log reaches the console file).
Gap: the dead state is not exercised.

**V7. A probe that fails leaves no queue, no DMA memory, and a reset
device; `remove` resets before freeing so the device cannot DMA into
freed memory.** Check: review of each driver's `fail` path and `remove`.
Gap: no probe-failure injection.

**V8. virtio_blk completes every bio it accepted exactly once:
normally from the completion callback, on `remove` with `-EIO`; a bio
it could not queue is returned to the block layer without completion.**
Check: `blk` self-test statistics. Gap: no in-flight unload test.

**V9. Drivers accept only the features they list; indirect descriptors,
event index and multiport console are never negotiated.** Check: review
of each `virtio_device_init` call. Gap: none.

**V10. The VirtIO stack knows nothing about the hypervisor** (invariant
9 of the constitution): it programs a PCI device and rings; no
hypercall, no CPUID probing. Check: review. Gap: none.
