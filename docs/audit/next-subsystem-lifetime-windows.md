# NEXT SUBSYSTEM — four windows nothing has ever raced

Date: 2026-09-16. Tree: `main` at 5667e5b (after PR #152, the orphan
record). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §4.

**Subsystem: a deterministic adversary for each of the four lifetime
windows the quiescence subsystem argues are safe and no test has ever
put a second CPU on the other side of.**

This report closes the inventory's §4 bullet that reads, in full:

> **never exercised by a test**: the straggler IPI (Q6), the
> `blk_submit`/`blk_unregister` window (Q11), the TCP
> timer-callback/free race (N-L3), runtime hot-unplug of virtio devices.

It is the first unit in six that is not cosmofs, and the reason is in
the inventory's own ordering rule: a correctness gap reachable today
comes before a feature set for later. These four are reachable today,
and what stands behind each of them is a paragraph.

**A note on what this report did not choose.** §3 carries a row saying
"Nothing in the tree can attempt an unprivileged open […] `/dev/fsctl`
checks `cred_privileged` on both open and write and neither check is
fired by a test; the same is true of every other 0600 device". That row
is **stale**, and this report corrects it rather than building on it:
`init --unpriv-test` (`userland/init/init.c:2826`) drops to uid
1000 and is refused by `mount`, `umount`, a mount namespace, a uts
namespace, `sethostname`, `kill` of root's process, `klog`, a reserved
port, a 0600 device (`/dev/vmm`), a 0700 directory, a 0644 file and the
sticky bit on `/tmp`, with the permitted cases asserted beside them.
What is actually missing is two lines: `/dev/fsctl` and
`/dev/net/tapctl` were added *after* that suite and nobody added them to
it. That is a test to write, not a subsystem, and this unit writes it in
passing.

## Problem

**The tree's weakest evidence is concentrated in exactly one place.**
The lifetime and quiescence report states its ordering argument as a
table of claims — W1, W2, Q1, Q2, the interrupt table, the block-device
Dekker pair, `module_owner_of`, `timer_cancel_sync` — and then says how
each was checked (`2026-09-lifetime-quiesce-report.md:75-95`):

> The host model (`tests/host/test_quiesce.c`) runs the same inline code
> with four reader threads and 2000 reclaim generations under ASan and
> UBSan […] No TSan run and no formal litmus check were made.

and, in its risks:

> **Ordering verified by review and sanitizers, not by a model checker.**

Review and a host model are good evidence for the *algorithm*. They are
no evidence at all for the four places where the algorithm meets a
driver, a socket or a device, because in those places nothing has ever
run the other side.

**What the kernel suite does test** is the primitives, and it tests them
well: `quiesce-grace`, `quiesce-call`, `irq-sync`, `timer-cancel-sync`
and `quiesce-stress` (`kernel/core/quiescetest.c`). Every one of them
drives the mechanism from one side and asserts the mechanism's own
postcondition. None of them puts a *concurrent adversary* on the other
side of a window and then asks whether the thing being protected
survived.

The four windows, each with what is claimed and what runs today:

### Q6 — the straggler IPI

`synchronize_quiesce` waits for every online CPU to publish. A CPU
spinning in a preempt-disabled region across its ticks will not, so
after two ticks the waiter sends it a reschedule IPI to give its return
path another chance (`kernel/core/quiesce.c:91-101`), up to eight times,
counting each round in `g_stats.straggler_ipis`.

**`straggler_ipis` is incremented in one place and read in none.** It is
declared in `kernel/include/kernel/quiesce.h:77`, exported through
`quiesce_get_stats`, and no test in the tree asserts it has ever been
non-zero. The kick path — the one that exists precisely for the case the
ordinary path cannot handle — has never run in a test.

### Q11 — `blk_submit` against `blk_unregister`

`blk_unregister` (`kernel/block/blk.c:254-280`) sets `gone` and then
spins until `submitting` falls to zero, both `seq_cst`, and the comment
states the Dekker argument:

```c
    /* Refuse new submissions, then wait for the ones inside the driver.
     * Both sides are sequentially consistent: a submitter that did not
     * see `gone` has raised `submitting` before we read it, or we saw
     * its increment (docs/kernel/quiesce/design.md, "Block devices"). */
```

`blk_unregister` is called by two tests (`kernel/device/devtest.c:601`,
`:633`) and in both the device is quiescent: register, use, unregister.
**No test has ever submitted to a device while it was being
unregistered**, which is the only circumstance the barrier exists for.

### N-L3 — a TCP timer's callback against the pcb's free

`tcp_pcb_free` cancels four timers in a row
(`kernel-services/network/tcp.c:409-412`) and each `timer_cancel_sync`
must return only once the callback is not running, or the callback
dereferences a pcb the caller is about to free.

`timer_cancel_sync` itself **is** tested, on a probe object with a
deliberate 20 ms callback (`quiescetest.c:341-408`), which is real
evidence for the primitive. What is not tested is the integration: four
timers on one object, a callback that touches the object, and a free
immediately after. The primitive being right does not make four uses of
it right, and a pcb is freed on paths a test can drive.

### Runtime hot-unplug of a virtio device

`vpci_remove` (`drivers/virtio/virtio_pci.c:379`) is the driver's remove
hook and, as the quiesce report says, **only module unload drives it**.
A device removed while a request is in flight is the case the hook's own
comment is about, and nothing reaches it.

## Why it matters

**Because the last two units of this shape each found a real defect on
their first run.** The lockup unit built a per-CPU sampler to chase a
latent spin and found not one hang but two. The snapshot-deadlist unit
put a snapshot in the crash suite for the first time and found, at
prefix 125, a hazard the report had explicitly argued could not exist.
In both cases the code had been reviewed, the argument had been written
down, and the argument was wrong in a way only a second thread could
show.

These four windows have the same profile: a written argument, no
adversary, and a failure mode — use-after-free, a bio reaching a
detached driver, a hung unregister — that is silent until it is not.

**And the cost of the gap is asymmetric.** If the adversaries find
nothing, the unit's product is four tests that will fail the day someone
changes the ordering, which is the thing the tree has no protection
against today. If they find something, it is a use-after-free in the
subsystem whose entire purpose is preventing use-after-free.

## Design

### One shape, four times

Each window gets a test that follows the same three steps, and the
middle one is where the house rule applies: **a proof's adversary is
built from the mechanism, not from a stopwatch.** A `sleep` on the
racing thread is not an adversary; it is a hope.

1. **Hold the window open by construction.** Each mechanism has a point
   where it must wait, and a test hook makes that wait long enough to
   aim at — `blk_unregister`'s spin, the timer callback's body, the
   preempt-disabled region.
2. **Drive the other side from a real second CPU**, refusing to run when
   the machine has one (`other_cpu()`, as `timer-cancel-sync` already
   does — a property about two CPUs needs a test with two CPUs).
3. **Assert the protected object, not the mechanism.** The question is
   never "did the barrier run" but "did anything touch the object after
   it was freed", which means poisoning and checking rather than
   counting.

### Q6 — the straggler

A thread pinned to another CPU disables preemption and spins for more
than two ticks; the test CPU runs `synchronize_quiesce`. The claim is
that the grace period *completes* — not that it is fast — and that the
kick path is what completed it.

- `quiesce_get_stats` before and after: `straggler_ipis` must rise.
- The period must end within the warning bound, not the panic bound.
- The spinner must be joined and the machine must still be healthy.

**Vacuity, named in advance**: `straggler_ipis` rising is only evidence
if the test made it rise, so the test brackets its own measurement and
asserts the delta rather than the total, and asserts a *lower* bound of
one rather than "non-zero at some point".

### Q11 — submit against unregister

A fake block device whose driver counts what it is given. N threads on
other CPUs call `blk_submit` in a loop; the test CPU calls
`blk_unregister`. Then:

- **no bio reaches the driver after `blk_unregister` returns** — the
  driver's counter is read after the join and must equal the count it
  had when unregister returned;
- **every submitted bio completes exactly once**, with 0 or `-ENODEV`
  and nothing else;
- **no submitter hangs**, which is the other half of the Dekker
  argument and the half a one-sided test would miss;
- the device's refcount returns to where it started.

A test hook widens the window: `blk_unregister` pauses between the
`gone` store and the `submitting` load, which is precisely the interval
the argument is about.

### N-L3 — the pcb and its timers

A pcb whose rexmit callback is made slow by a hook, closed while that
callback is inside the object. The pcb's memory is **poisoned on free**
and the callback verifies its magic on entry, so a callback that runs
after the free is a loud failure rather than a silent read of freed
memory (`docs/audit/rare-crash-detector`'s lesson, applied before the
crash rather than after).

All four timers, because the bug this is aimed at is not "cancel is
wrong" — that is tested — but "three of the four were cancelled".

### The virtio device

The smallest of the four and the only one needing an interface: the
device model can register and can remove on driver unregistration, and
nothing can say "remove *this* device now". The unit adds a test-only
`pci_test_remove(dev)` that runs the bound driver's `remove` on a live
device, and drives it with I/O in flight against a virtio-blk.

If that interface turns out to want more than a test hook — a real
hot-unplug path with a userland interface — **this report says in
advance that it stops at the hook and names the rest.** A device-removal
interface is its own unit and would be three designs in one to take it
here.

### What this unit does when it finds nothing

Says so, and keeps the tests. Four tests that fail the day someone
reorders a store are the product either way; a unit whose value depended
on finding a bug would be a unit with an incentive to report one.

### The §70 gate

*Ownership and lifetime.* The tests own what they allocate and free it
on every path. The adversary threads are joined before the test returns
— the suite's thread-count check (`thread_count()` either side, as
`timer-cancel-sync` already does) is what makes that an assertion rather
than an intention.

*Concurrency.* This unit is nothing but concurrency. Each test refuses
to run its racing half on a single-CPU machine and says so in its log
line rather than passing silently, because a test that quietly does
nothing on the machine CI runs is worse than no test.

*Memory.* One fake device, one pcb, a handful of threads. The poison
pattern is the page-poisoning idea applied to one object.

*Error handling.* A hook that cannot be installed fails the test rather
than skipping it.

*Security.* No new interface outside `#if CONFIG_DEBUG`;
`pci_test_remove` is a test hook and is compiled out of a release build,
which the release build gate checks.

*Performance.* Four tests, each bounded by a small multiple of a tick.
The straggler test is the long one at roughly three ticks (12-24 ms) and
the budget accommodates it.

*Observability.* `quiesce_get_stats` already reports `straggler_ipis`;
this unit is the first reader. The block test reports how many bios were
refused versus accepted, so the log line says what the race actually
produced rather than that it passed.

*Future extensibility.* The §4 bullet has one more entry after these
four — "ordering verified by review and sanitizers only: no TSan model,
no litmus tests" — and this unit does not close it. Litmus tests are a
different discipline and Apple clang has no TSan for this target; the
row stays, narrowed to what it still covers.

## Affected files

| file | change |
| --- | --- |
| `kernel/core/quiescetest.c` | the straggler test; the pcb-and-timers test |
| `kernel/core/quiesce.c` | nothing expected — the test reads the existing counter |
| `kernel/block/blk.c` | a `CONFIG_DEBUG` hook pausing `blk_unregister` inside the window |
| `kernel/device/devtest.c` | the submit-against-unregister test |
| `kernel-services/network/tcp.c` | a hook making a rexmit callback slow; the poison on free |
| `drivers/pci/pci.c` | `pci_test_remove`, `CONFIG_DEBUG` only |
| `drivers/virtio/virtio_pci.c` | nothing expected — the hook drives the existing remove |
| `userland/init/init.c` | the two lines the stale inventory row is actually worth: `/dev/fsctl` and `/dev/net/tapctl` refused to uid 1000 |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| docs | `docs/kernel/quiesce/testing.md`, README Status, `docs/README.md`, the inventory's §4 bullet and the stale §3 row |

## New APIs

```c
/* kernel/include/kernel/blk.h, CONFIG_DEBUG only */
/* Pause inside blk_unregister, between the `gone` store and the
 * `submitting` load: the interval the Dekker argument is about. */
void blk_test_unregister_pause(unsigned ms);

/* kernel/include/kernel/pci.h, CONFIG_DEBUG only */
/* Run the bound driver's remove on a live device. */
int pci_test_remove(struct pci_device *dev);
```

No kernel-facing API changes outside the debug build.

## Migration plan

1. **Q6**, which needs no hook at all — only a spinner and the existing
   counter. First because it is the one that can be written without
   touching a subsystem, so a failure is unambiguously the subsystem's.
2. **Q11**, the hook and the test. The most likely of the four to find
   something, because it is the only one whose argument is an
   ordering argument between two specific stores.
3. **N-L3**, the poison and the four-timer close.
4. **The virtio removal**, the hook and the test, which stops at the
   hook.
5. **The two unprivileged lines**, and the inventory's §3 row corrected
   to what is actually left of it.
6. Docs, README Status, the §4 bullet struck through, as-built.

Each step boots both architectures at the default CPU count and at
`SMP=1` to prove the refusal path; steps 1-4 run `make BUILD=release` to
prove the hooks compile out.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `quiesce-straggler` | a CPU spinning with preemption disabled across two ticks is kicked and the grace period completes: `straggler_ipis` rises by at least one *in this test's own bracket*, and the wait ends well under the warning bound | remove the kick loop: the period waits for the spinner to finish on its own, the delta is zero, and the test fails on the delta rather than on a timeout |
| `blk-submit-unregister` | with submitters on other CPUs and the window held open: **no bio reaches the driver after `blk_unregister` returns**, every bio completes exactly once with 0 or `-ENODEV`, no submitter hangs, and the refcount returns | make the `gone` store `relaxed`: a submitter that missed it raises `submitting` after the unregister read it, and a bio reaches a driver whose device is gone |
| `blk-unregister-drain` | the second half of the same argument from the other side: a submitter that *did* raise `submitting` is waited for, so `blk_unregister` does not return while a bio is inside the driver | skip the `submitting` spin: the unregister returns early and the driver counter moves after it |
| `tcp-pcb-timer-free` | a pcb closed while its rexmit callback is inside the object: the callback sees a live magic every time, and the free happens after it returns | cancel three of the four timers: the fourth fires into poisoned memory and the magic check names which timer it was |
| `virtio-remove-inflight` | a virtio-blk device removed with I/O in flight: every outstanding bio completes with an error rather than being forgotten, and nothing touches the device after remove returns | remove without draining: a completion arrives for a device already freed |
| `unpriv-test` (existing, extended) | uid 1000 is refused `/dev/fsctl` and `/dev/net/tapctl`, the two 0600 devices added after that suite was written | invert either check: the open succeeds and the test says which door opened |

**Vacuity, named in advance, because this unit is unusually exposed to
it.** Three of these tests can pass by not racing: the adversary thread
might finish before the window opens, or never start. Each therefore
asserts a *positive* fact that only a real race produces — the straggler
counter's delta, the count of bios refused with `-ENODEV` being non-zero
*and* non-total, the callback's entry count — and fails if that fact is
absent, rather than passing because nothing went wrong. A test that
cannot tell "the race did not happen" from "the race was handled" is not
evidence about the race.

**And on a one-CPU machine** each refuses its racing half and says so in
its log line, rather than passing quietly. `SMP=1` is run by hand
(`docs/development.md`, the CPU-count matrix), and this unit's tests are
a reason to keep running it.

## Benchmarks

- **The straggler test's wall clock**, which is a lower bound on how
  long a grace period takes when a CPU is uncooperative: the number the
  quiesce report's "4-8 ms floor" does not cover.
- **The block race's throughput**: how many bios were accepted and how
  many refused in the window, which says whether the window was actually
  hit rather than stepped over.
- **The suite's total**, since four concurrency tests with real waits
  are the kind of addition the per-test budget notices — and did notice,
  one unit ago.

## Risks

- **The tests may be flaky, and flaky is worse than absent.** A race
  test that fails once in fifty teaches the tree to ignore it. The
  mitigation is step 1 of the design: every window is held open by a
  hook rather than by timing, so the race is arranged and not hoped for.
  Any test that cannot be made deterministic is not shipped; it is
  reported as a finding with what it showed.
- **A hook that changes the thing it measures.** Pausing inside
  `blk_unregister` changes the interleaving it is meant to expose. The
  answer is that the hook widens the window rather than moving it: the
  pause goes between the two operations the argument is about, so what
  is tested is the ordering, not the timing.
- **The virtio removal may not stop at a hook.** If running `remove` on
  a live device turns out to need reference counting the device model
  does not have, that is a unit and not a step of this one; the report
  commits in advance to stopping and naming it.
- **This unit may find a defect it cannot fix in scope.** A broken
  ordering in `blk_unregister` is a small fix; a broken one in the
  epoch algorithm is not. The report commits to the fsck unit's
  precedent: measure it, name it, fix what is in scope and file the rest
  as a row with its evidence.

## Alternatives considered

- **A TSan or litmus model instead.** The right tool and unavailable:
  Apple clang ships no TSan runtime for this target, which is why the
  quiesce report listed it as a gap rather than doing it. A model also
  checks the algorithm, which is the part already checked; these four
  windows are where the algorithm meets a driver.
- **Extend `quiesce-stress` rather than write four tests.** It already
  runs readers against reclaimers and would hide these: one pass/fail
  line over four different claims, which is the shape the per-test
  budget and the fsctl unit both argued against. Four windows, four
  answers.
- **Fuzz the orderings.** No mechanism in the tree can perturb
  interleavings without a hypervisor or a scheduler hook, and building
  one to test four known windows is the larger answer to the smaller
  question. The hooks are that mechanism, aimed.
- **Do nothing: the code has been reviewed.** It has, and so had the
  release loop the deadlist unit's crash suite caught at prefix 125, and
  so had the two hangs the lockup unit found. Review is what produces
  the argument; it is not what tests it.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
