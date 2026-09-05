# NVMe driver: invariants

**M1. A command slot is owned by exactly one command from `slot_get` to
`slot_put`, and a completion for a free slot changes nothing.** The free
list is a linked list through `cmds[]` under the queue lock; a completion
whose id names a slot without a bio or waiter is logged and ignored.
Check: `nvme` self-test (32 reads from four CPUs, every completion
delivered once; a burst of segment I/O). Gap: no hostile-controller
test with forged completions.

**M2. Every segment mapped for a command is unmapped when the command
completes, is aborted, or the controller dies.** `unmap_cmd` runs under
the queue lock before `slot_put` on all three paths. Check: `nvme`
self-test (`dma_stats.maps - unmaps` unchanged across the test's I/O).

**M3. The queue lock is never held across `bio_complete`, `complete` or
an admin wait.** Completions are collected under the lock and delivered
after it; admin commands wait on a completion outside every spinlock
(under the admin mutex). Check: the lock-order checker on every boot
(`nvme-queue` and `nvme-admin` are leaves below `devices`/`modules`);
review.

**M4. A request is submitted on the queue of the CPU that submits it,
and that queue's vector is routed to that CPU.** `queue_for_this_cpu`
picks `ioq[cpu % nr_ioq]`; `pci_msix_request(..., cpu)` binds vector
`q` to CPU `q - 1`. Check: `nvme` self-test (with one queue per CPU, at
least 90 % of completions land on the issuing CPU; QEMU delivers 32 of
32). Gap: a thread migrating between the pick and the doorbell is
allowed and untested.

**M5. `timeout` never dereferences its argument before finding it in a
slot under the queue lock.** The bio may complete on another CPU at any
moment; only the slot table says whether it is still the driver's.
Check: review (the block layer's contract, `docs/kernel/device/api.md`,
`ops->timeout`). Gap: QEMU's controller never times out; the abort and
reset paths are exercised by review only.

**M6. A dead controller refuses every submission and completes
everything it held.** `controller_die` sets `dead` first (later
`nvme_submit` calls return `-EIO`), disables the controller, and completes
every slot's bio with `-ETIMEDOUT` and every waiter with `-EIO`. Check:
review; the path runs at `nvme_remove` (module unload) where it is the
normal shutdown. Gap: not driven under load.

**M7. Bring-up follows the specification's order and every step is
bounded.** Disable before programming AQA/ASQ/ACQ; `CC.EN` then `RDY`
within `CAP.TO`; admin commands within 5 s; `CFS` is fatal. A failure at
any step leaves the controller disabled and frees everything allocated.
Check: every boot on both machines (the log line with queue count and
depth); review of the failure labels in `nvme_probe`.

## Gaps (documented, not invariants)

- One controller is tested (QEMU's); `mqes`, `dstrd`, `mdts` and the
  namespace formats of real hardware are handled by the code paths but
  not seen.
- No SGL, no metadata, no namespace management, no re-initialisation.
