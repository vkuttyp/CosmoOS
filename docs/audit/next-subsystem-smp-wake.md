# NEXT SUBSYSTEM — smp-wake posts while the waiter is still running

> **Status: proposed.** Report and probe (`tools/smp-wake-probe.py`)
> only; nothing in the kernel changes in this PR.

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
- **`--fix`**: the candidate (§1).

| run | arch | target at the wake | reschedule requested | IPIs on the target | `smp-wake` |
|---|---|---|---|---|---|
| unforced | both | idle | yes | +1 | ok |
| `--window 20` | both | **the waiter itself** | no | **+0** | **FAIL** at the check, the sighting's message |
| `--busy` | both | `swp-busy`, prio 32 | no | +1 (not this wake's; likeliest the spinner's creation) | ok, **for the wrong reason** |
| `--window 20 --fix` | both | idle | yes | +1 | ok |
| `--fix` | both | idle | yes | +1 | ok |

Each row is one boot per architecture.

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

- **Wait for the target to be idle.** After `BLOCKED`, the test also
  waits until the target reads idle: `sched_cpu_load(target) == 0`,
  bounded at 1 s. A load of 0 cannot be read while the waiter is still
  current, because a current thread counts 1. The hint can read one
  high (S29), never low, so 0 is a sound observation that the waiter is
  off the CPU.
- **The wake records its decision on the woken thread**, as the
  hysteresis unit's pull record does. In debug builds, `sched_wake`
  writes `t->wake_resched`, whether it called `request_resched`, under
  the target's lock.
- **The assertion becomes exact:** the wake requested a reschedule
  *and* the target counted an IPI. A wake that requested none is not a
  pass whatever the count did.
- **Retry for a busy target.** If the target became busy between the
  idle read and the post, the record says no reschedule was requested.
  That round is retried, up to 5 rounds with a fresh waiter each, and
  the test fails only if no round's wake requested one, or if one did
  and no IPI arrived.

### 2. The record

- **flakes.md:** the entry gains "explained" (this PR) and "fixed by"
  (the implementation).
- **`docs/kernel/scheduler/testing.md`:** the test's claim, and why it
  waits for idle and reads the wake's record.
- **README Status entry.**

## Affected files

The implementation's. This PR adds the probe, this report, and the
explanation under the flakes entry.

| file | change |
|---|---|
| `kernel/include/kernel/thread.h` | `wake_resched` |
| `kernel/scheduler/sched.c` | record it in `sched_wake` (debug) |
| `kernel/scheduler/smptest.c` | the idle wait, the record-based assertion, the retry |
| `docs/kernel/scheduler/testing.md`, `docs/testing/flakes.md`, `README.md` | as above |

## APIs

None. One `struct thread` field, written only in debug builds.

## Tests

| test | proves |
|---|---|
| `smp-wake` | a cross-CPU wake of a thread blocked on an idle CPU requests a reschedule, and the target handles the IPI |

**Planned mutations** (each alone, both architectures, boot confirmed):

| mutation | expected |
|---|---|
| `request_resched` sends no IPI | fails: requested, no IPI |
| `sched_wake` never calls `request_resched` | fails: no round's wake requested one |
| the idle wait removed, under the probe's `--window` | fails: no round requested, since the waiter is current every time |
| the record not written | fails: no round's wake requested one |

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
