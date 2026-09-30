# NEXT SUBSYSTEM — thrtest step 25 asserts which of two threads reached the mutex first

> **Status: built (PR #265).** As designed, with these specifics:
>
> - **Three runs** in step 25's loop (`run` 0, 1, 2; phase 0, 1, 1).
>   `bp_force_contend` turns on the third run's wait, and
>   `bp_forced_reached` records whether it reached 2. The plain phase-1
>   run prints what it read: "the woken waiter first", or "the holder's
>   unlock first, most likely".
> - **Measured:** both architectures passed. The plain phase-1 run read 1
>   (the holder's unlock first, most likely), and the forced run reached
>   2.
> - **Mutations,** each alone, both architectures, boot confirmed:
>
>   | mutation | result |
>   |---|---|
>   | the forced run's assertion back to `== 1` | fails on both: `bp_state_seen == 1u` (it reads 2) |
>   | phase 0's assertion widened to 1 or 2 | survives on both, as planned: nothing makes phase 0 read 2. Recorded as such, not as a catch |
>   | the forced run's wait removed | fails on both, twice: `bp_forced_reached == 1u` and `bp_state_seen == 2u`. A missing wait is not a silent pass |
>   | `cosmo_cond_wait` relocking with `cosmo_mutex_lock` | `thrtest` hangs at step 20 on both, before step 23 or 25, and the boot times out. The plan said step 23 catches the rule first; an earlier step does, so step 25's dependence on it is not measurable this way |
> - **The docs:** `docs/libc/testing.md` row 25, `docs/libc/invariants.md`
>   and `docs/kernel/process/testing.md`'s summaries of steps 23 to 30,
>   flakes (fixed by), and README.

## Problem

`docs/testing/flakes.md` ("`thrtest` step 25: the mutex word was not 1
at the broadcast's probe") records one sighting. It came from CI run
36623173528, aarch64 debug, on PR #262, which changes no code:

```
thrtest: step 25
thrtest: FAIL bp_state_seen == 1u at line 2424
THREADTEST: FAIL 1
```

**What step 25 does.**
1. A holder thread locks `cv_m` uncontended, and three waiters sleep on
   `cv_c`.
2. Main broadcasts without the mutex, with libc's
   `__cosmo_cond_bcast_probe` running inside `cosmo_cond_broadcast` at
   phase `at`. Phase 0 is after `seq` moved and before the requeue.
   Phase 1 is after the requeue.
3. The probe reads `cv_m.state`, tells the holder to unlock, and waits
   until it has.
4. Every waiter must return.

The check that failed asserts that the probe read 1 (held, uncontended)
at both phases.

**At phase 1, 1 is not the only correct answer.** The requeue
(`cosmo_futex_requeue(..., 1, ~0u, seq)`) wakes one waiter and moves the
rest onto the mutex word. The woken waiter "relocks at 2 whatever the
word says", as the step's own comment puts it. The holder still has
`cv_m`, so the waiter marks it contended (2) and sleeps.

If that waiter reaches `cv_m` before the probe reads the word on the
broadcasting thread, the word reads 2 on a correct libc. The holder's
unlock then finds 2 and wakes it, which is the handoff working. The
step's comment says the holder's unlock "finds 1 ... either way". That
holds at phase 0, where nobody has been woken, and is a race at phase 1.

### Measured

`tools/cond-phase-probe.py` prints each iteration's phase and the value
the probe read (CPPROBE lines from the user-mode suite). `--force` makes
the phase-1 probe wait, yielding and bounded at 1 s, until the word
reads 2 before it reads it. A wait that times out fails the step instead
of passing untested; the probe prints whether it reached 2. That makes the woken waiter reach the held
mutex first: the order the sighting needs, made certain. `--fix` is the
candidate (§1).

| run | arch | phase 0 read | phase 1 read | step 25 | the user-mode suite |
|---|---|---|---|---|---|
| instrumented, unforced | both | 1 | 1 | ok | PASS |
| `--force` | both | 1 | **2** | **FAIL** `bp_state_seen == 1u` (the sighting's check) | FAIL 1 |
| `--force --fix` | both | 1 | 2 | ok | PASS |

Each row is one boot per architecture.

- **The sighting's check fails deterministically** once the woken waiter
  is first, on both architectures. Phase 0 reads 1 every time, so the
  sighting's failure was the phase-1 iteration, not attributable from
  its log.
- **Under `--force`, every waiter still returns** (the suite passes
  with the fix), so the handoff through "the unlock finds 2" works.
- **The two unforced boots read 1 at phase 1.** The probe reads the
  word just before it tells the holder to unlock, so the unlock most
  likely found 1 and woke nobody; the waiter could still arrive between
  the read and the unlock. That is the path the step exists to guard,
  and it is the usual order, not a certain one.

### Why it matters

The check fails a correct libc in the waiter-first order. And because
the order is a race, the step does not know which handoff path it
exercised on a given boot.

## Current implementation

- **`libc/src/thread.c`, `cosmo_cond_broadcast`:** increments `seq`,
  probe phase 0, requeue (wakes one, moves the rest onto the mutex
  word), probe phase 1.
- **`userland/tests/thrtest.c`, step 25:** `bp_holder`, `bp_probe`; the
  loop over `at` in {0, 1}.
- **`docs/libc/testing.md` row 25, and the step's comment:** "the
  holder's unlock finds 1".

## Design

### 1. Assert what each phase allows, and make the waiter-first order certain

- **Phase 0 must read 1.** Nobody has been woken, and nothing but the
  holder touches the word.
- **Phase 1 as it stands** accepts 1 or 2, and logs which. The word is
  held either way, and the holder's unlock must carry the handoff
  whichever order the two threads took.
- **A third run takes the waiter-first order for certain.** It is
  phase 1 again, with the probe waiting (bounded at 1 s, yielding) until
  the word reads 2 before it reads it, as `--force` does. It asserts
  that the wait reached 2 (a timeout is a failure, not an untested
  pass), and that every waiter returns: the unlock finds 2 and wakes
  the woken waiter.
- **The other order, the unlock finding 1, is not guaranteed.** It is
  the usual one: both unforced boots read 1 at the probe. But the plain
  phase-1 iteration can take either order on a given boot. Even a read
  of 1 does not prove it, since the waiter can arrive between the read
  and the unlock. Its coverage on any one boot is therefore uncertain;
  the logged value shows which order the read saw. Making it certain
  would need a seam in libc to hold the woken waiter, which this report
  does not propose. Greptile named this on #264.

### 2. The record

- **The step's comment and `docs/libc/testing.md` row 25:** what each
  phase reads, and why.
- **flakes.md:** the entry gains "explained" (this PR) and "fixed by"
  (the implementation).
- **README Status entry.**

## Affected files

The implementation's, as built (PR #265). The report (#264) added the
probe, this report, and the explanation under the flakes entry.

| file | change |
|---|---|
| `userland/tests/thrtest.c` | step 25: per-phase assertions, the third run (`bp_force_contend`, `bp_forced_reached`), both comments |
| `docs/libc/testing.md`, `docs/libc/invariants.md`, `docs/kernel/process/testing.md`, `docs/testing/flakes.md`, `README.md` | as above |

No kernel or libc change.

## APIs

None.

## Tests

| test | proves |
|---|---|
| `thrtest` step 25 | a holder's unlock inside a broadcast hands off to every waiter: before the requeue; after it, in whichever order the threads took (usually the unlock first, not guaranteed); and after it with the woken waiter first, made certain |

**Mutations** (each alone, both architectures, boot confirmed): the
banner's table. The plan predicted the relock mutation's effect on step
25's runs: the forced run unaffected, since a held word is contended at
2 either way; the plain phase-1 run hanging in its usual order. Neither
could be observed, because `thrtest` hangs at step 20 first.

## Benchmarks

None.

## Risks

- **The third run's wait is bounded at 1 s.** If the woken waiter never
  reaches the mutex in that time, `bp_forced_reached` stays 0 and the
  run fails ("the wait did not reach 2"). That is a stuck handoff too,
  not an untested pass.

## Alternatives considered

- **Drop the phase-1 value check.** The step would still check the
  handoff, but a run would no longer say which order it took, and the
  waiter-first order would stay a race.
- **Hold the woken waiter in libc to force the unlock-finds-1 order.**
  That needs a new seam in `cosmo_cond_wait` for a path the unforced
  runs already take. Not proposed.
