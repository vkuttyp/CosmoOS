# NEXT SUBSYSTEM — a migration that can land: declared per-CPU claims, a lock order lockdep can see, and a migrator that moves one thread

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report. It takes
up the scheduler's largest open row in
`docs/audit/2026-09-deferred-work-inventory.md` §2.3 -- **no
migration** -- at the point the previous attempt left it: thread
migration was built, worked, and was removed because three of four
AArch64 boots failed with it in the tree, once with seven concurrency
tests at once, and the corruption was not identified
(`docs/audit/next-subsystem-thread-migration.md`, "As built"). That
report's closing finding is this unit's opening premise: *the tree has
per-CPU assumptions nothing declares.* This unit does not build the
balancer. It builds the three things the balancer needed and did not
have -- a rule for per-CPU claims that the kernel checks, a run-queue
lock order that lockdep can check, and a migration primitive with tests
that move threads on purpose -- and it runs the whole self-test suite
under an adversary that migrates threads at random, so that the balancer
of the following unit lands on a tree that has already survived
migration rather than one that meets it for the first time.

## What is established (before this unit)

**A thread stays on the CPU chosen at its creation, for life.**
`pick_cpu` (`kernel/scheduler/sched.c:175`) runs once from
`sched_enqueue_new` (`:223`); `schedule_internal` (`:263`) sets
`next->cpu` to its own run queue's CPU on every switch, and `sched_wake`
re-enqueues a blocked thread on `t->cpu`. Nothing else writes `t->cpu`,
and nothing in the tree holds two run-queue locks. Since the placement
half of the migration unit shipped, `pick_cpu` rotates its ties
(`g_pick_rotor`, `:173`), so a thread created on an idle machine is no
longer always born on CPU 0; the `sched-spread` test
(`kernel/scheduler/smptest.c:564`) proves it.

**What was learned when migration was tried** (the migration report's
"As built", recorded there so the next attempt starts past it):

- A pull balancer moved ready threads correctly -- CPU 0 went from 8 of
  14 threads to 6 of 14, CPU 2 did ten times the context switches -- and
  made three of four AArch64 boots fail (`smp-wake`, `lockup-soft`,
  `quiesce-straggler`, `quiesce-grace`, `irq-sync`, `timer-cancel-sync`,
  `lockdep-contention` together in one run; `lockdep-contention` alone;
  `iommu` alone). With the balancer removed, four of four passed.
- Ruled out: `sched_wake`'s unlocked `t->cpu` read (a blocked thread is
  in no ready list), `list_remove` leaving a stale node, the IOMMU's
  fault accounting, the SMMU interrupt's CPU binding.
- Found: **lockdep cannot check the order of two run-queue locks**, because
  every run queue's lock is initialised with the one literal `"runqueue"`
  and lockdep keys a class by that name pointer
  (`kernel/include/kernel/lockdep_core.h:84-87`); a second acquisition
  reads as same-class recursion until annotated with `spin_lock_nested`,
  and the annotation only says "deliberate". Invariant S24 in
  `docs/kernel/scheduler/invariants.md` is *reserved* for the rule
  (increasing CPU-id order) and says lockdep cannot enforce it.
  **`rq->current` can be in a ready list**: a thread woken between
  blocking and stopping stays queued until `sched_set_running_current`
  removes it (`sched.c:379`), so "the running thread is never a queue
  entry" is false and a migrator must refuse it by name. **And the tree
  holds per-CPU assumptions nothing declares**: the `el2` self-test
  asserts that the hypervisor backend owns EL2 "on this CPU"
  (`kernel-services/virtualization/hvtest.c:828-833`) from an unpinned
  thread, and `g_el2_ready[]` is filled lazily per CPU
  (`kernel/arch/aarch64/hv_el2.c:244-265`). That test is right today only
  because the self-tests run on thread 0 (`kernel/core/main.c:210`),
  which was born on CPU 0, where `hv_probe` readied EL2 (`hv_el2.c:388`).

**What a per-CPU claim is, in this kernel.** Two accessors answer "which
CPU am I on": `this_cpu()` (the `struct percpu`, 87 static call sites)
and `arch_cpu_id()` (the number, 102 sites), both out-of-line functions
per architecture (`kernel/arch/x86_64/percpu.c:32-45`,
`kernel/arch/aarch64/percpu.c:15-20`). The answer is stable exactly
while the thread cannot be moved: with preemption disabled, with
interrupts off, in an interrupt handler, or when the thread's affinity
names one CPU. **Preemption disabled is the migration barrier this
kernel already has**: `schedule_internal` panics if entered with
`preempt_count != 0` (`sched.c:270`), so a thread that has disabled
preemption can never become `THREAD_READY`, and a migrator that moves
only ready threads can never move it. The quiesce read side is built on
exactly that (`docs/kernel/quiesce/design.md`, "A read-side section is a
preemption-disabled region"). Nothing states the rule for the accessors
themselves, and nothing checks it.

**Measured for this report: what a checked accessor would find.** To
size the audit rather than guess at it, one x86-64 debug boot was run
with a probe in the two accessors (reverted; the script is kept for the
build): a call from thread context, preemption enabled, interrupts
enabled, in a thread whose affinity admits more than one CPU, on a
machine with more than one CPU, was reported once per call site with
its return address, and the 113 sites it named were symbolised and
read. Reads of the current thread through the per-CPU pointer
(`thread_current`, `preempt_disable`, `preempt_enable`) were excluded in
the probe itself, because a thread's identity is the same on every CPU
that runs it. The 113 divide as follows.

| kind | sites | what they are |
| --- | --- | --- |
| **the answer is the same on any CPU that runs this thread** | 50 | `process_current` (27) and `me()` in lockdep (3) read `->current`; `might_sleep`, `waitqueue_prepare`, `wait_for_completion`, `semaphore_down`, `sched_block_current`, `sched_preempt`, `thread_exit`, `syscall_dispatch`, `faultinject_should_fail`, `copy_from_user`/`copy_to_user`/`strncpy_from_user`, `file_fault` read `preempt_count`/`irq_depth` to assert they are zero, which in a preemptible context they are on every CPU; `vm_space_destroy` asserts `cur_space != space`, and the CPU a thread runs on always has the thread's own space loaded; `pick_cpu` uses its own CPU as a default. **Not defects; they need the raw form and a one-line reason each.** |
| **diagnostics and statistics** | 14 | `clock_now_ns`'s per-CPU offset (`kernel/timer/timer.c:112`, five sites through `ndelay`, `clock_since_ns`, `clock_realtime_ns`), `arch_mmu_shootdown_stats`, `timer_ticks`, `timer_pending_count`, `timer_tick_cost_ns`, `ipi_count`, `bio_complete`'s local/remote counters (two), `smp_stop_others` on the panic path (two). A stale answer is a wrong number or a bounded error, never a wrong action; the clock's is bounded by the offsets' half-width and is declared as such. |
| **a claim about *this CPU* made where a preemption can move the thread** | 17 | listed below; these are the defects |
| self-tests | 31 | `schedtest.c` (8), `lockuptest.c` (9) and `lockup_sample_all` called from them (2), `lockdeptest.c` (2), `quiescetest.c` (2), `arch_test_paranoid_entry` and `gdt_ist_top` (3), `arch_test_fpu_switch`, `fpu_bench_pair`, `placed_main`, `nettest.c:3607`, `selftest_uaccess_guard` (5). Each either declares its claim (`preempt_disable`, or `thread_create_on` with one CPU) or reads raw and says why. |
| unresolved | 1 | one return address the kernel's symbol table does not cover (a loaded module or a cold section); the build names it. |

The seventeen, with the mechanism each becomes under migration:

1. **`schedule_internal` reads its `struct percpu` with interrupts on**
   (`sched.c:265`) and takes that CPU's run-queue lock thirteen lines
   later (`:278`). A tick in between preempts the caller; a migrator
   moves it; it resumes on another CPU holding a pointer to the CPU it
   left, locks *that* run queue, reads *its* `current` as `prev`, and
   switches on the wrong CPU's state. **`sched_set_running_current`
   (`:379-391`) has the same shape**: `pc`, then the lock, then
   `pc->current` dequeued from `pc->rq`. This is the corruption the
   balancer produced, and it explains why seven concurrency tests failed
   together: every one of them switches often.
2. **`quiesce_note_quiescent` publishes for the CPU it read, from a
   context that can move** (`kernel/core/quiesce.c:60`, called from
   `schedule_internal` at `sched.c:275` with interrupts on, and from
   `sync_quiesce_counting` at `quiesce.c:165`). Read the id on CPU A,
   migrate, publish "A is quiescent" from B while a reader on A sits in
   a read-side section: the grace period ends early, and the object it
   protected is freed under that reader. `quiesce-straggler` and
   `quiesce-grace` are on the balancer's list.
3. **lockdep reads *this CPU's* held-spinlock stack for a mutex
   acquisition with preemption on** (`my_cpu()`, `kernel/core/lockdep.c:75`,
   used at `:212-213` by `lockdep_acquire_check`, and by
   `lockdep_acquired` and `lockdep_is_held`; four sites). A thread taking
   a mutex holds no spinlock on any CPU, but after a migration `lc`
   names the CPU it left, whose *current* thread may hold one: a false
   "mutex taken under a spinlock" report, or a missed real one.
   `lockdep-contention` is on the list, twice.
4. **The TLB shootdown sender excludes "me" and then flushes "here",
   preemptibly** (`kernel/arch/x86_64/mmu.c:300`, and
   `kernel/arch/aarch64/mmu.c:465` the same way; `user_shootdown_targets`
   in `kernel/memory/vmm.c:979` computes the mask at five call sites).
   Read the id on A, exclude A from the interrupt targets, migrate, flush
   B locally: A keeps the stale translation. `vmm.c`'s design note (§6.4,
   "either in the mask or loads the changed table") covers a CPU that
   *joins* the space during the shootdown; it does not cover the sender
   leaving.
5. **`smp_call_function_single` compares the target to its own CPU
   before disabling interrupts** (`kernel/interrupt/ipi.c:143`). Compare
   on A, migrate, run `fn` on B with interrupts off: the caller asked for
   A, A never runs it, and the caller's wait -- if `fn` publishes
   something A was meant to publish -- is for nothing. `irq-sync` and
   `smp-wake` are on the list.
6. **`lockup_sample_all` records "my" frame into "my" sample after
   sending to "the others"** (`kernel/core/lockup.c:88`, `:113`). From
   the tick it runs in interrupt context and cannot move; the two probe
   sites are the self-tests calling it directly, where a migration
   between the two reads leaves the sample of the CPU it left
   unanswered and the timeout names an innocent CPU. `lockup-soft` is on
   the list; the production path is safe and the test path is not.
7. **Two reads attributed to `lock_common`** (`kernel/core/spinlock.c`),
   the acquisition path, which disables preemption first. The probe
   cannot fire after that, so these are reads the symboliser attributes
   to the lock path from an inlined neighbour; the build identifies them
   before classifying them.

That the x86-64 probe does not see the `el2` case is expected: the
hypervisor backend is AArch64, and an AArch64 boot of the probe is the
build's first step, so the list above is a floor. The clock unit's
comment in `kernel/include/kernel/timer.h:74-81` ("once threads migrate
they are all foreign, which is what that unit's step 6 exists to
re-check") and the README's account of it describe the sweep this unit
has to re-run, not one it can skip: four plain `clock_now_ns() - x`
subtractions remain outside tests against 88 uses of the saturating
helpers.

**Lockdep's class table** holds 256 classes (`lockdep_core.h:17`); the
suite records 213 on both architectures. The graph's edge matrix is
`before[LOCKDEP_MAX_NODES][LOCKDEP_NODE_WORDS]` with four subclasses per
class: 128 KiB at 256 classes, in debug builds only.

**The suite** is 375 kernel self-tests on both architectures, run on
thread 0, plus the `init --selftest` sections and `lxtest`. CI runs the
debug suite, the same suite on the other interrupt controller
(`test-gic`), on a protection-capable CPU (`test-guard`), the release
suite, and the panic and WXN boots; each of those is a separate image
built with a compile-time knob into its own output directory
(`CRASH_TEST=1`, `OUT=$(OUT)-crash`).

## The problem

### A per-CPU answer has no rule, so nothing can be migration-safe on purpose

Every one of the seventeen sites above is correct today, and every one
of them is correct by the accident the migration report named: the
thread cannot move. There is no statement anywhere of *when* the value
`this_cpu()` returns may be kept -- not in `percpu.h`, not in the
scheduler's invariants, not in the lockdep or quiesce designs that rely
on it -- and no check that would fail a boot when the statement is
broken. A balancer added to this tree therefore breaks it in seventeen
places at once, in code that has no test for the property, and the
failures look like seven unrelated flakes. That is precisely what
happened. The audit cannot be done by reading, either: the static count
is 189 sites, and a grep cannot tell `might_sleep`'s inlined assertion
from `schedule_internal`'s pointer; the probe told them apart in one
boot because it asked the question at run time, in the context that
matters.

### The one lock order that will exist cannot be checked

The rule for two run-queue locks is written (S24) and cannot be
enforced by the tool this kernel built for exactly that purpose,
because both locks are one class. The previous attempt discovered this
on its first boot, annotated the second acquisition as deliberate, and
lost the check. Review is not a substitute for a checker that runs on
every boot in CI, and the balancer of the next unit will hold two of
these locks in both directions thousands of times per boot.

### Migration has no primitive, so the suite cannot be run under it

The balancer was three claims tested as one: that threads can be moved
at all, that the policy for when to move them is sound, and that the
rest of the tree survives being moved. When it failed, nothing said
which. There is no `sched_migrate` to write a test against, no way to
move one named thread once and look, and no way to run the existing 375
tests -- every one of them a test of some concurrency property -- while
threads move underneath them. The clock unit's premise that "a `t0` in a
local variable is a foreign stamp" was corrected to "false until
migration exists" (`timer.h:74-81`, README); until something migrates,
that unit's sweep cannot be re-checked either.

## Why it matters

- **The scheduler's biggest row stays open until this is done.** The
  inventory says migration "needs an audit of those before it can land,
  not just a working balancer". This is that audit, made mechanical so
  it stays done: a site added next month that keeps a per-CPU answer
  across a preemption fails the debug boot with its file and line.
- **The seventeen are real bugs waiting for a trigger.** Migration is
  one trigger; a CPU-offline evacuation or a wake-time re-pick would be
  another. Fixing them now, with a check that keeps them fixed, is
  cheaper than finding them one flake at a time behind the balancer.
- **lockdep's contract is "every order is checked or it is written
  down".** A class of lock it cannot see is a hole in that contract, and
  it is the class the next unit leans on hardest.
- **A suite that has run under random migration is evidence.** After
  this unit, "three of four boots fail" cannot happen to the balancer's
  author without a named cause, because the tree will have been booted
  under a migrator many times, at a rate, with the check on.

## Design

### 1. The rule, and the accessors that check it

> **A per-CPU answer may be kept only while the thread cannot move:
> preemption disabled, interrupts off, interrupt context, or an affinity
> of one CPU. `preempt_disable()` is the migration barrier: a thread
> that holds it is never `THREAD_READY` and is never moved.** (S25)

In `CONFIG_DEBUG` builds, `this_cpu()` and `arch_cpu_id()` check the
rule and panic on a violation, naming the caller. The predicate, in
`kernel/core/percpu.c`, reads the raw block and returns "kept safely"
when any of these holds: the scheduler has not started; `preempt_count
> 0`; `irq_depth > 0`; interrupts are disabled; the current thread is
an idle thread or has an affinity with one bit; the machine has one
CPU. The check costs one flags read and a few loads; release builds
compile it out and the accessors are the raw reads they are today.

Two raw accessors, `raw_this_cpu()` and `raw_cpu_id()`, exist for the
sites whose answer is the same on any CPU or whose staleness is
harmless, and **every raw use carries a comment saying which**. The
exempt sites by construction are `thread_current()`, `preempt_disable()`
/ `preempt_enable()` (they are the barrier), the check itself, and
`arch_irq_restore`'s preemption point. The 50 identity reads and 14
diagnostics from the table become raw with their reason; `process_current`
alone covers 27 of them through `thread_current()`.

The check ships as a panic. During the sweep it runs as a warn-once
table keyed by return address (the probe's form) so one boot lists every
remaining site; the build promotes it once both architectures boot the
whole suite clean, and the suite gets a test that provokes it on purpose
(`percpu-claim`, below) through an expect seam of the kind lockdep's
tests use (`lockdep_expect`, `kernel/include/kernel/lockdep.h:95`).

### 2. The seventeen, fixed by their mechanism

Each fix is the smallest change that makes the claim true where it is
made; none changes behaviour on a tree where nothing migrates.

- **The switch path reads its per-CPU block with interrupts off.**
  `schedule_internal` saves interrupts first and reads `pc` and `rq`
  inside the saved region (the lock it takes is already `irqsave`, so
  this moves one line up); `sched_set_running_current` the same. The
  `preempt_count == 0` and `irq_depth == 0` assertions stay where they
  are, reading raw, since they are identity reads.
- **`quiesce_note_quiescent` disables interrupts around the publish**,
  so the CPU it names is the CPU it runs on (Q20). Its callers in the
  switch path are then inside the region above anyway.
- **lockdep reads a CPU's held-spinlock stack only with interrupts
  off.** `lockdep_acquire_check`, `lockdep_acquired` and
  `lockdep_is_held` take the raw lock (or save interrupts) before
  `my_cpu()` for the mutex kind; the spin kind already runs with
  preemption disabled. L11 gains the sentence that says so.
- **The shootdown sender disables preemption across "compute the
  targets, flush here, interrupt the rest"**, on both architectures, and
  `user_shootdown` keeps it disabled from computing the mask to the
  return. A design.md §6.4 note states the second half of the rule: the
  sender does not leave either.
- **`smp_call_function_single` saves interrupts before it asks whether
  the target is itself.**
- **`lockup_sample_all` disables preemption from the first read to the
  local record**; the tick path is unchanged, the test path becomes
  right.
- **The two `lock_common` attributions** are identified and fixed or
  declared.
- **`el2`**: the self-test's claim becomes "on the CPU that asks": it
  disables preemption, calls the backend's own ready-and-ask entry (the
  form the run loop uses under `arch_irq_save` at `hv_el2.c:310`), and
  re-enables. The lazily filled `g_el2_ready[]` stays lazy, because the
  run loop fills it on whatever CPU runs a vCPU and that is right.
- **Tests** declare their claim: a worker that records "which CPU ran me"
  reads under `preempt_disable`; a test that wants "another CPU than
  mine" for the whole test pins itself with the affinity it can already
  ask for at creation, or reads raw and says the answer may be stale.

The AArch64 probe boot may add to this list; the build's banner records
the final count against the seventeen.

### 3. A lockdep class per run queue, so S24 becomes a checked order

Each run queue's lock is initialised with its own name from a static
table (`"runqueue0"` ... `"runqueue63"`, `CONFIG_MAX_CPUS` entries), so
every instance is its own class and a reversed pair of acquisitions is
a cycle lockdep reports. The lockdep design's rule that "a dynamically
built name is not a valid class key" stands; a static per-instance
table is not a dynamic name, and the design says so. The class ceiling
rises from 256 to 320 (the edge matrix grows from 128 KiB to 200 KiB,
debug builds only); on the four-CPU CI machine the suite records 216
classes, on a 64-CPU machine 276.

S24 is rewritten from "reserved" to the rule as enforced:

> **S24. Two run-queue locks are taken in increasing CPU-id order,
> always.** Each run queue's lock is its own lockdep class, so a reversed
> pair is a cycle the checker reports; the only holder of two today is
> `sched_migrate`.

S2 ("`runqueue.lock` is a leaf") gains its one exception in the same
sentence: a second run-queue lock, under S24.

### 4. `sched_migrate`: one thread, one move, an answer that says why not

```c
enum sched_migrate_result {
    SCHED_MIGRATED,              /* moved: t->cpu == cpu, queued there */
    SCHED_MIGRATE_SAME_CPU,      /* already there */
    SCHED_MIGRATE_NOT_READY,     /* RUNNING (no queue entry), BLOCKED, EXITED */
    SCHED_MIGRATE_CURRENT,       /* READY but rq->current: the woken-before-blocked window */
    SCHED_MIGRATE_AFFINITY,      /* cpu not in t->affinity */
    SCHED_MIGRATE_OFFLINE,       /* cpu not online */
};
enum sched_migrate_result sched_migrate(struct thread *t, unsigned cpu);
```

The primitive takes the two run-queue locks in increasing CPU-id order
with interrupts saved, re-reads `t->cpu` under the first lock (a
concurrent migration may have moved it: retry from the top, bounded by
the fact that a thread moves only under both its locks), checks the
state, the `rq->current` identity, the affinity and the online mask
*under the locks*, dequeues through the policy, sets `t->cpu`, enqueues
through the policy on the destination, and requests a reschedule there
if the moved thread outranks its current. `nr_running` moves with the
dequeue and enqueue as it does today. A refusal returns without
touching anything and says which check refused, so a caller and a test
ask *which*, not *whether* (the lesson of the last unit's review:
"a bool asked about the caller's guess"). The policy gains one hook:

```c
struct thread *(*pick_migratable)(struct runqueue *rq, cpumask_t allowed);
```

returning a ready thread on `rq` that is not `rq->current` and whose
affinity admits some CPU in `allowed`, or NULL. The round-robin policy
scans its lists from the lowest priority up, so the thread it offers is
the one the queue would run last. `sched_migration_count()` reports the
moves made since boot, for tests and `sched_dump`.

`sched_wake`'s unlocked read of `t->cpu` stays: a thread it can wake is
`THREAD_BLOCKED`, and a blocked thread is never moved (S26, below).

### 5. The suite under a chaos migrator

A compile-time knob, `SCHED_CHAOS=1` (`CONFIG_SCHED_CHAOS`, debug
builds), arms a migrator in the tick: every fourth tick (16 ms at
250 Hz), each CPU offers one migratable thread from its own queue --
`pick_migratable(rq, online & ~self)` -- to the next online CPU in a
rotation that its affinity admits, through `sched_migrate`. From the
tick, with interrupts off, holding two run-queue locks in order: the
same context as the balancer the next unit will build, and a stricter
adversary than it, because it moves threads for no reason. The count of
moves is printed at the end of the suite (`sched: chaos migrated N
threads, M refused`) so a run proves it exercised, and the boot test
requires `N > 0`.

`make test-chaos` builds that image into `$(OUT)-chaos` and boots the
suite the way `test-crash` and `test-wxn` do, and CI gains the job
"Boot test under a chaos migrator (debug)" on both architectures. Before
the pull request the build runs it **eight times per architecture**
(the migration report's own measurement was four per arm; a change to
scheduling timing needs a rate, not a run), and reports the tally in
the banner.

Thread 0 -- the self-test thread -- is *not* pinned. That is the point:
the `el2` case was found because thread 0 could not move, and the whole
suite running on a thread that moves is the audit's strongest form.

### 6. What it does not do

- **No automatic balancer.** The pull, its period and hysteresis are the
  next unit, on a tree where migration has already been survived. This
  unit's `sched_migrate` is what that balancer calls.
- **No re-pick at wake-up, no migration of a running thread, no
  evacuation on CPU offline.** Each is a policy on top of the primitive;
  none is needed to test the primitive.
- **No `migrate_disable` counter.** `preempt_disable` is already
  exactly that here (S25); a second counter would be a second name for
  the same thing, and the check would have to honour both.
- **No change to placement**, timers, or the clock. The clock sweep is
  re-run (below), not redesigned.

### 7. The clock claim, made true

`timer.h:74-81` says a local `t0` is not foreign "because a thread is
assigned a CPU once and never moves", and that this is about to widen.
After this unit it has: a thread preempted between two `clock_now_ns`
reads can resume elsewhere. The comment, the README's account and the
clock unit's report are updated to say migration exists and the rule
is the general one; the four remaining plain subtractions outside tests
are converted or declared same-CPU by construction (a read pair with
interrupts off, like the tick's own), and the sweep is recorded in the
banner by count.

### Lifetime, in one paragraph

`sched_migrate` holds both run-queue locks for the whole move, so a
thread is in exactly one queue at every instant a lock is not held, and
`t->cpu` names it. A ready thread's context is saved and nothing on the
old CPU refers to it once it is dequeued; the thread's timers, if any,
belong to the CPU that armed them and wake it through `sched_wake` on
whatever `t->cpu` is then, which is why blocked threads are never moved
(they may be on a timer queue's list, and the callback finds them by
`t->cpu`). A thread with preemption disabled is running, and running
threads are refused; a thread in the woken-before-blocked window is
`rq->current` and is refused by identity. The chaos migrator runs in
the tick, holds nothing but the two locks, and touches no thread it
has not dequeued.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/percpu.h`, `kernel/core/percpu.c` | the rule in a comment; `raw_this_cpu`, `raw_cpu_id`; the debug predicate `percpu_claim_ok` and the check (`percpu_claim_expect` seam for the test); `thread_current`, `preempt_disable`/`enable` on the raw form |
| `kernel/include/arch/percpu.h`, `kernel/include/arch/cpu.h`, `kernel/arch/x86_64/percpu.c`, `kernel/arch/aarch64/percpu.c` | the raw reads become `arch_percpu_get_raw` / `arch_cpu_id_raw`; the checked names wrap them in debug builds |
| `kernel/scheduler/sched.c` | `schedule_internal` and `sched_set_running_current` read their block with interrupts off; `sched_migrate`; `sched_migration_count`; the chaos tick hook under `CONFIG_SCHED_CHAOS`; run-queue lock names from the static table; raw reads declared (`pick_cpu`, `request_resched`) |
| `kernel/include/kernel/sched.h`, `kernel/scheduler/sched_internal.h`, `kernel/scheduler/policy_rr.c` | `enum sched_migrate_result`, `sched_migrate`, `pick_migratable` in the policy and its RR implementation |
| `kernel/core/quiesce.c` | the publish under saved interrupts; the sync path's reads declared |
| `kernel/core/lockdep.c`, `kernel/include/kernel/lockdep_core.h` | held-stack reads under the raw lock for the mutex kind; `LOCKDEP_MAX_CLASSES` 320 |
| `kernel/memory/vmm.c`, `kernel/arch/x86_64/mmu.c`, `kernel/arch/aarch64/mmu.c` | the shootdown sender under `preempt_disable`; stats reads raw |
| `kernel/interrupt/ipi.c` | `smp_call_function_single` saves interrupts before the self test; counters raw |
| `kernel/core/lockup.c` | `lockup_sample_all` under `preempt_disable` |
| `kernel/timer/timer.c`, `kernel/include/kernel/timer.h` | the offset read declared raw with its bound; the comment made true; the four subtractions |
| `kernel/block/blk.c`, `kernel/core/smp.c`, `kernel/core/faultinject.c`, `kernel/syscall/syscall.c`, `kernel/process/process.c`, `kernel/syscall/uaccess.c`, `kernel/include/kernel/lockdep.h` (`might_sleep`) | raw reads with their reason |
| `kernel-services/virtualization/hvtest.c`, `kernel/arch/aarch64/hv_el2.c` | the `el2` claim made on the CPU that asks; a ready-and-ask entry |
| `kernel/scheduler/schedtest.c`, `smptest.c`, `kernel/core/lockuptest.c`, `lockdeptest.c`, `quiescetest.c`, `kernel/arch/x86_64/{trap,fpu}.c` tests, `kernel/core/selftest.c` (`fpu_bench_pair`), `kernel-services/network/nettest.c` | each test's claim declared |
| `kernel/scheduler/smptest.c` | `percpu-claim`, `sched-migrate`, `sched-migrate-refuses`, `sched-migrate-stress`, `lockdep-rq-order` |
| `kernel/core/selftest.c`, `selftest.h` | five tests registered (380) |
| `Makefile`, `.github/workflows/ci.yml`, `tests/boot/run_boot_test.py` | `test-chaos`; the CI job; the chaos summary line required |
| `docs/kernel/scheduler/{design,api,invariants,testing}.md` | the rule; the primitive; S2 amended, S24 real, S25, S26; the tests and the chaos boot |
| `docs/kernel/lockdep/{design,invariants}.md`, `docs/kernel/quiesce/invariants.md` | per-instance classes; L11 amended; Q20 |
| `docs/kernel/memory/design.md` (§6.4), `docs/kernel/diagnostics/design.md`, `docs/kernel/timer/design.md` | the sender's half of the shootdown rule; the sampler; the clock claim |
| `docs/audit/next-subsystem-thread-migration.md`, `next-subsystem-cpu-clock.md`, `README.md` | annotated: the corruption named; the clock premise now true; Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | §2.3: the "audit of those" struck; "no migration" narrowed to "no balancer" |

## New APIs

```c
/* percpu.h */
struct percpu *raw_this_cpu(void);   /* the block, unchecked: identity reads and diagnostics only, with a reason */
unsigned raw_cpu_id(void);           /* the number, unchecked: same rule */
/* this_cpu() and arch_cpu_id() panic in CONFIG_DEBUG when the answer could not be kept (S25). */
#if CONFIG_DEBUG
void percpu_claim_expect(void);      /* test seam: the next violation is counted, not fatal */
unsigned percpu_claim_expected_hits(void);
#endif

/* sched.h */
enum sched_migrate_result sched_migrate(struct thread *t, unsigned cpu);
uint64_t sched_migration_count(void);
/* struct sched_policy gains: */
struct thread *(*pick_migratable)(struct runqueue *rq, cpumask_t allowed);
#if CONFIG_SCHED_CHAOS
void sched_chaos_stats(uint64_t *migrated, uint64_t *refused);
#endif
```

No syscall, no user-visible change. `lockdep_core.h`:
`LOCKDEP_MAX_CLASSES` 320.

## Migration plan

1. **The probe, on both architectures**, in warn-once form, and the
   final list of sites with their classification in the banner. This
   step changes no behaviour and is where the AArch64 additions appear.
2. **The seventeen (or more), fixed**, each with the boot that named it
   re-run clean; the identity and diagnostic reads made raw with their
   reasons. Boots both architectures, the check still warning.
3. **The check promoted to a panic**, `percpu-claim` added; both
   suites, host tests, release both.
4. **Per-instance run-queue classes**, the ceiling raised, S24 rewritten,
   `lockdep-rq-order` added (it takes two run-queue locks in the wrong
   order through a test hook and expects the report).
5. **`sched_migrate` and `pick_migratable`**, with `sched-migrate` and
   `sched-migrate-refuses`; then `sched-migrate-stress`.
6. **The chaos migrator**, `make test-chaos`, the CI job; eight boots
   per architecture, the tally recorded. Any failure here is a finding
   about the tree, fixed under this unit, not a flake to record.
7. **The clock sweep re-run** by grep and by the probe's list, the
   comment and README made true.
8. Docs, README Status, the inventory rows, the as-built banner.

Each step boots both architectures; steps 3, 5 and 6 run the release
build and `host-test`, and the whole CI list runs before the pull
request, since a scheduler change is exactly the kind that passes
`make test` and fails something else.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `percpu-claim` | with the seam armed, a preemptible read of `arch_cpu_id()` in an unpinned thread on a multi-CPU machine is reported once; the same read under `preempt_disable`, under `arch_irq_save`, and from a thread created on one CPU is not | drop the `preempt_count` term from the predicate: the disabled read is reported and the test names which of the three quiet forms spoke |
| `lockdep-rq-order` | taking run queue 1's lock then run queue 0's, through a test hook, is reported as a cycle against the recorded order 0→1 | initialise every run-queue lock from the one literal again: the acquisition reads as recursion (a different report kind) or, once annotated, as nothing, and the expected cycle never arrives |
| `sched-migrate` | a worker pinned to no CPU, made READY on CPU A by being preempted by a higher-priority spinner there, is moved to B by `sched_migrate` (`SCHED_MIGRATED`), runs next on B (it records `raw_cpu_id()` under `preempt_disable` each time it runs), and `sched_migration_count` rose by one | skip the enqueue on the destination: the thread is on no queue, never runs again, and the test's bounded wait names it and its state |
| `sched-migrate-refuses` | each refusal by its name: a thread pinned to A → `AFFINITY`; a blocked thread → `NOT_READY`; the target offline → `OFFLINE`; A to A → `SAME_CPU`; and **the window**: a worker calls `waitqueue_prepare` (BLOCKED, still running), signals the test, and spins on a flag; the test wakes it (READY, queued, still `rq->current`) and asks to migrate it → `CURRENT`; then releases the flag and the worker finishes its wait normally | remove the `rq->current` identity check: the running worker's queue entry moves to B while it executes on A; `sched_set_running_current` on A fails to find it (the test checks the state and both queues under the locks before anything can switch) |
| `sched-migrate-stress` | for 200 ms: eight spinners, eight sleepers on 1 ms timers, four wait-queue ping-pong pairs, two mutex contenders, and one migrator thread that moves any migratable thread it finds to a random admitted CPU; every worker's counter advanced, every affinity was honoured (each worker checks `raw_cpu_id() & affinity` under `preempt_disable` each round), lockdep is clean, `threads_settle` holds, and the migration count is at least 100 | take the two locks in decreasing order in `sched_migrate`: the migrator and the chaos tick cross, lockdep reports the cycle (it can now); and with the shootdown fix reverted, the stress's file-mapped worker reads a stale page (a mutation the probe boot and this test hold together) |
| `sched-spread`, `smp-*`, the whole suite under `test-chaos` | unchanged assertions, with threads moving underneath them; the chaos count at the end is > 0 | with the `schedule_internal` fix reverted, the chaos boot fails in the switch (the previous unit's seven-test failure, now with a named cause); with the quiesce fix reverted, `quiesce-grace`'s guarded object is freed under a reader |

**Vacuity, named in advance.** `sched-migrate` must not pass because
the worker was *created* on B: the worker's first recorded CPU is A and
the assertion is on the change. `sched-migrate-stress` must not pass
with zero migrations: the count is asserted, and the migrator names the
refusal reasons it saw if it reaches none. The chaos boot must not pass
with the migrator idle: the summary line is required by the boot test.

## Benchmarks

- **The check's cost**, debug builds: `selftest_irqrestore_bench` and
  the syscall round-trip in `init --selftest`'s bench line, before and
  after; release builds are unchanged by construction and the reproducible
  build says so.
- **`sched_migrate`**: nanoseconds per move under the stress (two locks,
  a dequeue, an enqueue, a possible IPI), x86-64 and AArch64.
- **The chaos boot**: suite wall time and the migration tally against the
  plain debug boot, per architecture.
- **lockdep**: classes and edges recorded with per-instance run-queue
  classes against 213/941 today.

## Risks

- **The check fires at sites only some configurations reach**: the
  AArch64 GICv3 paths, modules (the one unresolved site), the Linux
  personality, the guard-capable CPU. Mitigation: the sweep runs
  `test-gic`, `test-guard` and a module-loading boot in warn-once form
  before the promotion, and the panic names the site, so a CI-only
  sighting is a one-line fix, not a mystery.
- **The chaos boot will find things the seventeen do not cover** -- that
  is what it is for -- and a finding there is a bug in this unit's scope,
  even when it is in another subsystem. The migration report's history
  says such findings arrive as flakes; the check and the named-cause
  rule are what stop them from being recorded as such.
- **Tests that read "my CPU" unguarded** become panics under the check.
  There are 31; each is a small edit, but a missed one in a rarely run
  branch fails a boot. The warn-once step lists them first.
- **Per-instance classes multiply edges**: every waker's lock precedes
  every run-queue class now, not one. On four CPUs that is a few hundred
  edges; the ceiling and the search scratch are sized for it, and the
  banner records the count.
- **The clock's local stamps become foreign** the moment the chaos boot
  runs; a subtraction the sweep missed becomes a wrapped interval. The
  four known sites are converted; the saturating helpers make a missed
  one a wrong measurement rather than a hang.
- **Thread 0 moving** exposes any boot-time assumption made by the code
  that runs the suite itself. That is intended, and the `el2` case is
  the known one.

## Alternatives considered

- **Build the balancer again, carefully.** That was the previous unit,
  and it failed without a named cause because three claims were tested
  together. The primitive plus the chaos adversary separates "the tree
  survives migration" from "the policy is good", and the next unit gets
  the second on its own.
- **Pin thread 0 to CPU 0** and leave the tests alone. It would hide the
  `el2` class rather than declare it, and the suite's own thread is the
  best-instrumented migration victim the tree has.
- **A `migrate_disable()` counter**, as Linux has. Linux needs one because
  preemption-disabled regions there are also spinlock-wait regions it
  wants to keep short; here `preempt_disable` already forbids a switch
  outright, so a migrate-disabled thread would be exactly a
  preempt-disabled one under another name.
- **Keep one run-queue class and encode the order in the subclass.**
  The subclass belongs to the acquisition, not the lock: lockdep would
  record `runqueue#0 → runqueue#1` for both directions and see no cycle.
  The order is between instances, so the classes must be instances.
- **A static audit by grep** instead of a runtime check. 189 sites; the
  grep cannot tell an inlined `might_sleep` from `schedule_internal`'s
  pointer, and it does not stay done. One probe boot classified 113
  sites in an afternoon and will do it again on every boot.
- **A boot-time knob for the migrator instead of a compile-time one.**
  The kernel has no command line, and the tree's precedent for a
  different-behaviour boot is a compile-time knob in its own output
  directory (`CRASH_TEST`), which also keeps the migrator's code out of
  the ordinary debug image.
