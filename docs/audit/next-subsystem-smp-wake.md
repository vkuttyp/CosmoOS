# NEXT SUBSYSTEM — smp-wake posts while the waiter is still running

> **Status: built (PR #263).** As designed, with these specifics:
>
> - **The fields** `wake_resched` and `wake_ipi_base` exist in every
>   build and are written only in debug, as the hysteresis unit's pull
>   record is. `ipi_count_on(cpu, kind)` is declared in `ipi.h`. The test
>   skips in a non-debug build, where nothing writes the record.
> - **The waiter reads its own record** (`thread_current()`) when it
>   returns, so the test never touches a joined thread.
> - **One wait loop, for both conditions** (`BLOCKED` and the target
>   idle), bounded at 1 s. Its verdict is checked after the join, so a
>   timeout never leaves a waiter behind.
> - **Measured:** both architectures passed in round 1, with one
>   reschedule IPI after the wake's snapshot and the wake seen 83–98 µs
>   after the post.
> - **Mutations,** each alone, both architectures, boot confirmed:
>
>   | mutation | result |
>   |---|---|
>   | no IPI in `request_resched` | fails at the count (`cw.ipis_after > cw.ipi_base`) |
>   | `request_resched` never called | fails at the count. The plan expected "no round requested"; the record keeps the decision, which this mutation leaves intact, and only the call goes |
>   | the idle wait removed, the waiter held 20 ms between `BLOCKED` and its switch | fails: "no round's wake found the target idle" |
>   | the record not written | fails: "no round's wake found the target idle" |
>
>   With the idle wait in place, the same 20 ms hold passes on both
>   architectures.
> - **The testing guide's old claim was out of date.** It said nothing
>   else notices a missing IPI. With no IPI, `process-user` also misses
>   its 15 s bound on both architectures, and on x86-64 `mutex`,
>   `process-spawn` and `hid-keyboard` also fail. The guide is corrected.
> - **The docs are the SMP ones** (`docs/kernel/smp/testing.md`,
>   `invariants.md` SMP9), where `smp-wake` lives, not the scheduler's.

## Problem

`docs/testing/flakes.md` ("`smp-wake`: no reschedule IPI counted on the
target") records one sighting, on CI run 35901262531 (x86-64,
2026-09-23):

```
SELFTEST: smp-wake ... FAIL: check failed: cw.ipis_after > cw.ipis_before at line 454 (2 ms)
```

**What the test does.**
1. It pins a waiter to another CPU. The waiter reads the target's
   reschedule-IPI count, blocks on a semaphore, and reads the count
   again when it returns.
2. The test polls until the waiter's `state` reads `THREAD_BLOCKED`,
   then posts.
3. It asserts that the count rose: the wake interrupted the target.

**The kernel owes that IPI only sometimes.** `sched_wake` calls
`request_resched`, which sends `IPI_RESCHEDULE`, only when the target is
running its idle thread or something of lower priority.

**Two ways the test can post when none is owed.**

1. **The waiter is still the target's current thread.**
   - `waitqueue_prepare` sets `THREAD_BLOCKED` under the wait-queue lock
     and returns. The waiter switches out later, in
     `sched_block_current`.
   - A post in between finds `rq->current` is the waiter itself, at
     equal priority, so no IPI is sent.
   - `schedule_internal` then finds the waiter already `READY` ("woken
     between blocking and reaching here") and runs it straight on. It
     never slept, so nothing needed interrupting.
   - The test's poll wakes on a tick. That usually lands after the
     switch, and rarely inside this microsecond window.
2. **Another thread of equal priority is running on the target**: the
   candidate flakes.md named. The post enqueues the waiter behind it,
   with no IPI.

The first is the one the probe reproduces with the sighting's exact
message. The sighting's log records neither what the target ran nor
what the wake decided, so which of the two it was is not known.

**The count is also a superset.** The waiter's two reads span its whole
block, so *any* reschedule IPI to that CPU in between passes the check,
including one owed to another thread's wake. Under the probe's `--busy`,
the wake requested nothing, and the count still rose (96 → 97 on
aarch64, 77 → 78 on x86-64). The likeliest source is the spinner's own
creation: `sched_enqueue_new` onto the then-idle target requests a
reschedule. The probe does not record which IPI arrived. Either way, the
check passed for a wake that sent no IPI.

### Measured

`tools/smp-wake-probe.py` records, at the waiter's wake and under the
target's run-queue lock:
- what the target was running, and whether that was its idle thread or
  the waiter itself;
- whether `request_resched` was called.

Its modes:

- **`--window MS`**: holds the waiter MS ms between setting `BLOCKED`
  and switching out (a spin in `waitqueue_prepare`, for the thread
  named `cross-waiter` only).
- **`--busy`**: runs a spinner at the waiter's priority on the target
  through the post.
- **`--fix`**: the candidate (§1). The target is read idle first,
  before any `--busy` spinner exists, so a timeout leaves nothing running.
  Then the wake must have requested a reschedule, and the target must
  have handled one after the wake. That is counted from a snapshot of
  the target's handled count, taken in `sched_wake` under the target's
  lock, not from the waiter's read before it blocked.

| run | arch | target at the wake | reschedule requested | IPIs on the target | `smp-wake` |
|---|---|---|---|---|---|
| unforced | both | idle | yes | +1 | ok |
| `--window 20` | both | **the waiter itself** | no | **+0** | **FAIL** at the check, the sighting's message |
| `--busy` | both | `swp-busy`, prio 32 | no | +1 (not this wake's; likeliest the spinner's creation) | ok, **for the wrong reason** |
| `--window 20 --fix` | both | idle | yes | +1 from the wake's snapshot | ok |
| `--fix` | both | idle | yes | +1 from the wake's snapshot | ok |
| `--busy --fix` | both | `swp-busy`, prio 32 | no | the block-to-return count rose (76 → 77, 97 → 98); **from the wake's snapshot, +0** | FAIL, cleanly: the only failure in the boot, the spinner joined |
| the no-IPI mutation, `--fix` | both | idle | yes | +0 | FAIL |

Each row is one boot per architecture.

The `--busy --fix` row is the superset, caught: the IPI that raised the
old count had arrived before the wake, so a count from the wake's
snapshot does not include it. The first `--fix` asserted "requested and
the block-to-return count rose", which that IPI would still have passed.
Greptile found it on #262.

**Does the test catch its target defect?** Yes, measured with a
mutation: `request_resched` sending no IPI at all fails `smp-wake` on
both architectures (x86-64 line 455, aarch64 line 455). The superset
did not hide it there, but `--busy` shows that it could.

### Why it matters

It is a false failure of the self-test suite, and the check behind it
can also pass without the event it asserts.

## Current implementation

- **`kernel/scheduler/wait.c`, `waitqueue_prepare`:** `BLOCKED` set
  under the wait-queue lock; the switch comes later.
- **`kernel/scheduler/sched.c`, `sched_wake`:** enqueues, and calls
  `request_resched` if the target is idle or running lower priority.
  `request_resched` sets `need_resched` and sends the IPI cross-CPU.
- **`kernel/scheduler/smptest.c`, `selftest_smp_wake_pinned`:** the
  test above.
- **`kernel/interrupt/ipi.c`:** counts handled IPIs per CPU and kind.

## Design

### 1. Post to an idle target, and read what the wake decided

- **Wait for the target to be idle.** The test waits until the waiter
  reads `BLOCKED` *and* the target reads idle, `sched_cpu_load(target)
  == 0`, in one loop bounded at 1 s, checked after the join. A load of 0 cannot be read while the waiter is still
  current, because a current thread counts 1. The hint can read one
  high (S29), never low, so 0 is a sound observation that the waiter is
  off the CPU.
- **The wake records its decision on the woken thread**, as the
  hysteresis unit's pull record does. In debug builds, `sched_wake`
  writes `t->wake_resched`, whether it asks for `request_resched`, under
  the target's lock. The fields exist in every build. The waiter reads
  them from `thread_current()` when it returns, and the test skips in a
  non-debug build.
- **The wake also snapshots the target's handled reschedule count**,
  under the same lock, before it sends (`t->wake_ipi_base`).
- **The assertion:** the wake requested a reschedule *and* the target
  handled one after the snapshot. A wake that requested none is not a
  pass whatever the count did. An IPI the target handled before the wake
  no longer counts.
- **One residue remains.** Another CPU's reschedule IPI to the same
  target, sent between this wake and the waiter's return, is not told
  apart from this wake's. The window is the wake's own latency, and a
  lost IPI would also have to coincide with it.
- **Retry for a busy target.** If the target became busy between the
  idle read and the post, the record says no reschedule was requested.
  That round is logged and repeated, up to `SMP_WAKE_ROUNDS` (5) with a
  fresh waiter each. The test fails if no round's wake requested one, or
  if one did and no IPI arrived after its snapshot.

### 2. The record

- **flakes.md:** the report (#262) explained the entry; the
  implementation adds "fixed by the smp-wake unit (PR #263)".
- **`docs/kernel/smp/testing.md`:** the test's claim, and why it waits
  for idle and reads the wake's record, with the mutations. It also
  corrects the old "nothing else notices" claim.
- **`docs/kernel/smp/invariants.md`:** SMP9's check.
- **README Status entry.**

## Affected files

The implementation's, as built (PR #263). The report (#262) added the
probe, this report, and the explanation under the flakes entry.

| file | change |
|---|---|
| `kernel/include/kernel/thread.h` | `wake_resched`, `wake_ipi_base` |
| `kernel/interrupt/ipi.c`, `kernel/include/kernel/ipi.h` | `ipi_count_on(cpu, kind)`: another CPU's handled count |
| `kernel/scheduler/sched.c` | record both in `sched_wake` (debug) |
| `kernel/scheduler/smptest.c` | the idle wait, the record-based assertion, the retry, the non-debug skip |
| `docs/kernel/smp/testing.md`, `docs/kernel/smp/invariants.md`, `docs/testing/flakes.md`, `README.md` | as above |

## APIs

`ipi_count_on(cpu, kind)`, a read of another CPU's handled count. Two
`struct thread` fields, written only in debug builds.

## Tests

| test | proves |
|---|---|
| `smp-wake` | a cross-CPU wake of a thread blocked on an idle CPU requests a reschedule, and the target handles the IPI |

**Mutations** (each alone, both architectures, boot confirmed): the
banner's table.

## Benchmarks

None. `sched_wake` gains one store in debug builds, under a lock it
holds.

## Risks

- **A target that is never idle for a second** skips nothing and fails
  the idle wait. That would be a real finding (something pinned there
  never sleeps), not noise.
- **Retries could hide a rare defect.** They are bounded, and each
  retried round is logged with what the target ran.

## Alternatives considered

- **Count the IPI at the sender.** `ipi_send` succeeding says nothing
  about the target handling it; the claim is the target's.
- **Hold the waiter's CPU idle by pinning everything else away.** Not
  possible for kernel threads that are per-CPU by design.
- **Drop the IPI claim and time the wake.** That was the previous form
  (`woke_at - sent < 2 ms`). A tick inside the window passes it, and a
  held vCPU fails it; this report's own flakes history is why it went.
