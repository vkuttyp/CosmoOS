# e1000e driver: invariants

**E1. A frame reaches the stack exactly as the wire carried it, or not
at all.** `RCTL.SECRC` strips the CRC so the descriptor's `length` is
the frame; a descriptor with any `errors` bit set, or a `length` below
an Ethernet header, is counted and freed rather than handed up. A driver
that passes a frame it knows is damaged makes the stack's checksums the
last line of defence for a problem the hardware already reported.

Check: the receive counters `rx_errors` and `rx_dropped` are separate,
and the self-test that runs the network suite over this interface
requires it to complete with the same results as over virtio-net.

**E2. A transmit is either on the ring or refused, never held.** A chain
that needs more descriptors than are free is `-ENOBUFS` on the spot. The
stack owns queuing policy, and a driver that buffers behind a full link
hides the fullness from the layer that could do something about it.

**E3. Every mapped buffer is unmapped by the path that learns the device
is done with it, and by no other.** Receive buffers are unmapped in the
handler when `DD` is seen; transmit segments when `DD` is seen on their
descriptor; both when the ring is torn down or reset. There is no
reference to a buffer from anywhere the device cannot see, so the
`dma_stats` outstanding count returns to what it was after unregister.

Check: the module-unload path is exercised by the two-interface
self-test, which requires the DMA map count after the driver is gone to
equal the count before it was loaded.

**E4. A transmit that never completes is noticed, said, and recovered
from — and the recovery is bounded.** The watchdog declares a hang when
descriptors are outstanding and `TDH` has not moved in five seconds,
logs the counts, drops the outstanding frames as `tx_dropped`, and
re-initialises the ring. A device that keeps hanging keeps being reset
and keeps saying so; it is never left with a full ring and a silent
link.

**E5. The interrupt handler does not trust the cause bits.** It reads
`ICR` to clear it and services both rings every time. The legacy and
queue-mapped cause schemes differ, and QEMU's model raises one or the
other; a handler that branched on them would work on one and be silent
on the other. Servicing an empty ring costs a register read.

**E6. This driver adds nothing to the kernel's interface.** No new
`netif_ops` member, no new capability bit, no new system call, no
`sysctl` name. That was the hypothesis of `docs/audit/next-subsystem.md`
— that `netif` was not virtio-shaped — and it is stated here so that a
future change which does need one is recognised as the finding it would
be.
