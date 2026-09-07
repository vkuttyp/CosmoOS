# AHCI driver: invariants

**A1. A port's disk DMAs through the controller and has no device
identity of its own.** The blkdev's `dev` is the controller's PCI
function; every `dma_map` and `dma_alloc` is keyed by it; there is no
`struct device` per port. An IOMMU fault provoked through the disk is
attributed to the controller's requester id.

Check: `ahci-identify` (the blkdev's `dev` is on the PCI bus and in a
domain when there is a unit); `iommu` (a fault through `ahci0p1` is
counted under the controller's requester).

**A2. A command is in a slot from `PxCI` to completion, and completes
exactly once.** A slot is allocated under the port's lock, its segments
mapped before it is written, and freed — segments unmapped, `active`
bit cleared — by exactly one of: the handler seeing its `PxCI` bit clear,
the error recovery, the timeout, a detach, a reset. Each of those frees
the slot before completing the bio, so no second path can find it.

Check: `ahci-io` (the layer's counters advance by exactly the operations
issued; DMA maps equal unmaps); `ahci-timeout` and `ahci-unplug` (a
command taken back by the timeout or a detach completes once, with the
stated error).

**A3. `submit` refuses rather than waits.** Every slot taken is
`-EAGAIN`, at once, and the block layer's pending queue holds the order;
nothing in `submit` sleeps or spins on the device (it runs from
`bio_complete` too).

Check: by construction; the racing readers in `ahci-timeout` submit into
a port under restart and are served in order afterwards (the pending
queue's lost-wakeup fix from the USB unit applies here as well).

**A4. Nothing waits for a disk that is not coming back.** A detach stops
the port, completes every command it held with `-ENODEV`, and
unregisters the blkdev before dropping the reference; a timeout stops
the port and fails every command it held (the victim `-ETIMEDOUT`, the
rest `-EIO`), because a port that stopped answering does not get its
queue replayed on the guess that one command was the problem.

Check: `ahci-unplug` (a command in flight at the detach completes
`-ENODEV`, not never); `ahci-timeout` (`-ETIMEDOUT` within the layer's
bound; reads and writes work after the restart).

**A5. Recovery restarts the port; it never guesses about the device's
state.** After a task-file error the port is stopped, `PxSERR` cleared,
a COMRESET issued if `BSY` or `DRQ` stay set, the port started; the
command that was executing (`PxCMD.CCS` at the error) fails `-EIO` and
the ones the HBA had not issued are written to `PxCI` again. A COMRESET
re-identifies the disk and keeps the blkdev only if the same serial,
capacity and sector size answer.

Check: `ahci-reset` (a command in flight at the reset completes `-EIO`;
the same blkdev serves afterwards); the task-file recovery has no
reproducer in QEMU and is reviewed against §6.2.2.1.

**A6. What the driver does not understand, it refuses with one line and
leaves alone.** An ATAPI signature, a port multiplier, an unknown
signature, a SATA controller whose programming interface is not AHCI, a
controller with neither MSI-X nor MSI, a geometry outside 512–4096
bytes a sector: each is one log line and no blkdev, never a partial
driver for it.

Check: the `QEMU_SATA=cd` shape (an ATAPI device on port 1 is named in
the log and no `ahci0p1` exists); the others by review.
