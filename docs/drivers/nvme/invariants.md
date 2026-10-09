# NVMe driver: invariants

**M1. A command slot is owned by exactly one command from `slot_get` to
`slot_put`, a completion for a free slot changes nothing, and a slot the
controller may still answer is never reused.** The free list is a linked
list through `cmds[]` under the queue lock; a completion whose id names
a slot without a bio or waiter is logged and ignored. An admin command
that times out in software leaves its slot *orphaned* (out of the free
list) until the controller's late answer frees it, so that answer can
never be delivered to a newer command with the same id; a slot whose
command is being aborted is marked `aborting` and, if the command
completes meanwhile, stays reserved until the timeout path releases it,
so the Abort can never name a replacement request; an Abort that itself
gets no answer resets the controller rather than releasing the id it
names, while an Abort the controller answered with an error status is a
completed Abort (nothing names the id) and, if the request finished on
its own meanwhile, no failure at all (Greptile on PR #24). A reset frees every reserved slot, since a
disabled controller answers nothing.

**M1a. A waiter on the stack is never signalled after its frame is
gone.** The admin timeout path decides under the queue lock: if the slot
still names the waiter the command is orphaned and `-ETIMEDOUT`;
otherwise the interrupt path has detached the waiter and is about to
`complete` it, and the caller waits for that signal and reports the
real result. Check: review (Greptile on PR #24). Gap: not driven by a
test. Check: `nvme` self-test (32 reads from four CPUs, every
completion delivered once; a burst of segment I/O); review of the
orphan/aborting paths (QEMU's controller never times out). Gap: no
hostile-controller test with late or forged completions.

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

**M6. A dead controller refuses every submission.** `controller_die`
sets `dead` first, so later `nvme_submit` calls return `-EIO`. It drains
and completes slots only after `CSTS.RDY` acknowledges disable; if the
acknowledgement fails, slots, mappings, queues and their bios remain
allocated because the controller may still own them. Check: the NVMe
disable-acknowledgement self-test and old-behavior probe.

**M6a. A submission racing controller death is either refused before
ownership transfers or completed exactly once.** The submitter checks
`dead` again while holding the queue lock, after mapping and before taking
a slot. A rejected bio owns no slot or DMA mapping. The fault/interleaving
self-test forces controller death in that window on a synthetic queue; a
successful return without a callback is a failure.

**M7. Bring-up follows the specification's order and every step is
bounded.** Disable before programming AQA/ASQ/ACQ; `CC.EN` then `RDY`
within `CAP.TO`; admin commands within 5 s; `CFS` is fatal. A failure at
any step frees resources only if disable is acknowledged; if not, the
BAR and every DMA allocation potentially visible to the controller are
retained. Check: every boot on both machines and review of the failure
labels in `nvme_probe`.

**M8. The controller's DMA memory is never freed before disable is
acknowledged.** A failed `RDY`-clear wait makes the controller dead and
refuses new I/O, but leaves queue memory and active mappings allocated;
removal releases vectors and waits for handlers, then retains controller
state if the device still has not acknowledged disable. Leaking is safer
than returning memory the device may still access.

Check: `nvme-disable-ack` injects a failed disable wait during removal,
checks that the queue's SQ, CQ, PRP pages, active command, bio and data
buffer mapping remain owned, then releases the synthetic fixture after
the assertion.

## Gaps (documented, not invariants)

- One controller is tested (QEMU's); `mqes`, `dstrd`, `mdts` and the
  namespace formats of real hardware are handled by the code paths but
  not seen.
- No SGL, no metadata, no namespace management, no re-initialisation.
