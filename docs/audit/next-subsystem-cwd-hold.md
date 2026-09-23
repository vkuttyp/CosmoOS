# NEXT SUBSYSTEM — a held walk: the working-directory race becomes a proof

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **The swap half needed a rule the report did not have.** The racer's
>    own setup `chdir`, made before its threads existed, was the first
>    `chdir` the seam saw: it registered as the swapper, waited its whole
>    two-second bound for a hold that could not come, and wrote that
>    timeout into the record — so both doors "failed" on `swap_timed_out`
>    while the real swap took eighty microseconds (the instrumented
>    record said so; the theory did not). A `chdir` in a single-threaded
>    process now registers nothing: there is no other thread whose walk
>    it could pull from under.
> 2. **`released_after_put` was derived from the wrong reading, twice.**
>    First from the count at the hold, which the `outlive` pass's own
>    `rmdir` lowers by one between the hold and the put, so a correct
>    kernel failed. Then from the count the *walk* read on resume, which
>    let the release-before-put mutation **survive**: a walk woken a few
>    instructions before the put loses the race to it every time, so the
>    two orders read the same. It is now read by the **releasing** side at
>    the instant it releases — one below what it read before its put —
>    which is ordering-sensitive and race-free, and the mutation dies at
>    both doors in every pass.
> 3. **The wait-after-publish mutation survived too**, and for a reason
>    the report should have seen: in every racer thread A reaches the
>    seam before thread B reaches `chdir`, by construction, so the order
>    the rule exists for never happened. A third native pass,
>    `swapfirst`, starts the swapper first and starts the walker only once
>    `debug.cwd_hold` reads **4** — a new state, "armed and the swapper is
>    waiting inside `chdir`" — never on time. Under the mutation the
>    walker captures `d2`, `held_matches_old` is false and the open is
>    `-ENOENT`; on a correct kernel the pass is indistinguishable from
>    `capture`. Native only: a Linux program has no sysctl to read the
>    state from, and the doc says so.
> 4. **The door mutations were caught earlier than predicted, by the
>    count.** With the reference removed at either door's `open`, the
>    `capture` pass sees **two** references before the put where a correct
>    kernel sees three (ramfs's pin, the process's, the walk's) — the walk
>    holds none — and the other door's passes stay green, which is what
>    "separately proved" means. The `outlive` pass then ends the boot as
>    the report said: `KERNEL PANIC: cwd walk resumed on a freed directory
>    (pid N): type 0x5a5a5a5a refcount 0x5a5a5a5a`. Every pass now runs
>    before any is judged, so the first pass's failure cannot hide the
>    second's panic.
> 5. **Without the poison, the refcount check is what fires**: `type 0x2
>    refcount 0x0`. The poison is not what makes the proof; it is what
>    makes the *old* test a sometimes-detector, and what makes the panic
>    name a poisoned object rather than one that merely reads as
>    unreferenced.
> 6. **Two sightings of `tcp-pcb-timer-free` on the way**, in the same
>    slot right after `net-lo-udp`, on this branch's own x86-64 boots:
>    the hard lockup the report's measurement met, and once a `TLB
>    shootdown acknowledged by 2 of 3 CPUs`. Both fit one mechanism —
>    the callback spinning in interrupt context above the armer thread
>    that started its 1 ms timer on the same CPU, with the test blocked in
>    `thread_join(armer)` and the releaser never created. Recorded in
>    `docs/testing/flakes.md`; the fix is another unit's and is proposed
>    there, not made here.
> 7. **The Linux door needed a native parent.** A kernel-created process
>    is always native (`process.c`: "kernel-created processes are always
>    native"), so `cwd-hold-linux` spawns `init --probe cwd-hold-linux:<pass>`
>    and init spawns `lxcwd`; the seam is armed for the name `lxcwd`.
>
> **The mutations**, each applied alone on x86-64, each boot confirmed
> booted, run twice — the second round after items 2 and 3:
>
> | # | mutation | what failed (round 2) | round 1 |
> | --- | --- | --- | --- |
> | 1 | the reference removed at the native `open` | `capture`: `ref_before_put == 3` fails (2: the walk holds none); `outlive`: `KERNEL PANIC: cwd walk resumed on a freed directory (pid 197): type 0x5a5a5a5a refcount 0x5a5a5a5a`. The Linux door's passes are untouched | caught in `capture` by the count; the test stopped there |
> | 2 | the reference removed at the Linux door's `do_open` | the mirror image: native's three passes green, Linux `capture` fails on the count, Linux `outlive` panics by name (pid 202) | the same, stopped at `capture` |
> | 3 | the release moved before the put | `released_after_put` false in **every** pass at **both** doors | **survived**: derived on the walk's resume, which loses the race to the put |
> | 4 | the swapper's wait moved after the publish | `swapfirst`: status 16 (the open failed `-ENOENT`), `held_matches_old` 0, `ref_before_put` 2 — the walker captured `d2`. `capture` and `outlive` pass, as they must: A is ahead in both | **survived**: no racer had the swapper ahead |
> | 5 | the poison removed, with mutation 1 kept | `KERNEL PANIC: cwd walk resumed on a freed directory (pid 197): type 0x2 refcount 0x0` — the refcount check, as predicted | — |
>
> **As run**: the seam idle adds one load to every relative walk in a
> debug build and nothing to a release build; the racers take 25–90 ms
> per door in the boot. No benchmark, as the report said.

Constitution §68 report. It takes the inventory's §1.3 row *"the cwd-ref
fix is a regression test, not a proof; the seam that would prove it is
named and not built"* (`docs/audit/2026-09-deferred-work-inventory.md`),
and closes it by building the seam that row names.

## Problem

The cwd-ref unit (`docs/audit/next-subsystem-cwd-ref.md`) fixed a
use-after-free that needed no privilege: every relative-path system call
read `process_current()->cwd` with no lock and no reference, handed the
raw pointer to a path walk that can block, and another thread's `chdir`
could swap the field and drop what was the last reference while the walk
was inside the directory. The fix is `process_cwd_get()` — a reference
taken under `p->lock`, held for the length of the walk — at twenty sites
across both personalities, and invariant **P29** now states the rule for
every mutable per-process pointer.

**The fix is right and the test cannot see it.** That unit said so
itself, in its own "As run" table: every reverted-fix run passed,
including one with each freed vnode poisoned, *"because the walk is never
inside the few instructions where the free lands"*. The test
(`userland/tests/cwdtest.c`, step 4) moves the process in and out of a
directory another thread keeps removing while a third walks a relative
path inside it. It exercises the window about a thousand times per boot
and lands in it never. The unit labelled the test a regression test, the
README says the same (README.md:1637), and the inventory carries the row
this report closes.

### Measured

`tools/cwd-race-probe.py`, shipped with this report, puts the bug back at
**one** site — the native `open`, which is what step 4's walker calls —
and poisons every freed vnode with `0x5a`, so a walk that resumes inside
one cannot find memory that still looks like a directory. Then the
ordinary boot suite runs.

| boot | architecture | `cwdtest` verdict | walks in the victim | boot verdict |
| --- | --- | --- | --- | --- |
| 1 | x86-64 | **caught**: `#GP` in `kobject_get`, `RAX=5a5a5a5a5a5a5a5a`, during step 4 | — (the boot ended there) | FAIL, `KERNEL PANIC` |
| 2 | x86-64 | `0 failure(s)` | 766 | PASS |
| 3 | x86-64 | `0 failure(s)` | 986 | PASS |
| 4 | x86-64 | `0 failure(s)` | not recorded¹ | PASS |
| 5 | x86-64 | `0 failure(s)` | not recorded¹ | PASS |
| 6 | x86-64 | not reached² | — | FAIL, a hard lockup in the kernel self-tests |
| 1 | AArch64 | `0 failure(s)` | 889 | PASS |
| 2 | AArch64 | `0 failure(s)` | not recorded¹ | PASS |
| 3 | AArch64 | `0 failure(s)` | 797 | PASS |

¹ The harness keeps the serial log only for a failing boot, and the
shared `boot-test.log` is overwritten by the next run; the count was
lost before it was copied out. Recorded as lost rather than filled in.

² Boot 6 never reached the user-mode suite: CPU 3 spun for ten seconds
inside `timer_kick`'s debug hold — the callback seam `tcp-pcb-timer-free`
arms — with the other three CPUs halted in the idle loop, and the
hard-lockup detector ended the run. The mutation has no path into TCP's
timer (the unowned read is in `sys_open`, the poison is on a freed vnode,
and the spinning path holds neither), so this is a sighting of that
test's held callback parked where nothing could release it — by the look
of the idle CPUs, on the test thread's own CPU — recorded in
`docs/testing/flakes.md` with the log kept and the mechanism marked as a
guess, and not attributed to the probe. It counts as no boot of this
measurement. It is also, uncomfortably, the hazard this report's §4 is
about: a seam whose release depends on a thread that may not run.

**The rate, stated as a rate**: one catch in five x86-64 boots that
reached the test, none in three AArch64 boots. Eight boots is a small
sample and the number is not the point; the shape is. Every miss had the
walk inside the victim hundreds of times.

**With the bug present and the vnode poisoned, the test catches it in
some boots and not in others.** The catch is unmistakable when it comes:
a `#GP` at `kobject_get` with `RAX=5a5a5a5a5a5a5a5a` during step 4 — the
walk taking its reference on a directory that has already been freed,
which is the use-after-free itself, one instruction into `walk_parent`.
The miss is equally plain: the same step, some hundreds of walks inside
the victim, `0 failure(s)`, and the boot passes.

Two things follow, and the second corrects the cwd-ref unit's record.

- **The rate is the finding, and a rate is not a proof.** A test that
  fails on a broken kernel in some boots is a flake generator on that
  kernel and a silent pass on this one. Its pass says nothing about the
  fix that a coin does not.
- **The cwd-ref unit's "poisoned run passes" was wrong.** Its "As run"
  table records `memset(vn, 0xAA, …)` before `kfree` as passing, and
  concluded the walk "is never inside the few instructions where the
  free lands". With the poison at `vnode_release` — the one place every
  vnode free passes through — the walk *is* inside them, at a rate the
  table above measures. Whatever that unit's `memset` poisoned, it was
  not what the walk read. The README's sentence to the same effect
  (README.md:1630) is corrected in this unit's docs commit.

So the tree today has a use-after-free fix whose test cannot see the
bug, because nothing poisons a freed vnode; with the poison it sees the
bug sometimes. The seam is what makes it see the bug every time.

### Why a regression test is not enough here

Three reasons, in the constitution's order.

1. **Correctness.** P29 exists because an argument of the form "only the
   process itself writes this" stopped being a single-writer argument the
   day threads arrived, and nothing in the tree noticed. A rule that was
   violated silently once deserves a test that fails loudly, not one that
   passes on a broken kernel a thousand walks a boot.
2. **§70.** The constitution asks for deterministic tests where the tree
   can have them. This tree can: it has built a held-fault seam three
   times in the last week (the file fault, the anonymous fault, the
   virtio removal's hold-per-pop) and a condition-variable probe before
   that, all of one shape — *arm by identity, hold at the mechanism,
   release on the collision itself, never on a clock*. The cwd-ref report
   named exactly this seam as "what would turn it into a proof" and
   deferred it as a scope decision. The scope is this unit.
3. **The class, not the instance.** P29's census lists the per-process
   fields that are mutable after spawn. `cwd_locked` is the one that
   already had a defect; `handles` is the next candidate, and a field
   added next month is the one after. The seam built here is the proof
   template for the class: a walk held with its pointer in hand while the
   swapper does what a swapper does.

## Current implementation

**The reference.** `process_cwd_get()` (`kernel/process/process.c:1268`)
reads `p->cwd_locked` and takes a reference in one `p->lock` section.
`process_cwd_snapshot()` does the same plus the path, from the same
acquisition. Both doors' `open`, `stat`, `mkdir`, `unlink`, the unix
socket's `bind`/`connect` and `spawn` use them; the raw field is read by
nothing outside `process.c`.

**The swap.** `process_chdir()` (`process.c:1300`) looks the target up
from a snapshot, then publishes vnode and path together under `p->lock`,
and puts the old vnode *outside* the lock with the comment "safe to be
the last reference now: every walk that started while this was the cwd
took a reference of its own". That comment is the claim under test.

**The walk.** Three callers walk a path: `resolve()` (behind
`vfs_lookup`), `vfs_open()`, and the parent-lookup used by `mkdir`,
`unlink` and their kin — and **`vfs_open` does not go through
`resolve`**; it calls `walk_parent` itself (`vfs.c:1309`). What every
one of them shares is `walk_parent` (`vfs.c:933`), whose relative branch
is the first and only place a walk consumes its starting directory:

```c
    } else {
        cur = start;
        vnode_get(cur);      /* the cwd pointer, dereferenced here */
    }
```

A relative path is the only kind that reaches that branch; an absolute
one takes `vfs_current_root()` whatever `start` is. So the seam has one
possible home, and it is that line — not `resolve()`, which the report's
first draft named and which the racer's `open` never enters.

**The free.** `vnode_put` on the last reference unhashes the vnode under
the mount lock and calls `vnode_release`, which drops the page cache,
calls the filesystem's `evict`, and `kfree`s. Nothing poisons the object:
the pmm poisons freed *frames* (`kernel/memory/pmm.c`, `CONFIG_DEBUG`),
the slab zeroes on allocation, and a freed vnode keeps looking like a
directory until its memory is reused. That is why the test as it stands
passes on a broken kernel every time, and why the probe's poison is what
turns it into a sometimes-detector: the first thing a walk does with its
base is `vnode_get`, and `kobject_get` on `0x5a5a…` is a `#GP`.

**The seams this tree already has**, because the design below is their
fourth instance and departs from them in one respect only:

| seam | arm | hold | release |
| --- | --- | --- | --- |
| `vm_test_file_hold_arm()` | the next FILE fault in any user space | between the fault's two phases | another FILE fault installs, or the range is unmapped |
| `vm_test_anon_hold_arm(va)` | one page, by address | before the anonymous install, stale flags in hand | another thread installs that page |
| `vblk` hold per pop | from inside the worker | per unit of work | the removal's own boundary |
| `__cosmo_cond_probe` (libc) | one-shot, immediately before the wait | at the sleep window | the signaller |

Every one is event-driven. Every one is armed by something narrower than
"the next one anywhere" once a wider arm was found to catch the wrong
customer (the anonymous seam's first version would have held the
process's own stack faults). This seam is armed by **process name**, for
the reason given below.

**The test.** `cwdtest` is run from `rc.test` on both architectures; its
verdict is part of the user-mode suite's `FAILS`. Its Linux twin does not
exist: `tests/linux/lxtest.c` covers `chdir`/`getcwd` single-threaded,
and the cwd-ref unit's test 2 ("the same program under the Linux door")
was never written. The Linux test programs build on both architectures
(`tests/linux/linux.mk`; only the musl canary is x86-64-only).

## Why it matters

- A use-after-free reachable by an unprivileged program is the class of
  defect this repository treats as security, and the fix for it is
  guarded by a test that passes when the fix is removed. Any refactor of
  `process_chdir`, `resolve`, or the twenty call sites can reintroduce the
  bug and CI will stay green.
- The Linux personality is the door where threads have been reachable
  longest, and "a check only in `native.c` is half-enforced" is a lesson
  this repository has paid for. The Linux door has **no** test for this
  at all, regression or otherwise.
- P29 is stated as a census. A census without a proof template is a list
  the next author extends with a comment; with one, it is a list the next
  author extends with a test.

## Design

### 1. The seam: a walk held with its pointer in hand

Two halves, one state machine, `CONFIG_DEBUG` only, in the VFS where the
pointer is consumed rather than in `process.c` where it is produced —
because the bug-proof removes the reference *at a call site*, and a seam
inside `process_cwd_get()` would never see the mutated path.

**The walk half**, in `walk_parent`'s relative branch, immediately before
`vnode_get(cur)` — the one line every relative walk from every caller
passes through (see "The walk" above; `vfs_open` never enters
`resolve`). If the seam is armed for this process, the calling thread is
not the registered swapper, and `start` is non-NULL, this walk becomes
the held one.
It records `start`, its refcount, and the thread, sets state **held**,
and waits — a **killable** wait on a wait queue with a bounded deadline,
because that is how this kernel's threads leave a dying process: "every
other thread leaves at its next return to user mode or killable wait"
(`process_exit`), and a seam that parked a thread in an unkillable sleep
would hold up the exit of the very process it is testing. A walk woken by
a kill returns `-EINTR` from the lookup, which is what any killable wait
in a system call returns, and the record says it was interrupted. When
it resumes normally it performs **the liveness check**, stated exactly
because pass 2 below resumes on a directory that is *dead* and must not
trip it:

```
live(vn) :=  vn->type == VNODE_DIR
          && kobject_refcount(vn) != 0
          && kobject_refcount(vn) != 0x5a5a5a5a     /* the poison word */
```

`VNODE_DEAD` is **not** part of the predicate. It means "unlinked, no new
lookups" (`walk_parent` answers `-ENOENT` for a dead directory), and a
dead directory a walk still references is exactly what pass 2 produces
on a correct kernel; the seam *records* the flag rather than judging it.
A freed vnode fails the predicate on its type under the poison below
(the type reads `0x5a5a5a5a`, not `VNODE_DIR`) and on its refcount
without it (`kobject_release_final` leaves the count at zero). The
failure is a named panic — `cwd walk resumed on a freed directory
(pid %d)` — rather than a wild fault in `vnode_get`. Then the walk
proceeds exactly as it would have.

**The swap half**, in `process_chdir()`, in three touches. Before its
own lookup it registers the calling thread as the swapper (so the walk
half never holds it — a `chdir` with a relative path also walks, and
holding the thread that is supposed to do the releasing is a deadlock,
not a rule to write in the test). **After its lookup and before it
publishes** — outside `p->lock`, with the new directory in hand — it
waits, bounded, for state **held**. Then it publishes as it does today,
and immediately before `vnode_put(old)` records whether the held walk's
pointer *is* `old`, records `old`'s refcount, puts, records that the put
has happened, and completes the held walk.

The wait sits before the publish and not before the put, and review of
this report's first draft is why: with the wait after the publish, a
swapper that reaches `chdir` first installs `d2`, the walker then
captures `d2` and is held on it, and the swapper puts `d1` — a race
between two threads that the seam was supposed to order, and one that
makes pass 1 fail on a correct kernel. Waiting before the publish means
the walk that is held has necessarily captured the directory about to be
replaced, and `held_matches_old` in the record is the assertion that it
did rather than an assumption that it must have.

So the order the race needs is *enforced by the seam*, not arranged by
the test: the swapper cannot publish until a walk holds the old pointer,
cannot put until it has published, and the walk cannot resume until the
put has happened. No thread in the
racer sleeps, spins on a sysctl, or guesses. That is the one respect in
which this seam departs from the file-fault one, whose probe program
polls `debug.file_fault_hold`: a program under the Linux door has no
sysctl to poll, and an order enforced in the kernel serves both doors
with one racer each.

**What the seam records**, for the kernel test to read back after each
pass: whether a walk was held; whether the held pointer was the directory
the swapper replaced; the refcount at hold; the refcount the swapper saw
before its put; whether the put preceded the release (the correct order
— a mutation that releases first would make the whole proof vacuous, and
the test asserts this flag rather than trusting the seam); whether the
resumed directory was `VNODE_DEAD`; whether either wait timed out or was
interrupted. On a correct kernel
the refcount before the put is **two** (the process's and the walk's)
and the vnode survives; with the fix removed it is **one**, the put
frees it, and the resumed walk panics on the poison.

**State**: 0 idle, 1 armed, 2 held, 3 released; readable as
`debug.cwd_hold`. **One arm serves one hold.** The state machine ends at
*released* and does not rearm itself, so a racer that ran both passes
under one arm would run the second — the one about lifetime —
uninstrumented, with a record that still described the first. The
kernel test therefore spawns the racer **once per pass**, arms before
each spawn, and reads a record per pass (`--held capture`,
`--held outlive`). Arming by **process name** rather than pid, because
the pid is known only after the spawn returns and by then the child may
be running — the child cannot wait for an arm it has no door to observe.
A name is known before the spawn, and the seam binds to the first
process of that name whose walk it holds.

**Both waits are bounded** (the held walk at five seconds, the swapper at
two) **and killable**. A timeout is recorded and *fails the test*, so a
racer that is wrong hangs nothing and says so; a kill releases either
side at once. The bounds are not what the proof rests on: on a correct
interleaving neither is reached, and the events are what release each
side.

### 2. Freed vnodes are poisoned in debug builds

`vnode_release` fills the object with `0x5a` before `kfree`, under
`CONFIG_DEBUG`, as the pmm already does to frames. This is the cheap
half of the unit and it stands on its own: it is the difference between
a use-after-free that faults and one that reads a plausible directory,
and it costs one `memset` per vnode free in a build that already poisons
every freed page. The measurement above is what it buys alone — step 4
becomes a detector that fires in some boots. The liveness check in the
seam reads a poisoned type and refcount and names the defect instead of
leaving it to a `#GP` in `kobject_get`; the seam is what makes it fire
in every boot.

### 3. Two racers, one per door

Each is a two-thread program that runs the one pass its argument names;
the kernel test spawns it once per pass with the seam armed for its
name, waits for it, and reads that pass's record.

**Pass 1 — the walk resolves against the directory it started in.**
Thread A opens `f`, which exists only in the current directory `d1`;
thread B `chdir`s (absolute path) to `d2`, where `f` does not exist. A
is held with `d1` in hand; B's swap releases it; A's open **succeeds**.
This is the positive half: the walk used the directory it captured, not
the one the process has now.

**Pass 2 — the reference outlives the swap.** From inside `d1`, thread A
opens `f`; thread B removes `f` and `rmdir`s `d1` **by absolute path**
— a relative `unlink` from B would itself be a relative walk, and if it
came before A's `open` the seam would hold B, the thread that is supposed
to release, before B ever reached `chdir` and registered as the swapper
(found in review). The absolute path never enters `walk_parent`'s
relative branch. And the seam checks the rule rather than trusting it:
a held B times out, proceeds to `chdir`, and the swap half finds the
held thread is itself, which the record reports as `swapper_was_held`
and the test asserts false — so the mistake fails by name. B `rmdir`s
`d1` (allowed: `remove_entry`
refuses a mountpoint, a mount root and a sticky entry, `ramfs_rmdir`
refuses a directory that is not empty — which is why `f` goes first —
and nothing refuses a directory for being somebody's cwd; a `VNODE_DEAD`
directory is still a vnode), and `chdir`s away. On `ramfs` the `rmdir` drops the
entry's pin, so B's swap holds the last reference the *process* has.
With the fix, the held walk holds one more: refcount two before the put,
the vnode survives, A's open is `-ENOENT` (the directory is dead, not
freed). With the fix removed at that door's `open`, refcount one, the put
frees, the poison lands, and the resumed walk panics by name.

**Native**: `userland/tests/cwdtest.c` gains `--held capture` and
`--held outlive`, each running its pass and nothing else, so the kernel
test has a child that does one thing per spawn; the four existing steps
stay as they are and keep their "regression" label in the testing doc.

**Linux**: `tests/linux/lxcwd.c`, freestanding like `lxtest`, threads by
`clone(CLONE_VM|CLONE_THREAD|...)` as `lxtest` already does, relative
`openat(AT_FDCWD, "f")`, `chdir`, `unlinkat`, `unlinkat(AT_REMOVEDIR)`.
Same two passes, one per invocation, same exit codes.

### 4. The §70 gate

**Correctness.** The seam adds, on the walk side, one load-and-compare
of a global state word to `resolve()` in debug builds, and nothing in
release builds. It changes no lock order: the held walk waits holding
nothing (it is at the top of `resolve`, before `walk_parent` takes any
lock), and the swapper waits holding nothing (after `p->lock` is
released, before the put).

**Concurrency.** The seam is process-global, as every seam here is, and
inherits the condvar report's rule: armed immediately before the run it
instruments, and cleared on every exit of the test that armed it,
including the ones that fail. It binds to one process name and one
thread on each side; a second walk in the bound process while one is
held passes through untouched, so the racer's other thread is never
caught by accident.

**Ownership and lifetime.** The seam holds no reference of its own; it
*records* refcounts and lets the code under test own what it owns. That
is the point: a seam that took a reference would hide the defect it
exists to expose.

**Failure.** A timeout on either side is a recorded failure. A held
thread whose process is killed or whose sibling calls `exit`: both waits
are killable, `process_exit` and `process_kill` wake every thread of the
process and `process_kill_pending` makes the wait return `-EINTR`, so the
held walk leaves at once, as any blocked system call does. The deadline
is for a racer that is *wrong*, not for one that is dying.

**Security.** Debug-only, a global the kernel test writes, no user-facing
surface beyond a read-only sysctl of one integer.

**Performance.** None outside debug builds; inside them, a load and a
compare on the relative-path walk and a `memset` per vnode free.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/vfs/vfs.c` | the seam's walk half in `walk_parent`'s relative branch, before `vnode_get(cur)` — the one line every caller's relative walk shares; `vnode_release` poisons under `CONFIG_DEBUG`; `vfs_test_cwd_hold_*` |
| `kernel/process/process.c` | the swap half in `process_chdir`: register as swapper before the lookup, wait for the hold **before publishing**, then record-put-release around `vnode_put(old)` |
| `kernel/include/kernel/vfs.h` | the seam's API and its record struct; a comment on the poison |
| `kernel/syscall/native.c` | `debug.cwd_hold` |
| `userland/tests/cwdtest.c` | `--held`: the two passes |
| `tests/linux/lxcwd.c`, `tests/linux/linux.mk` | the Linux racer, added to `LINUX_TEST_PROGRAMS` |
| `kernel/process/proctest.c`, `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | `cwd-hold-native`, `cwd-hold-linux` |
| `docs/kernel-services/vfs/design.md`, `invariants.md`, `testing.md` | the seam under "Testing strategy"; the poison under "Memory"; the test entries |
| `docs/kernel/process/invariants.md` | P29's Check line names a proof |
| `docs/kernel/process/testing.md` | `cwdtest`'s section: steps 1–4 stay regression tests, `--held` is the proof |
| `docs/compat/linux/testing.md` | `lxcwd` |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §1.3 row struck |
| `README.md` | Status entry |
| `tools/cwd-race-probe.py` | shipped with this report; `tools/elf-share-probe.py`'s revert now touches what it restores, the same fix |

## APIs

### New

```c
/* kernel/include/kernel/vfs.h -- CONFIG_DEBUG only; no-ops otherwise.
 *
 * Hold the next relative-path walk made by a process of this name
 * before it dereferences its starting directory (walk_parent's relative
 * branch), until that process's chdir has published its new directory
 * and put the old one. The order is enforced here, not arranged by the
 * test: the chdir waits for the walk to be held BEFORE it publishes,
 * the walk waits for the put. One arm, one hold; arm again for the next
 * pass. Both waits are killable and bounded; a timeout is recorded,
 * never hidden. */
void vfs_test_cwd_hold_arm(const char *process_name);
unsigned vfs_test_cwd_hold_state(void);           /* 0 idle, 1 armed, 2 held, 3 released */

struct vfs_cwd_hold_record {
    bool held;                 /* a walk was held */
    bool held_matches_old;     /* the held walk's directory is the one the swapper replaced */
    bool swapper_was_held;     /* the swapper's own relative walk was what got held: a wrong racer, named */
    bool released_after_put;   /* the swapper put before it released -- the order the proof needs */
    bool resumed_dead;         /* the directory was VNODE_DEAD when the walk resumed (pass 2 expects it) */
    bool walk_timed_out, swap_timed_out;
    bool interrupted;          /* a killable wait returned -EINTR: the racer was dying */
    uint32_t ref_at_hold;      /* the directory's refcount when the walk was held */
    uint32_t ref_before_put;   /* what the swapper saw before its put: 2 with the fix, 1 without */
};
void vfs_test_cwd_hold_disarm(struct vfs_cwd_hold_record *out);   /* after every pass and on every exit; returns what happened */
```

Three entry points and one struct; nothing else is added.

### Existing, relied on

Named here so that nobody proposes them twice: `kobject_refcount`
(`kernel/object/object.c`) is what the record reads;
`wait_event_killable` and `process_kill_pending` (`kernel/wait.h`,
`process.c`) are what make both waits leave with a dying process;
`VNODE_DEAD` is the flag `remove_entry` already sets on a removed
directory; the seam records it and the liveness check does not judge it,
because pass 2 resumes on a dead, live directory by design.

## Migration plan

1. **The poison**, alone, both architectures booted. It changes nothing
   observable on a correct kernel and every existing test must agree.
2. **The seam**, unarmed. Every existing test must agree; the idle cost
   in `walk_parent` is one load.
3. **The native racer and `cwd-hold-native`**, one spawn per pass. Then
   the bug-proof: the probe's mutation at the native `open`, which must
   turn the boot into the named panic. If it does not, the seam is wrong
   and nothing else proceeds.
4. **The Linux racer and `cwd-hold-linux`**, and the same mutation at
   the Linux door's `do_open`.
5. **The order mutation**: release before put. The test must fail on
   `released_after_put`, or the test trusts the seam it is supposed to be
   checking.
6. Docs, inventory, README, the as-built banner; release builds,
   `gmake host-test`, every mutation alone with the runner confirming
   each boot.

## Tests

| test | what it proves | bug-proof |
| --- | --- | --- |
| `cwd-hold-native`, pass 1 (`capture`) | a held walk resolves against the directory it captured: `open("f")` succeeds after the process has moved to a directory without `f`, **and** the record says `held_matches_old` — the walk held `d1`, not `d2` | the swapper does not wait before publishing (the first draft's design): the walker can capture `d2`, `held_matches_old` is false and the open is `-ENOENT`, and the test fails by name. The open's success alone cannot be the proof: once a walk has captured `d1`, nothing the seam does afterwards changes what it opens, which is why the record carries the ordering and the test asserts it |
| `cwd-hold-native`, pass 2 (`outlive`) | the walk's reference outlives the swap: refcount **2** before the put, the directory survives dead (`resumed_dead`), the open is `-ENOENT` and the process lives; `held_matches_old` and `released_after_put` are true | the reference removed at the native `open` (the probe's mutation): refcount 1, `KERNEL PANIC: cwd walk resumed on a freed directory` |
| `cwd-hold-linux`, both passes | the same at the Linux door | the reference removed at `do_open` (which serves the Linux `open` and `openat` both): the same panic; **and** the native mutation alone must leave this test green, which is what says the two doors are separately proved |
| both, the order | the seam released the walk only after the put | the swap half completes before `vnode_put(old)`: `released_after_put` false, the test fails by name |
| both, the poison | the liveness check sees the free | the poison removed with the native mutation kept: the resumed walk reads a freed but intact vnode. The refcount check is expected to catch it anyway (`kobject_release_final` leaves the count at zero), so this row is run to *record* which check fired rather than to predict one; if neither does, the poison is the only thing standing between the proof and the cwd-ref unit's false pass, and the banner says so |

Every pass asserts `held` and `held_matches_old` true, and
`swapper_was_held`, both timeouts and `interrupted` false, so a racer
that never reached the seam — or reached it holding the wrong directory,
or held its own swapper — is a failure and not a pass.

## Benchmarks

None. Nothing here runs in a release build; in a debug build the seam
idle is one load on the relative-path walk, and the poison is one
`memset` per vnode free, on a path that already drops a page cache.
Reported as run in the banner, not promised.

## Risks

- **The seam binds one thread per side.** A racer with a third thread
  calling `chdir` would present a second swapper; the seam refuses it,
  records it, and the test fails by name rather than the second `chdir`
  waiting on a hold that belongs to the first. The racers have two
  threads by construction, and the refusal is what makes that a checked
  property rather than a convention.
- **A `chdir` whose path is relative on the swapper thread** walks
  through the same `walk_parent` branch and must not be held: handled by
  the swapper registration, not by asking the test to use absolute paths
  — though the racer does, because it should.
- **The Linux racer's `clone`** is the milestone-10 shape `lxtest` uses;
  if the freestanding program cannot express pass 2's `unlinkat` and
  `rmdir` simply, the pass is written with the raw syscall numbers the
  `lxabi.h` header already carries.
- **A held thread in a dying racer** leaves at the kill, because the
  wait is killable; the deadline never enters into it. The kernel test
  never kills the racer anyway; it waits for it.
- **CI slowness** cannot make a correct run time out, because neither
  side waits for time: each waits for the other's event. A timeout there
  means an event did not happen, which is the failure it reports.

## Alternatives considered

- **Widen step 4 until it lands.** More threads, more rounds, a loaded
  machine. The cwd-ref unit tried the shape; the window is a few
  instructions against a whole walk, and a probability is not a proof.
  It would also make the user-mode suite slower for everyone to buy a
  chance.
- **Delay in `process_chdir` under a debug knob.** A sleep between the
  swap and the put widens the window for a racing walk to *start* — but
  the walk that matters is one that already loaded the pointer, and a
  delay on the swapper's path slows the swapper, not the reader. The
  deterministic-adversary rule from the balancer unit applies: build the
  adversary from the mechanism.
- **Arm by pid, child polls a sysctl.** The file-fault probe's shape. It
  works at the native door and not at the Linux one, and a proof that
  covers one door of two is the half-enforced check this repository has
  already been bitten by.
- **A seam inside `process_cwd_get()`.** Cannot see the bug-proof, which
  removes the call to it.
- **Leave it.** The fix stands on its construction, as the cwd-ref unit
  argued. So did "only the process itself writes this".

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_014qfcm8FQycZFpYeonUcCz2
