# NVMe driver: design

Milestone 9 of `docs/audit/2026-09-post-roadmap-audit.md` §19 (Prompt #2
§26; audit §10.4 "NVMe readiness"). The driver is the module `nvme`
(`drivers/nvme/nvme.c`, `nvme.h`), bound through the `pci` bus to class
`01:08` programming interface `02` (NVM Express), and registers one
block device per active namespace with the block layer. It reaches the
kernel only through exported symbols, like every driver module.

## Controller bring-up (NVMe 1.4 §7.6.1)

1. `pci_enable_device(pdev, true)`, `dma_set_mask(64)` (every NVMe
   controller addresses 64 bits; the queues and PRP lists may live
   anywhere), map BAR0 (`pci_map_bar`), read `CAP`: `MQES` (queue depth
   − 1), `DSTRD` (doorbell stride), `TO` (ready timeout in 500 ms
   units), `CSS` must include the NVM command set, `MPSMIN` must allow
   4 KiB pages.
2. If `CSTS.RDY`, clear `CC.EN` and wait for `RDY` to fall (`TO` bound).
3. Admin queues: `AQA` (32 entries each), `ASQ`/`ACQ` from `dma_alloc`
   (zeroed). `CC` = `IOSQES` 6 (64-byte commands), `IOCQES` 4 (16-byte
   completions), `MPS` 0 (4 KiB), `CSS` NVM, `AMS` round robin, `EN`.
   Wait for `CSTS.RDY` (`TO` bound; `CSTS.CFS` is a fatal failure).
4. MSI-X: `pci_msix_enable(pdev, 1 + nr_cpus)`; vector 0 is the admin
   completion queue's, routed to CPU 0.
5. `Identify Controller` (CNS 1): model, serial, firmware for the log,
   `MDTS` (maximum transfer size as a power-of-two count of 4 KiB pages;
   0 = unlimited, capped by the driver at 128 pages), `NN` (namespaces).
6. `Set Features` Number of Queues asks for `nr_cpus` I/O pairs; the
   controller answers with what it grants; the driver uses
   `min(granted, nr_cpus)`.
7. For each I/O pair `q` (1-based): `Create I/O Completion Queue`
   (interrupt vector `q`, physically contiguous, depth
   `min(MQES + 1, 64)`), then `Create I/O Submission Queue` bound to it.
   The pair's MSI-X entry is requested on CPU `q − 1`
   (`pci_msix_request(pdev, q, nvme_irq, cq, "nvme-ioq", q - 1)`), so a
   completion is handled on the CPU that submitted it.
8. `Identify Active Namespace ID List` (CNS 2), then `Identify Namespace`
   (CNS 0) per id: `NSZE` (blocks), `FLBAS` → `LBADS` (block size, 512
   B to 4 KiB accepted; metadata per block is refused). A `struct nvme_ns`
   with an embedded `struct blkdev` is registered under the exact name
   `nvme<controller>n<nsid>` (`blk_register_named`), `sector_size` =
   block size, `max_sectors` = MDTS in sectors, `max_segments` = the PRP
   list capacity (512 entries on one page, minus the first PRP), and a
   30 s request timeout.

Every admin command is issued through the same submission path as I/O
and waited for on a completion (`struct nvme_cmd_wait`), with a 5 s bound;
a timeout during bring-up fails the probe and the controller is left
disabled.

## Queues

```c
struct nvme_queue {
    struct nvme_ctrl *ctrl;
    uint16_t qid, depth;
    struct nvme_sqe *sq;  dma_addr_t sq_dma;       /* 64-byte commands */
    struct nvme_cqe *cq;  dma_addr_t cq_dma;       /* 16-byte completions */
    uint16_t sq_tail, cq_head, phase;              /* phase: expected phase bit of the next completion */
    volatile uint32_t *sq_db, *cq_db;              /* doorbells: BAR0 + 0x1000 + (2q [+1]) * (4 << DSTRD) */
    struct nvme_cmd *cmds;                         /* per command id: bio or waiter, PRP list page */
    uint16_t free_head;                            /* free command ids, a linked list through cmds[] */
    unsigned inflight;
    spinlock_t lock;                               /* sq_tail, free list, cmds[]; never held across bio_complete */
    int vector;                                    /* MSI-X vector, -1 for none */
    unsigned cpu;
};
```

A command id is the index of a `struct nvme_cmd` slot: `{ struct bio *bio;
struct nvme_cmd_wait *wait; uint64_t *prp_list; dma_addr_t prp_dma;
uint64_t issued_ns; }`. The slots' PRP list pages come from one
`dma_alloc` per queue at creation (one 4 KiB page per slot), so
submission never allocates. Submission takes the queue lock, pops a
slot, fills the 64-byte command in the SQ ring, releases the lock and
rings the doorbell after `dma_sync_for_device`. The interrupt handler
reads completions while the phase bit matches, records the status,
frees the slot under the lock, rings the CQ head doorbell, and completes
the bio (or the waiter) *after* dropping the lock. Completion order
within a queue is the controller's; the block layer needs none.

Per-CPU ownership: `nvme_submit` uses the queue of the submitting CPU
(`arch_cpu_id() % nr_ioq`, so fewer granted queues than CPUs still
work). Two CPUs never contend on one queue's lock unless the controller
granted fewer queues than there are CPUs; the lock stays because a
thread may migrate between reading its CPU and taking the lock, and
because the interrupt handler and a submitter share the slot table.

## Data path

A bio's segments (`bio_vec`, `docs/kernel/device/design.md`) become PRPs:
`PRP1` is the first segment's bus address (any offset within a page);
`PRP2` is the second page's address when the transfer ends within two
pages, else the bus address of the slot's PRP list holding every further
page address. The block layer's segment rules (every segment but the
first starts on a page boundary, every segment but the last ends on one)
are exactly what makes each segment a run of whole page entries, so the
list is built by walking the segments in order without any bounce. Each
segment is `dma_map`ped at submission and `dma_unmap`ped at completion
(the mapping is a no-op today but the discipline is what an IOMMU port
needs; `docs/kernel/device/invariants.md` D5). `BIO_FLUSH` is the Flush
command on the namespace; `BIO_READ`/`BIO_WRITE` are Read (02h) / Write
(01h) with `SLBA` and `NLB − 1`. A full queue answers `-EAGAIN`, which the
block layer's pending list absorbs (D7a).

## Timeouts, abort and reset

The block layer's request timer (`blkdev.timeout_ns`, 30 s) calls the
driver's `timeout` operation in thread context. The driver issues an
Abort admin command for the (queue, command id); if the controller
completes the aborted command (status Command Abort Requested) the bio
finishes with `-ETIMEDOUT` through the normal path. If the abort itself
does not complete within 5 s, or the controller reports `CSTS.CFS`, the
driver *resets*: it clears `CC.EN`, waits for `RDY` to fall, completes
every in-flight command on every queue with `-ETIMEDOUT`
(the controller has forgotten them), marks the controller dead, and
every later submission answers `-EIO`. A dead controller's namespaces
stay registered until the module is removed, so a mounted filesystem
sees errors, not a vanished device. Re-initialisation after a reset is
future work; QEMU's controller does not time out, and the path is tested
through the RAM device's stall mode at the block layer and by review here.

## Removal

`nvme_remove`: `blk_unregister` every namespace (no submit is inside the
driver afterwards), disable the controller (in-flight commands are
completed `-EIO`), release the MSI-X vectors with `interrupt_unregister_sync`
semantics (`pci_msix_disable` after a grace period so a handler still
running on another CPU finishes), free the queues, `blkdev_put` the
namespaces (their memory goes when the last holder is gone,
`docs/kernel/quiesce/design.md`), unmap BAR0, free the controller.

## QEMU

`scripts/qemu-run.sh` adds `-device nvme,drive=nvme0,serial=cosmo-nvme0`
over a fresh 8 MiB raw file on both machines (q35 and virt). The boot
test requires `module: loaded nvme 1.0` and `blk: nvme0n1: 16384 sectors
of 512 bytes`. The `nvme` self-test writes and reads the namespace
through the block layer (single and multi-segment bios), checks that the
completions of I/O issued from every CPU arrive on that CPU's queue,
checks the DMA map/unmap balance, and formats and mounts a cosmofs on
it.

## What is deliberately not here

SGLs (PRPs cover the NVM command set on every controller), namespace
management, asynchronous event requests, the Get Log Page error log,
power states, controller re-initialisation after a reset, multipath,
and the Linux-style `/dev/nvme0` character device. Each is a
straightforward addition to `nvme.c`; none changes the block layer.
