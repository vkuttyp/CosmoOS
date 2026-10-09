# VirtIO: invariants

**V1. A device is driven only after `VIRTIO_F_VERSION_1` was offered
and FEATURES_OK was confirmed by re-reading.** Legacy devices and
feature rejection end in status FAILED and `-ENOTSUP`; no queue is set
up. Check: every boot logs the negotiated features for three devices.
Gap: no test presents a legacy-only or feature-rejecting device.

**V2. Nothing the device writes is used unchecked, and nothing in the
shared allocation is read to make a decision except the used ring.**
The descriptor table is write-only for the driver: the free list, every
chain link and every chain length live in driver-private arrays
(`shadow_next`, `chain_len`, `in_bytes`), so a `next` field the device
rewrites (self-loop, cycle, out of range, into a free descriptor) has no
effect on what the driver builds or reclaims. Used ids are checked
against those records (below the queue size, in flight, not a
duplicate), a reported `len` is clamped to the chain's writable bytes,
config reads are bounded by the DEVICE window and generation-stable,
NOTIFY writes stay inside the NOTIFY window, queue vectors are confirmed
by read-back. Check: `tests/host/test_virtq.c` drives the real
`virtqueue.c` with a hostile peer under ASan/UBSan; review of
`vpci_read_config`, `vpci_notify`, `vpci_setup_queue`. Bad elements are
skipped and counted (`vq->bad_used`) without stopping the drain. Gap: a
duplicate completion naming a head that has since been legitimately
reused is indistinguishable from a valid one at this layer (the split
ring carries no generation), so such a device can corrupt its own
driver's requests; containing that needs per-request generation tags in
the driver and is future work.

**V3. Every bus address in a descriptor came from `dma_alloc` or
`dma_map`.** Rings and per-request headers are `dma_alloc`ed; bio data
is `dma_map`ed by `blk_submit` and again by virtio_blk. Check: review;
`blk` self-test rejects a stack buffer before it reaches the driver.
Gap: `virtq_add` cannot verify the addresses it is given.

**V4. `virtq_add` never publishes a partial chain, and `avail->idx` is
bumped after a release fence.** Free-list bookkeeping and the ring write
happen under the queue spinlock. Check: every boot (block I/O
completes correctly under `blk`); `test_virtq` (normal, boundary, full
and empty chains, 500 rounds of out-of-order completions leave the free
list exact).

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

**V11. Teardown excludes submitters and stops completion delivery before
draining driver-owned buffers.** A reposting poll is disabled before reset;
reset stops device DMA; queue teardown synchronizes the interrupt and
disables its deferred poll before outstanding records are reclaimed.
Every accepted buffer has a driver-owned record independent of used-ring
entries, every mapping is unmapped exactly once before its buffer is freed,
and posted receive accounting ends at zero. Probe failure and queue rebuilding obey
the same rule for partially initialized resources. `virtq_free` frees ring
bookkeeping and never owns the cookies' storage. Live timeout recovery
retains queue/record storage, stops device DMA, and serializes completion
and recovery claims under the same lock; it must exclude new submission
publication during its drain. Unit 1's reproduction and
validation are recorded in `docs/audit/2026-10-08-vnet-remove-report.md`.
For the persistent entropy buffer, the post-and-kick gate is closed before
reset; a completion after that boundary cannot publish another descriptor.
Check: the `VRNG-RESET-REPOST: PASS posts_after_reset=0` line virtio-rng's
module init prints after a synthetic reset-time completion (a marker the
boot harness requires), and `tools/virtio-rng-repost-probe.py --old`, which
restores the old gate and must see `posts_after_reset=1`.
