# NEXT SUBSYSTEM — a virtio device dedicated to removal

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**,
and the banner below records where the build differed from the design.

**What the build changed:**

1. **The seams reach the driver through a table, because the driver is
   a module.** The design said "one debug hook in `virtio_blk.c`" and
   the test would call it; the link said otherwise — the kernel image
   cannot name a module's symbols. `virtio_blk` publishes a
   `struct blk_test_driver_hooks` to the block layer at its module init
   (`blk_test_driver_hooks_set`), the test asks for it by driver name,
   and a build where the module did not publish skips rather than
   fails to link.
2. **The disk is found by capacity, not by name.** The design said
   `blk_find("vdb")`. On `virt` the boot image is itself a virtio-blk,
   so the removal disk is **`vdc`** there and `vdb` on q35. It is 4 MiB
   where every other disk in the machine is 8, so the test looks for
   the only 4 MiB virtio disk and the two machines need no special
   case. The harness's marker allows either name.
3. **The unheld pass finds nothing, and that is the measurement.** The
   report expected the held pass to occupy the window and the unheld
   one to be a regression guard; as run, the unheld pass finds **0**
   requests in flight at the remove, every run, on both architectures.
   That is the number that says the hook is necessary, so it is printed
   and kept rather than treated as a boring second pass.
4. **`-ENODEV` completions were not in the design's first draft and are
   in the build**, with the sharper count beside them: review found
   that a driver's `-EAGAIN` is queued by `blk_submit` rather than
   returned, so a full table produces accepted bios that
   `blk_unregister` completes `-ENODEV`. The assertion is three
   statuses, and `-EIO` **equal** to what the remove found.
5. **The boundary stamp is inside `vblk_remove`**, also from review: a
   stamp the caller takes after `pci_test_remove` returns can be beaten
   by a completion on another CPU that draws its number first.
6. **The bug-proofs did not land where the design aimed them.** Two
   mutations kill the *machine* before the test can assert anything (a
   page fault in the block layer), one was killed by an observable the
   design did not name (the re-probe, not a late interrupt), one
   survived exactly as predicted, and one was added because of the
   first two. See "Bug-proofs, as run".
7. **The unit found a defect, which is what it was for, and it came
   from review rather than from the test.** `vblk_remove` read and
   cleared the driver's slot table with no lock while the completion
   path takes `vb->lock` for the same table — and, the deeper half, it
   did so *before releasing the queue's interrupt*, then freed the ring
   and the DMA pool a handler would be walking. `blk_unregister` keeps
   submissions out and a device reset stops the device; neither has
   anything to say about a handler already inside `vblk_done`.
8. **The first fix was wrong, and the review that found the defect
   found that too.** I built a `gone`/`in_done` barrier in the driver
   on the belief — stated in the code, the invariant, this report and
   the README — that *this kernel has no `synchronize_irq`*. It has
   one: `kernel/include/kernel/interrupt.h` declares it, documents that
   a handler is a quiesce read-side section, and `pci_msix_release`
   already masks the entry and calls it through `irq_release_msi`.
   NVMe releases its vectors before freeing its queues and says so in a
   comment; xHCI calls `synchronize_irq` by hand; AHCI disables its
   interrupt before tearing its ports down. **virtio-blk was the only
   one of the four doing it in the wrong order**, and the fix is that
   order, not a new mechanism: `virtq_free` (which releases the vector)
   moves ahead of the slot walk, and the hand-rolled counter is gone.
   A counter could not have been right anyway — review's next point —
   because it can only see handlers that have already entered, while
   the mask is what stops one that has not.
9. **The third pass is a read-side section, not a parked handler.** It
   holds `quiesce_read_lock` from a thread across the removal's
   teardown and asserts three stamps from one sequence: the removal
   enters the teardown, the section ends, the walk begins. An earlier
   version parked a real completion walk inside `vblk_done`, which
   meant spinning in interrupt context on cpu0 — where every MSI-X
   vector lands — for as long as the removal took: that CPU stops
   answering TLB shootdowns (one-second deadline) and stops ticking for
   the lockup detectors, and `lockup-hard` duly failed beside it. A
   preemption-disabled section is the same thing to
   `synchronize_quiesce` and none of those things to the machine. It
   also took the park hook back out of the completion path.
10. **A latent flake was repaired on the way**: six `thread_count() ==
   before` checks in `lockuptest.c`, asserted the instant a join
   returns, when the count falls at the reaper. This branch's thread
   churn exposed one on AArch64; the repair is a bounded wait, made
   here because it blocked the gate, and recorded in
   `docs/testing/flakes.md`.

**The lifetime-windows unit stopped one level lower than it planned, and
said so.** Its report (`docs/audit/next-subsystem-lifetime-windows.md`,
"Runtime hot-unplug of a virtio device", line 148) named four windows
nothing had ever raced; three got their adversary. The fourth —
`vpci_remove` with real I/O outstanding — was narrowed to
`device-remove-busy`, which drives the unbind *transition* on a fake
device, because the only virtio-blk in the test machine is `vda`, the
scratch disk every filesystem test runs on: "a boot-time suite that
removes it destroys the run" (`kernel/device/devtest.c:623-640`). The
report committed to stopping and naming the rest, and the inventory
carries it as the one clause of that row still open
(`docs/audit/2026-09-deferred-work-inventory.md:344`, "**Still open**:
removing a *live virtio* device with real I/O outstanding, which needs a
virtio device dedicated to removal in the test machine").

**Takes up** that clause, and with it the first item in
`docs/drivers/virtio/testing.md`'s gaps ("unload with requests in
flight"). This is a §6 first-class pick: a correctness gap reachable
today, in section 4, with a deterministic test the tree can build once
the machine has the device.

## What is established

**`vpci_remove` is real and nothing but module unload has ever run
it** (`drivers/virtio/virtio_pci.c:379`): unregister the virtio device
(which runs the virtio driver's `remove`), status 0, `pci_msix_disable`,
unmap the BARs, drop the creator's reference. The driver side
(`drivers/virtio/virtio_blk.c:333`, `vblk_remove`): `blk_unregister`
first — "no submit is inside the driver after this" — then a device
reset, then every slot still in flight completed with `-EIO`, then the
slot tables freed at `vblk_release` when the last holder of the block
device is gone. Each step has a comment saying why it is in that order.
No test has ever run the order with a bio at the device.

**The block layer's half is proved, on a fake device.**
`blk-submit-unregister` and `blk-unregister-drain`
(`kernel/device/devtest.c:801`) race `blk_submit` against
`blk_unregister` from a second CPU with the window held open by a
debug hook (`blk_test_hold_in_driver`, `kernel/block/blk.c`), and
assert the claim rather than the mechanism: some accepted, some
refused, each accepted bio completed exactly once, nothing reached the
driver after the unregister returned (invariant Q11,
`docs/kernel/quiesce/invariants.md:95`). The hooks live in `blk_submit`
itself, so they work for any block device, including a real one.

**The device-model half is proved, on a fake device.**
`device-remove-busy` shows that removal is the whole unbind and not the
driver's hook alone — `driver`, `drvdata`, the bound count and the state
all go together — because a driver that frees what `drvdata` names (the
virtio one does) would otherwise leave a bound device holding a dangling
pointer (`kernel/device/device.c:183-193`, `device_test_unbind`).
`pci_test_remove` (`drivers/pci/pci.c:196`, debug builds) does that for
a live PCI device and has no caller.

**The test machine has one virtio-blk, and it is spoken for.**
`scripts/qemu-run.sh:217` attaches `virtio-blk-pci` over `QEMU_TESTDISK`
first, on both machines, "so it is vda for the storage self-tests"; the
`cosmofs-*` self-tests format it and `init --selftest` mounts it
(`docs/kernel/device/testing.md:27-50`). Four virtio functions sit at
`00:02.0`–`00:05.0` under q35 and the documentation says so
(`docs/kernel/device/api.md:360-366`). A block device already knows its
hardware: `struct blkdev::dev` (`kernel/include/kernel/blk.h:108`), which
virtio-blk sets to its virtio device (`virtio_blk.c:294`), whose `hw` is
the PCI function (`drivers/include/drivers/virtio.h:74`).

**The tools the test needs exist.** A second CPU for the other side
(`other_cpu_for_blk`, and the rule that a property about two CPUs needs
a test with two CPUs); the frame poisoner that turned a 1-in-30 crash
into a deterministic failure (the rare-crash unit); the sequence
`blk_test_tick` draws stamps from so "after the unregister returned" is
an order and not two clocks (`blk.c:340-360` — today it stamps only the
unregister's return and the parked submitter's departure, and it is
`static`; this unit makes it callable so the test's own completion
callback can stamp each completion); and the harness's per-run fresh
disk images, one `dd` line each.

**And one fact about a full slot table that the assertions must
respect.** A driver that refuses a bio with `-EAGAIN` does not hand it
back to the submitter: `blk_submit` queues it on the device's `pending`
list and every completion resubmits from the head (`blk.c`, "The pending
queue"), and `blk_unregister` completes whatever is still pending with
`-ENODEV` (`blk.c:430-437`). So in exactly the state step 3 builds — the
driver's table full — some *accepted* bios will complete `-ENODEV`, from
the block layer, not the driver.

## The problem

### The argument has never met the device

Every step of `vblk_remove` is justified in a comment and none has been
exercised with a request at the device. The failure modes are the ones
the lifetime report called silent until they are not: a completion
interrupt arriving after the slot tables are freed (a use-after-free in
the interrupt path); a bio the block layer accepted reaching a driver
that has reset its device (a request that never completes, so its
waiter hangs); a reset that lands while the device is mid-DMA into a
buffer the driver then frees. Three of the four windows found nothing
wrong; the fifth adversary the lifetime unit built (the lockup sampler)
found two hangs on its first run. The prior on this one is not that the
code is wrong — it is that nobody knows.

### The reason it was skipped is a machine, not a mechanism

The narrowed test exists because removing `vda` destroys the run, and
adding a device to CI's machine "is a change to CI's machine and not a
step of this unit". That is a scope decision, correctly taken then. It
is one `-drive` and one `-device` line, on both machines, and it is this
unit's whole reason to exist.

## Design

### The machine: a second virtio-blk, last in line

`scripts/qemu-run.sh` attaches a second `virtio-blk-pci` over
`QEMU_RMDISK` (default `<image dir>/rmdisk.img`, **4 MiB** of zeros
created when missing; the harness makes `boot-test.log.rmdisk.img`
fresh per run as it does the others). Four rather than eight because
every other disk in the machine is 8 MiB, and the size is what the
test identifies this one by — see below. It is attached **after every device the
documentation numbers** — after the NIC, the NVMe, the USB and SATA
controllers — so `00:02.0`–`00:05.0` keep their meaning and no test
that counts or names PCI functions moves; and after `vda` on both
machines. That makes it **`vdb` on q35 and `vdc` on `virt`**, where the
boot image is itself a virtio-blk and takes the name first — the
design said `vdb` on both and was wrong about the `virt` machine.
`QEMU_RMDISK=0` leaves it out, and the test then skips with that
reason. The boot marker `blk: vd[bc]: 8192 sectors of 512 bytes` is
required when the disk is present. Nothing else in the tree learns of
it: `blk-bench`'s name list does not include it, the
filesystem tests find `vda` by name, and `blk_count` assertions are
relative.

### The device is reachable from its disk

The test finds the disk by **capacity**: the only 4 MiB virtio disk in
the machine, which needs no special case for the two machines' two
names (`rm_find`). It holds the reference that `blk_find` returns, and reaches the PCI function through the two hops the
model already records: `to_virtio_device(bd->dev)->hw` is the bus device
behind the transport, `to_pci_device` of that is what `pci_test_remove`
takes. No new field.

### One hook in the driver, at the completion

`blk_submit`'s hooks park a submitter *before* `ops->submit`; they say
nothing about a bio at the device. The case this unit exists for needs
the driver's own in-flight table non-empty at the moment `vblk_remove`
runs, and a real QEMU device completes a read in microseconds. So one
debug hook in `virtio_blk.c`: `vblk_test_hold_completions(bool)`, which
makes `vblk_done` (the interrupt's completion walk) return without
consuming anything while armed. The device does the I/O and signals; the
driver leaves the slots in flight by construction; the remove then finds
them. That is the honest description of what the hook does — the
requests are done at the device and unconsumed by the driver — and it is
the window the removal has to survive: slots the reset will drop and the
remove must complete exactly once. A second counter,
`vblk_test_inflight_at_remove()`, records how many `vblk_remove` found,
so the test asserts the window was occupied rather than hoping.

### The test: `virtio-remove-inflight`

Debug builds; skips on one CPU or without the removal disk. The shape is the
lifetime unit's three steps: hold the window open by construction, drive
the other side from a real second CPU, assert the protected object.

1. Find the removal disk, hold it, resolve its PCI function. Read the first sector
   once, so the device is known to work before it is removed.
2. Start a submitter on another CPU (the `blk-submit-unregister` shape):
   it submits reads of the removal disk continuously, counting accepted, refused
   (`-ENODEV`), any other errno, and completions by status. Wait until
   at least four have been accepted *and completed* — the device is
   live, not merely present.
3. Arm the completion hold. Wait until the submitter has filled the
   driver's slots: with completions held, accepted minus completed
   grows monotonically, and once it exceeds `nr_slots` the table is
   full and the excess is on the block layer's pending list. (The
   submitter never sees the driver's `-EAGAIN`: `blk_submit` queues it
   and returns 0, as "What is established" says; the count is the only
   observable, and it is enough.)
4. `pci_test_remove(pdev)` from the test thread, while the submitter
   keeps submitting. The removal stamps its own boundary (step 6).
5. Release the hold. Keep the submitter running until it has seen four
   `-ENODEV` refusals after the remove, then stop and join it.
6. Assert:
   - `vblk_test_inflight_at_remove() >= 1`: the window was occupied.
   - every accepted bio completed exactly once, each with one of three
     statuses and nothing else: `0` (done before the hold, or done and
     consumed), `-EIO` (a slot the driver held at the remove, completed
     by `vblk_remove`'s leftover walk), `-ENODEV` (queued on the block
     layer's pending list behind a full table, completed by
     `blk_unregister`). The sharper claim is the count: **the `-EIO`
     completions equal `vblk_test_inflight_at_remove()` exactly** —
     the driver completed precisely the slots it held, no more (a
     double completion) and no fewer (a stranded slot) — and the
     `-ENODEV` completions plus the submitter's `-ENODEV` refusals are
     everything the unregister turned away.
   - no completion carries a stamp later than the removal's own
     boundary stamp. The submitter's `done` callback is the test's own
     code, run at `bio_complete`; it stamps each completion with
     `blk_test_tick()` and keeps the maximum. The boundary is stamped
     **inside the removal**, not by the caller afterwards: a debug
     stamp at the end of `vblk_remove`, after its leftover walk, drawn
     from the same sequence (`vblk_test_remove_seq()`). A stamp taken
     by the test after `pci_test_remove` returns would race a callback
     on another CPU that completes after the return but draws its
     number first, and would pass the very case the check exists for.
     With the boundary inside, the atomic increment puts every
     completion and the end of the remove in one total order: a
     completion numbered after the boundary completed after the driver
     had finished removing, which is the late-interrupt use-after-free
     whether or not it happened to crash; one numbered before it is
     the leftover walk's own, or an interrupt the reset still allowed,
     and both are the removal's business. This needs `blk_test_tick`
     callable from the driver and the test (it is `static` today).
   - the disk is no longer in the registry; the virtio device is off its bus
     (`device_find(&virtio_bus, name) == NULL`); the PCI function is
     `DEV_UNBOUND` with `driver` and `drvdata` NULL and the virtio-pci
     driver's bound count one lower.
   - the test's own reference is the last: `blkdev_put` runs
     `vblk_release`, observed by a debug release counter, and the
     frame poisoner's verify-on-allocate stays silent through the rest
     of the boot.
7. **Bring it back.** `pci_test_rebind(pdev)` (new, debug: the bus's
   `try_bind` against the registered driver, for a device in
   `DEV_UNBOUND`) re-probes the function: the disk reappears with the
   same name and capacity, and a read of its first sector returns what step
   1 read. This is the assertion that the removal left the hardware
   sane — status 0, MSI-X disabled, BARs unmapped, and the device able
   to be driven again — and it means the test leaves the machine as it
   found it.

Then the same sequence **without the hold**, as a second pass: a race
with no adversary, labelled a regression and not a proof, in the shape
`cwd-ref` uses for a window it could not reach — because the held
variant proves the in-flight case and the unheld one is what a future
change to the ordering would be most likely to break.

### What the reset means, stated

`virtio_device_reset` in `vblk_remove` is what makes the leftover
completions safe to issue: after it the device has dropped every request
and will neither DMA nor signal. The test cannot see the device's side
of that under TCG any more than the mprotect unit could see cache
coherence; what it sees is the driver's side — nothing completes after
the return, nothing touches freed memory — and the report says so rather
than claiming the reset was proved.

## Affected files

| file | change |
| --- | --- |
| `scripts/qemu-run.sh` | `QEMU_RMDISK`, a second `virtio-blk-pci` attached last on both machines; `QEMU_RMDISK=0` leaves it out |
| `tests/boot/run_boot_test.py` | a fresh `boot-test.log.rmdisk.img` per run; the `vd[bc]` marker when the disk is present, and the test's own three lines wherever it can run |
| `drivers/virtio/virtio_blk.c` | `vblk_test_hold_completions`, `vblk_test_inflight_at_remove`, `vblk_test_remove_seq` (the boundary stamp, at the end of `vblk_remove`), a release counter (debug) |
| `drivers/pci/pci.c`, `drivers/include/drivers/pci.h` | `pci_test_rebind` (debug) |
| `kernel/device/device.c`, `kernel/include/kernel/device.h` | `device_test_bind` for it, beside `device_test_unbind` |
| `kernel/block/blk.c`, `kernel/include/kernel/blk.h` | `blk_test_tick` made callable (debug), so the driver's remove and the test's completion callback draw from one sequence |
| `kernel/device/devtest.c` | `selftest_virtio_remove_inflight`; the comment at 623-640 that names this unit as future work becomes a pointer to the test |
| `kernel/core/selftest.c` | the registry entry, beside `blk-unregister-drain` |
| `docs/kernel/device/{api,testing}.md` | the knob, the attachment order, the machine the tests assume, the test |
| `docs/drivers/virtio/{design,testing}.md` | the hooks; the gap struck |
| `docs/kernel/quiesce/{invariants,testing}.md` | Q11's check gains the real device |
| `docs/development.md`, `docs/kernel/arch/aarch64/testing.md` | the knob in the tables |
| `docs/audit/next-subsystem-lifetime-windows.md` | one line at the narrowed row: built by this unit |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §4 clause struck |
| `README.md` | Status entry |

## Tests

| case | what it establishes |
| --- | --- |
| `virtio-remove-inflight`, held | with `n ≥ 1` requests done at the device and unconsumed, the removal completes exactly those `n` with `-EIO` and the block layer completes the pending ones with `-ENODEV`; every accepted bio completes once; nothing completes after the removal's own boundary stamp (the completion callback's stamps against `vblk_test_remove_seq`); the disk, the virtio device and the driver binding are gone; the release runs on the last put; the poisoner is silent |
| the same, unheld | the natural race, a regression guard |
| the rebind | the disk comes back under its old name and reads the same first sector: the hardware was left sane |
| `QEMU_RMDISK=0` | the test skips with its reason; every other marker unchanged |
| the documented PCI numbering | `00:02.0`–`00:05.0` unchanged with the new function present (`selftest_pci` walks every function; the doc's count is corrected, not asserted) |

**Bug-proofs, as run** (x86-64, each applied, built, booted and
reverted; the table says what actually happened, which is not what the
design predicted in three places):

| mutation | designed expectation | as run |
| --- | --- | --- |
| `vblk_remove` without `blk_unregister` first | a bio reaches a driver whose slots are freed: the submitter hangs, or the poisoner reports | **killed** — the machine dies first: `KERNEL PANIC: page fault: kernel write at 0x624 ... from a kernel thread`, 12 s into the boot. The defect is real and immediate; the *test's* assertions never get to speak |
| `vblk_remove` skipping its leftover completions | the held bios never complete | **killed** by the test: `rm_completions(&s) == s.ok` — 64 accepted bios with no completion |
| `vblk_remove` completing a leftover slot twice | the `-EIO` count exceeds what the remove found, and `c_double` fires | **killed**, but again by the machine, not the counter: a page fault at address 0 inside the block layer with `blk-pending` held. A second `bio_complete` corrupts the list before the test can count it. So this mutation does **not** prove the counter |
| the remove reporting one more in flight than it completed | — (added because of the row above) | **killed** by `s.c_eio == found`: the equality is live, not vacuous |
| `vpci_remove` without `pci_msix_disable` | a late interrupt after the vector is torn down; may be silent | **killed**, by a different observable than predicted: no late interrupt appeared, and the **rebind** failed (`pci_test_rebind(pdev) == 0`) because the second probe cannot take vectors the first never released. The teardown is proved by the re-probe, not by an interrupt |
| `vblk_remove` without the device reset | the leftover completions are issued for requests the device still holds; may be silent under TCG | **survived**: the boot **passed**, the test passed, 64 found and 64 completed. Predicted, and it stands as predicted — the reset is kept for the rule, not for a proof this environment can give (the `mprotect` unit's PAN bracket, again) |
| a `thread_join` removed from `lockup-soft` | — (the repair this unit made; its own proof) | **killed** by `threads_settled(before)` after its one-second bound: waiting for the condition is no weaker than asserting it |
| the queue released **after** the slot walk, as it was before the fix | — (the defect review found; see below) | **killed**: the walk begins while the test's read-side section is still open — `held_until < walk` fails. That is the ordering property itself, so the proof is the defect put back |
| the leftover walk without `vb->lock` | — | **survived**, and that is the honest state: with the vector released first the walk is already exclusive, so the lock is the rule `vblk_timeout` always followed rather than the thing carrying the guarantee. It stays for the rule |

Three earlier mutations proved the *first* attempt at this fix — a
`gone`/`in_done` barrier inside the driver — and are not in the table
because the code they perturbed is gone: the drain removed was killed,
the counter miscounted hung the boot, and the lock alone survived.
They are recorded here rather than deleted silently, because the
design they belonged to was wrong for a reason worth keeping (item 8
of the banner).

**Two of the seven are killed by the kernel rather than by the test**,
and that is worth saying plainly: for those two the test is not the
thing standing guard, the block layer's own structure is. The test's
counters are guarded by the fourth row instead.

## Risks

**The machine changes for every run, including release and the guard
boots.** One more PCI function with DMA behind the IOMMU on both
machines; a few milliseconds of enumeration. `QEMU_RMDISK=0` is the
opt-out, and the marker is gated on it.

**A test that removes a device could leave the machine worse for the
tests after it.** The rebind is the answer, and it is asserted: the
test ends with the disk back and readable. If the rebind fails the test
fails, loudly, before anything else runs on a machine with a half-dead
function.

**The held variant proves a narrower thing than "I/O outstanding".** The
requests are done at the device and unconsumed by the driver, which is
the state the remove must handle; requests genuinely in the device's
queue at reset are the unheld pass's business, and that pass is a
regression guard. The report says which is which.

**Two of the bug-proofs may be silent under TCG.** Named above, with
the rule the mprotect unit set: a bracket kept for the argument is kept,
and the report records that the environment could not show it.

**`pci_test_rebind` is a second debug-only path into the device model.**
It reuses `try_bind`; it does not add a policy. A device in `DEV_FAILED`
is refused, so a probe that failed is not retried by a test into a
different state than the boot left it.

## Alternatives considered

**Hot-unplug through QEMU (`device_del` over QMP) instead of
`pci_test_remove`.** The real thing, and the one that would also test a
PCI hotplug interrupt path. Rejected for this unit: this kernel has no
PCI hotplug (`§3`, "hotplug: no CPU hotplug, no PCI rescan"), so the
kernel side would be a new subsystem before the test could run, and the
window this unit is for — the driver's remove with I/O outstanding — is
reached by `pci_test_remove` exactly as an unplug would reach it. Named
as the unit after PCI hotplug, if that is ever built.

**Remove `vda` after the filesystem tests are done.** Rejected: the
user-mode suite mounts it after the kernel self-tests, and the order in
which future tests need it is not this unit's to freeze.

**Reuse the NVMe or SATA disk instead of adding a device.** Rejected:
the window is `vpci_remove`'s, and those are other drivers' windows;
each would be its own unit, and the virtio one is the one the lifetime
report named.

**A virtio-blk with `QEMU_TESTDISK` sized larger and partitioned, one
partition for removal.** Rejected: removal is per device, not per
partition.

**Hold the window with a slow device (a throttled QEMU drive) instead of
a completion hook.** Rejected: a stopwatch adversary, and the house rule
is that a proof's adversary is built from the mechanism.

---

Named and deferred by this report: PCI hotplug (a rescan, a hotplug
interrupt, `device_del` from the harness), which would make the
removal an event the kernel receives rather than one a test issues;
**a module unload with requests in flight**, which is a different path
from a device removal — `vblk_module_shutdown` unregisters the driver
and the model unbinds every device it holds — and has no test at
either level; **a removal test for the other three block drivers** (NVMe, AHCI and
USB storage each already release their interrupt before freeing what a
handler touches — that is where this driver's fix came from — but none
of the three is *tested* under a removal with I/O outstanding), each
needing a device of its own in the machine; and
`vpci_remove` under a module unload with a mounted filesystem on
`vda`, which is a policy question (refuse, or drain) before it is a
test.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_012Ba88BLmjoLX3pw9yA4RaY
