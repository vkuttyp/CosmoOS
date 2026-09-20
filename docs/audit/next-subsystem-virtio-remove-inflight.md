# NEXT SUBSYSTEM — a virtio device dedicated to removal

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

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
into a deterministic failure (the rare-crash unit); the sequence stamps
`blk_test_tick` records so "after the unregister returned" is an order
and not two clocks (`blk.c:340-360`); and the harness's per-run fresh
disk images, one `dd` line each.

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
`QEMU_RMDISK` (default `<image dir>/rmdisk.img`, 8 MiB of zeros created
when missing; the harness makes `boot-test.log.rmdisk.img` fresh per
run as it does the others). It is attached **after every device the
documentation numbers** — after the NIC, the NVMe, the USB and SATA
controllers — so `00:02.0`–`00:05.0` keep their meaning and no test
that counts or names PCI functions moves; and after `vda` on both
machines, so it is **`vdb`** by the same ordering rule that makes the
scratch disk `vda`. `QEMU_RMDISK=0` leaves it out, and the test then
skips with that reason. The boot marker `blk: vdb: 16384 sectors of 512
bytes` is required when the disk is present. Nothing else in the tree
learns of `vdb`: `blk-bench`'s name list does not include it, the
filesystem tests find `vda` by name, and `blk_count` assertions are
relative.

### The device is reachable from its disk

The test finds the disk by name, `blk_find("vdb")`, holds the reference
that returns, and reaches the PCI function through the two hops the
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

Debug builds; skips on one CPU or without `vdb`. The shape is the
lifetime unit's three steps: hold the window open by construction, drive
the other side from a real second CPU, assert the protected object.

1. Find `vdb`, hold it, resolve its PCI function. Read the first sector
   once, so the device is known to work before it is removed.
2. Start a submitter on another CPU (the `blk-submit-unregister` shape):
   it submits reads of `vdb` continuously, counting accepted, refused
   (`-ENODEV`), any other errno, and completions by status. Wait until
   at least four have been accepted *and completed* — the device is
   live, not merely present.
3. Arm the completion hold. Wait until the submitter has filled the
   driver's slots: accepted minus completed reaches `nr_slots`, or the
   submitter starts seeing `-EAGAIN` refusals from the driver (the block
   layer queues those; either says the table is full).
4. `pci_test_remove(pdev)` from the test thread, while the submitter
   keeps submitting. Record a `blk_test_tick` immediately after it
   returns.
5. Release the hold. Keep the submitter running until it has seen four
   `-ENODEV` refusals after the remove, then stop and join it.
6. Assert:
   - `vblk_test_inflight_at_remove() >= 1`: the window was occupied.
   - every accepted bio completed exactly once, each with `0` or
     `-EIO`, none with anything else; `-ENODEV` only from refusals.
   - no completion carries a stamp later than the remove's return: the
     driver completed its leftovers *inside* `vblk_remove`, as its
     comment says, and nothing completed afterwards (a completion after
     the return is the late-interrupt use-after-free, whether or not it
     happened to crash).
   - `blk_find("vdb") == NULL`; the virtio device is off its bus
     (`device_find(&virtio_bus, name) == NULL`); the PCI function is
     `DEV_UNBOUND` with `driver` and `drvdata` NULL and the virtio-pci
     driver's bound count one lower.
   - the test's own reference is the last: `blkdev_put` runs
     `vblk_release`, observed by a debug release counter, and the
     frame poisoner's verify-on-allocate stays silent through the rest
     of the boot.
7. **Bring it back.** `pci_test_rebind(pdev)` (new, debug: the bus's
   `try_bind` against the registered driver, for a device in
   `DEV_UNBOUND`) re-probes the function: `vdb` reappears with the same
   name and capacity, and a read of its first sector returns what step
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
| `tests/boot/run_boot_test.py` | a fresh `boot-test.log.rmdisk.img` per run; the `vdb` marker when the disk is present |
| `drivers/virtio/virtio_blk.c` | `vblk_test_hold_completions`, `vblk_test_inflight_at_remove`, a release counter (debug) |
| `drivers/pci/pci.c`, `drivers/include/drivers/pci.h` | `pci_test_rebind` (debug) |
| `kernel/device/device.c`, `kernel/include/kernel/device.h` | `device_test_bind` for it, beside `device_test_unbind` |
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
| `virtio-remove-inflight`, held | with `n ≥ 1` requests done at the device and unconsumed, the removal completes each exactly once with `-EIO`, nothing completes after `pci_test_remove` returns, the disk, the virtio device and the driver binding are gone, the release runs on the last put, and the poisoner is silent |
| the same, unheld | the natural race, a regression guard |
| the rebind | `vdb` comes back and reads the same first sector: the hardware was left sane |
| `QEMU_RMDISK=0` | the test skips with its reason; every other marker unchanged |
| the documented PCI numbering | `00:02.0`–`00:05.0` unchanged with the new function present (`selftest_pci` walks every function; the doc's count is corrected, not asserted) |

**Bug-proofs**, each to fail for its stated reason, each reverted after:

- `vblk_remove` without `blk_unregister` first → a bio accepted after the
  reset reaches a driver whose slots are freed: the submitter's bio
  never completes, or the poisoner reports the touch. Which of the two
  is recorded, not predicted.
- `vblk_remove` skipping the leftover completions → the held bios never
  complete: the submitter's accepted count never meets its completed
  count, reported by the bounded wait.
- `vpci_remove` without `pci_msix_disable` → a late interrupt after the
  vector is torn down. **May be silent under TCG** if the reset alone
  stops the device from signalling; if it is, the report says so and
  the proof stands on the argument, as the mprotect unit's PAN bracket
  does.
- `vpci_remove` without the reset → the device keeps its rings; the
  leftover completions are issued for requests the device still holds.
  Same caveat: the observable is the poisoner, and it may not fire.
- the rebind without the removal having disabled MSI-X → the second
  probe's `pci_msix_enable` fails or double-allocates: `vdb` does not
  come back.

## Risks

**The machine changes for every run, including release and the guard
boots.** One more PCI function with DMA behind the IOMMU on both
machines; a few milliseconds of enumeration. `QEMU_RMDISK=0` is the
opt-out, and the marker is gated on it.

**A test that removes a device could leave the machine worse for the
tests after it.** The rebind is the answer, and it is asserted: the
test ends with `vdb` back and readable. If the rebind fails the test
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
removal an event the kernel receives rather than one a test issues; the
same test for the NVMe, AHCI and USB drivers' remove paths, each on a
device of its own; and `vpci_remove` under a module unload with a
mounted filesystem on `vda`, which is a policy question (refuse, or
drain) before it is a test.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_012Ba88BLmjoLX3pw9yA4RaY
