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
non-zero.

**And writing this report found that the code comment above it is
wrong**, which is worth more than the test. A CPU publishes at interrupt
return only when it is outside every read-side section
(`kernel/arch/x86_64/trap.c:91`, and the AArch64 twin):

```c
        if (pc->irq_depth == 0 && pc->preempt_count == 0 && (frame->rflags & RFLAGS_IF)) {
            quiesce_note_quiescent();
```

So a reschedule IPI sent to a CPU spinning with preemption disabled is
taken, handled, and returns **without publishing** — `preempt_count` is
not zero, which is the whole reason that CPU is a straggler. The kick
cannot help the case its comment names first:

```c
            /* A straggler is in a preempt-disabled region across its
             * ticks, or its tick keeps landing inside one: an extra
             * interrupt gives its return path another chance. */
```

The first clause is false and the second is true. The lifetime report
says so two paragraphs above the bullet this unit is closing — "**the
straggler IPI helps a halted CPU, not one spinning with preemption
off**" (`:175-178`) — and the comment in the code never caught up. A
halted CPU has `preempt_count == 0` and publishes on the extra
interrupt; a CPU whose periodic tick keeps landing inside a short
preempt-disabled region gets a differently-timed interrupt that lands
outside one. Neither is a spinner.

That changes what a test can honestly claim, and the first draft of this
report claimed the wrong thing: it proposed a preempt-disabled spinner
and an assertion that the kick completed the period. It would have
reported positive evidence for a kick that did nothing. The design below
is the corrected one, and the comment is a defect this unit fixes.

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

### Q6 — the straggler, and what may honestly be claimed about it

This is the one window with no hook, because there is nothing to hold
open: the waiter's own loop is the mechanism. It is also the one whose
first design was wrong, so the claims are stated narrowly and each says
which side it is about.

**The waiter's side**, which is assertable and deterministic. A thread
on another CPU disables preemption and spins past two ticks while the
test CPU runs a grace period. Then:

- `straggler_ipis` rises by at least one *within the test's own bracket*
  — this says the waiter noticed a straggler and kicked, which is a fact
  about the waiter;
- the kicks stop at eight, which is the bound in the code;
- the grace period completes once the spinner exits, and well inside the
  warning bound.

**What the test must not claim** is that the kick completed the period,
because it cannot have: the spinner's return path finds `preempt_count`
non-zero and does not publish. The test asserts the opposite instead —
that the period was *still waiting* while the kicks were being sent —
which is the true statement and the one that would break if someone
"fixed" publication to ignore `preempt_count`.

**The system's side**, which is the claim the lifetime report actually
makes (risk 2: a long preempt-disabled section stalls the waiter, not
the system). While one CPU spins and one waits, a third does ordinary
work — a counter it increments, a timer that fires — and the test
asserts that work completed. That is the property a user of this
subsystem depends on, and nothing tests it today.

**And the positive half, which this unit cannot honestly supply.** The
obvious third test — a grace period with another CPU idle — proves
nothing: an idle CPU publishes at the top of its idle loop and again
after each periodic tick, so the period completes in about a tick,
*before* the two-tick threshold that sends the first kick. Zero kicks,
and the assertion "completed without reaching the bound" is satisfied
just as well by a kernel with the kick path deleted.

Follow that through and the question changes. To be kicked at all a CPU
must be pending past two ticks, and the populations that can be are:

- **a CPU spinning with preemption disabled** — which the kick cannot
  help, because its return path finds `preempt_count` non-zero;
- **a CPU whose periodic tick keeps landing inside a short
  preempt-disabled region** — the comment's second clause, which the
  kick genuinely can help, because an IPI arrives at a different phase
  and its return may land outside the region;
- **a halted CPU whose tick is not arriving at all**, which this kernel
  does not produce: the tick is periodic on every online CPU.

So the only population the kick demonstrably helps is the second, and
**arranging it is a phase coincidence, not a construction**: a test
would have to make a CPU's tick collide with a short disabled region
repeatedly while an IPI at an unrelated phase misses it. That is a
probability, and this report's own rule is that a test which cannot be
made deterministic is not shipped.

**So this unit does not demonstrate that the kick causes progress, and
says so rather than asserting something weaker and calling it
evidence.** What it does instead is bound the claim from both sides —
the waiter kicks, the spinner is not helped, the system stays live — and
leave behind a named question: *what is the straggler kick worth?* On
the evidence assembled here it helps one population that no test can
arrange, and the comment above it describes a population it cannot help.
That belongs in the inventory as a row, with this report as its
evidence, rather than being dressed up as a passing test.

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

**Two hooks, because the argument has two halves and one hook only
reaches one of them.**

The *refusal* half is reached by pausing `blk_unregister` between the
`gone` store and the `submitting` load: submitters arriving in that
interval must all be refused.

The *drain* half is not, and the first draft of this report thought it
was. Every submitter that enters during that pause sees `gone` and turns
back before the driver, so a bio is inside the driver only if it got
there before the pause began — which is scheduling, not arrangement, and
the test could pass having never occupied the state it claims to verify.
The drain half needs a **submit-side** hold: a hook that stops a
submitter *inside the driver*, after it has raised `submitting`, and a
handshake that lets the test start the unregister only once a submitter
is known to be there. Then `blk_unregister` must not return until that
submitter leaves, and the test asserts the order of those two events
rather than their timing.

### N-L3 — the pcb and its timers

The oracle here has to be aimed carefully, and the first draft aimed it
at the wrong mechanism. `timer_kick` — the body of all four callbacks —
takes a reference immediately (`tcp.c:337-343`):

```c
static void timer_kick(struct tcp_pcb *pcb, unsigned flag)
{
    __atomic_fetch_or(&pcb->work_flags, flag, __ATOMIC_RELAXED);
    pcb_get(pcb);
```

So a hook placed *after* `pcb_get` tests reference counting, not
synchronous cancellation: the pcb survives because the callback holds a
reference, and `timer_cancel_sync` could be a no-op without the test
noticing. A hook placed *before* it, with only an entry-time magic
check, tests nothing either — the free can land during the hold, after
the check has passed.

So: the hook goes **before the acquisition**, and the callback checks
the pcb's magic **after the held interval as well as on entry**. The
interval is therefore one in which the pcb is protected by nothing but
`timer_cancel_sync` refusing to return, and the second check is the
statement that it did refuse. The pcb is poisoned on free
(`docs/audit/rare-crash-detector`'s lesson, applied before the crash
rather than after), so the failure names itself instead of being a
silent read.

All four timers, because the bug this is aimed at is not "cancel is
wrong" — that is tested — but "three of the four were cancelled".

### The virtio device

The smallest of the four and the only one needing an interface: the
device model can register and can remove on driver unregistration, and
nothing can say "remove *this* device now".

**The hook must be the whole transition, not the driver's hook.**
Calling `remove` alone leaves the device still bound: the driver's bound
count, `driver`, `drvdata` and the bound state all stay as they were,
and since `vpci_remove` frees the object `drvdata` points at, the device
is left bound with a dangling pointer for a later real unregister to
remove a second time. The first draft proposed exactly that. So
`pci_test_remove` performs the same unbind the ordinary path performs —
the driver's `remove`, then the bookkeeping — and the test asserts the
device is unbound afterwards, not merely that `remove` ran.

If that interface turns out to want more than a test hook — a real
hot-unplug path with a userland interface — **this report says in
advance that it stops at the hook and names the rest.** A device-removal
interface is its own unit and would be three designs in one to take it
here.

### What this unit does when it finds nothing

Says so, and keeps the tests. Tests that fail the day someone reorders a
store are the product either way; a unit whose value depended on finding
a bug would be a unit with an incentive to report one.

It has already found two things without running, which is worth stating
because both came from writing the oracles rather than from executing
them: `quiesce.c`'s straggler comment names the case the kick cannot
help, and the kick's value is unproven for want of a population a test
can arrange. Neither is a crash. Both are the kind of thing that only
turns up when somebody tries to write down what a mechanism guarantees
and finds they cannot.

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

And this unit **adds** a row rather than only striking one: what the
straggler kick is worth. It fires only for a CPU pending past two ticks,
it cannot help the spinner that its own comment names, and the one
population it can help is a tick-phase collision no test can arrange.
Deleting it, bounding it, or proving it are three different units and
none of them is this one.

## Affected files

| file | change |
| --- | --- |
| `kernel/core/quiescetest.c` | the straggler test; the pcb-and-timers test |
| `kernel/core/quiesce.c` | the straggler comment corrected: its first clause names the case the kick cannot help, which writing this report found |
| `kernel/block/blk.c` | a `CONFIG_DEBUG` hook pausing `blk_unregister` inside the window |
| `kernel/device/devtest.c` | the submit-against-unregister test |
| `kernel-services/network/tcp.c` | a hook making a rexmit callback slow; the poison on free |
| `drivers/pci/pci.c` | `pci_test_remove`, `CONFIG_DEBUG` only: the **whole unbind transition**, not the driver's hook alone |
| `drivers/virtio/virtio_pci.c` | nothing expected — the hook drives the existing remove |
| `userland/init/init.c` | the two lines the stale inventory row is actually worth: `/dev/fsctl` and `/dev/net/tapctl` refused to uid 1000 |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| docs | `docs/kernel/quiesce/testing.md`, README Status, `docs/README.md`, the inventory's §4 bullet and the stale §3 row |

## New APIs

```c
/* kernel/include/kernel/blk.h, CONFIG_DEBUG only */
/* The refusal half: pause inside blk_unregister, between the `gone`
 * store and the `submitting` load. */
void blk_test_unregister_pause(unsigned ms);
/* The drain half, which the pause above cannot reach: hold a submitter
 * inside the driver after it has raised `submitting`, and tell the test
 * when one is there, so the unregister races an occupied window rather
 * than an empty one. */
void blk_test_hold_in_driver(bool on);
bool blk_test_submitter_parked(void);
void blk_test_release_in_driver(void);

/* kernel/include/kernel/pci.h, CONFIG_DEBUG only */
/* Unbind a live device the way the ordinary path does: the driver's
 * remove *and* the bookkeeping after it. Running the hook alone leaves
 * the device bound with a dangling drvdata, because vpci_remove frees
 * what drvdata points at. */
int pci_test_remove(struct pci_device *dev);
```

No kernel-facing API changes outside the debug build.

## Migration plan

1. **Q6 and the comment it corrects.** The three straggler tests need no
   hook — a spinner, an idle CPU and the existing counter — and the same
   step fixes `quiesce.c`'s comment, whose first clause names the case
   the kick cannot help. First because it needs no subsystem touched, so
   a failure is unambiguously the subsystem's.
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
| `quiesce-straggler` | the **waiter's** side: a CPU spinning with preemption disabled past two ticks is kicked -- `straggler_ipis` rises by at least one in this test's own bracket, the kicks stop at eight, and the period is **still waiting** while they are sent, because a preempt-disabled CPU cannot publish at interrupt return | remove the kick loop: the delta is zero and the test fails on the delta. And the converse bug-proof, for the claim this test refuses to make: make the trap publish without checking `preempt_count` and the still-waiting assertion fails -- that assertion is what stops the test crediting the kick with the completion |
| `quiesce-straggler-system` | the claim the lifetime report actually makes (risk 2): a long preempt-disabled section stalls **the waiter, not the system** -- a third CPU's ordinary work completes while one spins and one waits | have the waiter hold something the third CPU needs: its work stops too, and the test names it |
| `quiesce-straggler-idle` | that an idle CPU needs **no** kick: a grace period with another CPU idle completes with `straggler_ipis` unchanged, in about a tick. This is deliberately not the positive case for the kick -- an idle CPU publishes before the two-tick threshold, so a test asserting the kick helped it would pass with the kick path deleted, which is how the first draft of this row was wrong | make the idle publish conditional on something an idle CPU does not satisfy: the delta becomes non-zero and the period lengthens, which is the assertion that this case never reaches the kick at all |
| `blk-submit-unregister` | with submitters on other CPUs and the window held open: **no bio reaches the driver after `blk_unregister` returns**, every bio completes exactly once with 0 or `-ENODEV`, no submitter hangs, and the refcount returns | make the `gone` store `relaxed`: a submitter that missed it raises `submitting` after the unregister read it, and a bio reaches a driver whose device is gone |
| `blk-unregister-drain` | the drain half, **arranged rather than hoped for**: a submit-side hook stops a submitter inside the driver after it has raised `submitting`, the test starts the unregister only once that submitter is known to be there, and asserts `blk_unregister` returned *after* the submitter left -- an order of two events, not a timing | skip the `submitting` spin: the unregister returns while the submitter is inside and the order assertion names which came first. Without the submit-side hook this test passes on a machine where no submitter ever reached the driver, which is why it has one |
| `tcp-pcb-timer-free` | a pcb closed while a callback is inside it, **held before the callback takes its reference**: the magic is live on entry *and* after the held interval, so that interval is one in which nothing but `timer_cancel_sync` protects the pcb | cancel three of the four timers: the fourth fires into poisoned memory and the check names which timer. And the aim-check: move the hook after `pcb_get` and the test still passes with `timer_cancel_sync` stubbed out -- which is the test measuring reference counting instead, so the hook's position is itself asserted |
| `virtio-remove-inflight` | a virtio-blk device removed with I/O in flight through the **whole unbind transition**: every outstanding bio completes with an error rather than being forgotten, nothing touches the device after remove returns, and the device is left *unbound* -- no `driver`, no `drvdata`, the bound count down | remove without draining: a completion arrives for a device already freed. And: call the driver's hook alone, as the first draft proposed, and the device is left bound with a dangling `drvdata` for a later unregister to remove a second time |
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
  mitigation is step 1 of the design: three of the four windows are held
  open by a hook rather than by timing, so the race is arranged and not
  hoped for. Q6 is the exception, which is why its claims are stated as
  facts about the waiter and about the system rather than about the
  kick. Any test that cannot be made deterministic is not shipped; it is
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

The design landed as the *reviewed* version of it, which is not the
version this report was first written with: three of its four oracles
measured the wrong thing and the review caught all three before any of
them existed. What follows is what the build added to that.

**The straggler section produced the unit's first finding without
running.** `quiesce.c`'s comment named a preempt-disabled spinner as the
case the kick helps; the trap's publish is gated on `preempt_count == 0`,
so that is precisely the case it cannot help. The comment is corrected
and says which population it can help and which it cannot. And the kick's
value is left as an open question rather than dressed up: it fires only
past two ticks, cannot help the spinner, and the one population it can
help is a tick-phase collision no deterministic test can arrange.

**`timer_cancel_sync` gained a spin counter, for the same reason
`blk_unregister` did.** Both already counted *that* they waited, after
the fact; a test that must release a parked callback needs to know a
cancel is waiting *while* it waits, or the release is a timer and the
race is a hope. Both counters are `CONFIG_DEBUG` only.

**Three CPUs, each doing what the other two cannot.** The pcb test parks
a callback on one CPU, where it spins; closes from another, which cancels
under `pcb->lock` with interrupts disabled and then spins; and releases
from a third. Putting the releaser beside the callback deadlocked the
machine on the first run, and the reason is written where the CPUs are
chosen rather than left for the next person.

**The virtio window stops lower than the report expected, and says so.**
`pci_test_remove` performs the whole unbind, which is what the review
asked for, and `device-remove-busy` asserts it: the hook runs once, the
driver is unbound, `drvdata` is cleared, the bound count falls, and a
second unbind is a no-op. What it does **not** do is remove the
machine's live virtio-blk with real I/O outstanding — that is the
scratch disk the filesystem tests run on, and a boot-time suite that
destroys it destroys the run. A virtio device dedicated to removal is a
change to CI's machine, not a step of this unit. The report committed in
advance to stopping and naming the rest; this is that, one level lower
than it anticipated.

**Two mistakes of mine, recorded because they cost a run each.** A
second test registered the synthetic bus a second time and panicked the
machine, each test holding its own `registered` flag — now one
`ensure_fake_bus()`. And an edit that silently failed to apply had me
re-test an unchanged image and read the previous failure as though it
were new.

### As run

**319 self-tests.** Six windows asked a question nothing had asked
before; each reports the number that makes its answer checkable rather
than a pass.

| window | what ran, and what it said |
| --- | --- |
| Q6, the waiter | **5 kicks over a 32 ms wait, and the spinner was not helped by any of them.** The first time `straggler_ipis` has been read since it was written |
| Q6, the system | **5749 units of ordinary work** on a third CPU while one stalled the waiter — the lifetime report's risk 2, asserted |
| Q6, idle | a grace period over idle CPUs took **7277 us and sent no kick**: this case never reaches the threshold, which is why it is not the kick's positive test |
| Q11, refusal | **15 accepted and 349784 refused across the window**, every accepted bio completed exactly once, and nothing reached the driver after the unregister returned |
| Q11, drain | **the unregister spun 6118 times** for a submitter parked inside the driver, and returned after it left — an order of two events, released only once the spin counter proved the drain was draining |
| N-L3 | **a cancel spun 96 times** for a callback holding the pcb, which stayed live through the interval — measured before the callback takes its reference, so it is synchronous cancellation being measured and not reference counting |
| removal | the driver's hook and the model's bookkeeping together, with **3 units of work outstanding**; a second unbind is a no-op |

**What the unit found, and it found it twice before running anything.**
`quiesce.c`'s straggler comment named the one population the kick
provably cannot help, and the kick's value for the population it *can*
help is unproven because no deterministic test can arrange it. Both are
recorded — the comment fixed, the question filed — and neither is a
crash. They are the kind of thing that surfaces only when someone tries
to write down what a mechanism guarantees and finds they cannot.

**What it did not find**: no use-after-free, no bio reaching a detached
driver, no hung unregister. The report said in advance that it would say
so and keep the tests, and that is what this is. Six tests that fail the
day someone reorders a store are the product either way.

**Three of the four windows needed a hook and one did not**, as the
design says: `blk_test_unregister_pause`, `blk_test_hold_in_driver`,
`tcp_test_hold_callback`, `pci_test_remove`, and two spin counters —
all `CONFIG_DEBUG`, all compiled out of the release build, which the
release gate checks.

**On one CPU** each racing test refuses its half and says so in its log
line. The pcb test needs three CPUs and says that too. `SMP=1` is run by
hand and these tests are a reason to keep running it.
