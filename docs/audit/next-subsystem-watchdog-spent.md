# NEXT SUBSYSTEM — the self-test hang watchdog is spent on a passing test

## Problem

`selftest_run_all` arms the scheduler's hang watchdog once, at 8 s, and
kicks it before each test (`kernel/core/selftest.c:982`, `:988`). When
the last kick is older than the period, CPU 0's tick prints a dump:
every run queue, every thread, a sample of every CPU, and a profile of
each busy one.

It prints that dump **once per arming**. `watchdog_check` sets
`g_watchdog_fired`, and only `sched_watchdog_arm` clears it
(`kernel/scheduler/sched.c:816-845`); a kick does not. The lockup report
recorded that the watchdog "fires once"
(`docs/audit/next-subsystem-lockup.md`). What it did not record is where
that one firing goes.

`cosmofs-replay` is test 180 of 409. It mounts and checks 410 prefix
images behind one `SELFTEST` line, in 13 to 20 s, and nothing in it
kicks. So in every debug boot:
- the watchdog fires inside that passing test;
- it stays spent for the rest of the run.

**Every debug log on this machine shows it.** On 2026-09-30, `out/`
held 30 self-test logs from boots before this unit's probe, on both
architectures: 21 chaos boots and 9 plain, warn, no-balancer and
scratch boots. Each holds exactly one `[WATCHDOG] no progress for
800x ms`, and it always falls inside `cosmofs-replay`. That test was at
positions 168 to 218, since the registry has grown and been reordered
across the trees those logs came from. The release logs hold no
self-tests. CI's passing runs do not keep the serial log. Its one failed
run whose log was echoed (36035970200, x86-64 chaos) shows the same dump
in the same test.

### Measured

`tools/watchdog-spent-probe.py` adds one self-test, `wdprobe-sleeper`,
which sleeps 9 s without a kick: a test that stops making progress for
longer than the period. It can be registered in one of two places, and
nothing else about it changes:
- `--late`, just before `syscall-fuzz` (test 403), after `cosmofs-replay`;
- `--early`, just before `cosmofs-replay` (test 180): the control.

`--fix` is the candidate (§1). `read` names the test each dump fell in:
the first `SELFTEST:` line after it.

Every boot below failed the harness, and in every one the only failure
was the sleeper's own budget (`took 900x ms (budget 8000 ms)`), as
intended. Each row is one debug boot, run one at a time.

| arch | sleeper | `--fix` | dumps | the dump fell in | the sleeper got a dump |
|---|---|---|---|---|---|
| x86-64 | late | no | 1 | `cosmofs-replay` (14612 ms) | **no** |
| x86-64 | early | no | 1 | `wdprobe-sleeper` (test 180) | yes; `cosmofs-replay` then got none |
| aarch64 | late | no | 1 | `cosmofs-replay` (20458 ms) | **no** |
| x86-64 | late | yes | 1 | `wdprobe-sleeper` (test 403) | yes |
| x86-64 | early | yes | 1 | `wdprobe-sleeper` (test 180) | yes |
| aarch64 | late | yes | 1 | `wdprobe-sleeper` (test 403) | yes |

The watchdog does report the sleeper, but only while it has not yet
fired. Which test it reports is decided by the order of the registry.
With the candidate, it reports the sleeper wherever it sits, and
`cosmofs-replay` passes without a dump.

### Why it matters

1. **229 tests run with no hang watchdog.** These are every test after
   position 180:
   - all 48 `net-*` tests;
   - the 34 `el2-*` and 13 `hv-*` guest tests;
   - the 12 `process-*`, 12 `signal-*` and 9 `tty-*` tests;
   - 61 of the 65 `cosmofs-*` tests;
   - `process-user` (the whole user-mode suite) and `syscall-fuzz`.

   A hang in any of them reaches the harness's 180 s timeout with no
   dump. `docs/testing/flakes.md` removed the bounds that "named a hang"
   on the grounds that "the self-test watchdog (8 s, with a scheduler
   dump) ... reports" them. For these 229 tests, nothing does.
2. **All five over-budget sightings ran after it**, so none of them
   recorded what its test was doing at 8 s:
   - `net-accept-race` (test 279), once;
   - `net-nicbench` (295), twice;
   - `syscall-fuzz` (403), twice.

   Each is "recorded, not attributed" in `flakes.md`. The dump is the
   record the watchdog exists to supply, and the probe shows it would
   have been printed.
3. **Every passing debug log carries a hang report.** The dump sits in
   the middle of `cosmofs-replay`'s output. `docs/development.md` tells
   the reader to treat `[WATCHDOG] no progress` as a test that stopped
   making progress.
4. **One number, two places.** The harness's per-test budget is "the
   hang watchdog's period" (`docs/verification/design.md` §6). But the
   composite budgets (`process-user` 20 s, `cosmofs-replay` 40 s) exist
   only in `run_boot_test.py`, so the kernel's watchdog holds those two
   tests to 8 s. `docs/verification/api.md` states the composites as
   "20 s each"; the harness has 40 s for `cosmofs-replay`.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| arming | `selftest.c:982` | `sched_watchdog_arm(8 s)` once, before the first test |
| per test | `selftest.c:988` | `sched_watchdog_kick()`: moves the last-kick time; the latch is untouched |
| the check | `sched.c:833-862` | CPU 0's tick: armed, not fired, and no kick for the period, then dump and set `g_watchdog_fired` |
| in-test kicks | `nettest.c:884, 899, 1561, 2153, 2342` | long waits in network tests mark progress |
| budgets | `run_boot_test.py:797-847` | 8000 ms (`SELFTEST_BUDGET_MS`), plus `composite_budget_ms` for two tests |

## Design

### 1. The runner arms the watchdog for each test, at that test's budget

The per-test `sched_watchdog_kick()` becomes
`sched_watchdog_arm(budget)`. Arming clears the latch, so:
- the watchdog fires at most once **per test**;
- a test that fires it cannot spend it for the tests after it.

The period is the test's budget. A test that goes quiet for longer than
it is allowed to take is dumped, at the point the harness would fail it.
A composite test that passes within its budget is not dumped.

The in-test kicks stay. They mark progress in waits that end, so a
kicking test is dumped later than its budget or not at all. The harness
still fails that test on its duration. The rule is: **the dump belongs
to the test that is running, and says what it was doing when it went
quiet.**

### 2. The budget lives in the registry, and the harness reads it from the boot

- `struct selftest` gains `budget_ms`; 0 means the default,
  `SELFTEST_BUDGET_DEFAULT_MS` (8000).
- `process-user` gets 20000 and `cosmofs-replay` 40000, with the
  harness's comment on why moved beside them.
- Before the first test, the runner prints one line:
  `SELFTEST: budgets default=8000 process-user=20000 cosmofs-replay=40000`.
- The harness takes its budgets from that line and drops
  `composite_budget_ms`. A self-test boot without the line fails
  (`missing budgets line`), so a stale harness cannot quietly fall back
  to its own numbers.
- `SELFTEST_BUDGET_MS` is removed. The kernel's watchdog cannot see it,
  so an override would split the one number again. Nothing sets it
  (a grep over the tree and CI finds only its documentation).

### 3. A passing boot prints no dump

`\[WATCHDOG\] no progress` joins `FORBIDDEN_MARKERS`. Under §1 and §2 a
dump means some test was quiet for its whole budget, and that test
fails its budget anyway. The marker adds one thing: a regression
(`cosmofs-replay` held to 8 s again, or the per-test arm reverted to a
kick) now fails every debug boot instead of hiding in it.

## Affected files

| file | change |
|---|---|
| `kernel/core/selftest.c` | `budget_ms` in the registry (two entries set); the budgets line; per-test `sched_watchdog_arm` |
| `kernel/include/kernel/selftest.h` | `struct selftest` gains `budget_ms`; `SELFTEST_BUDGET_DEFAULT_MS` |
| `kernel/scheduler/sched.c`, `kernel/include/kernel/sched.h` | the header's comment ("prints ... once") says once per arming; the fire count, the state read and the quiet arming for the two tests (§Tests) |
| `tests/boot/run_boot_test.py` | budgets from the boot's line, `composite_budget_ms` and `SELFTEST_BUDGET_MS` removed, the forbidden marker |
| `docs/verification/design.md` §6, `api.md`, `invariants.md` F6, `testing.md`; `tests/README.md`; `docs/development.md` | where the budgets live, "20 s each" corrected, the watchdog per test |
| `docs/testing/flakes.md` | the hang-bounds paragraph's claim now holds for every test; the five budget entries point here |
| `README.md` | Status entry |

## APIs

- Kernel, for the two self-tests:
  - `sched_watchdog_fire_count()`: how many times the watchdog has
    fired;
  - `sched_watchdog_state(uint64_t *period_ns, bool *fired)`: the
    current arming;
  - `sched_watchdog_arm_quiet(period_ns)`: an arming whose firing is
    counted, not printed.
- Nothing is added for userland. The boot log gains one line.

## Tests

| test | proves |
|---|---|
| `watchdog-spend` (new self-test) | arms a 50 ms period in quiet mode (the dump counted, not printed) and sleeps 150 ms: the fire count rose by exactly one, so the watchdog fires once per arming, not once per period. It then **returns with the watchdog fired**, on purpose, and does not re-arm it. |
| `watchdog-rearm` (new self-test, registered directly after `watchdog-spend`) | at entry, before it calls anything that arms, reads `sched_watchdog_state()`: not fired, and the period is its own budget (8000 ms). Only the runner can have made that true. A runner that kicks instead of arming leaves `watchdog-spend`'s fired state (and its 50 ms period) in place, and this test fails. |
| the forbidden marker | a passing boot has no dump; the plain boot today would fail it |
| the budgets line | the harness fails a boot whose self-tests printed no budgets line |
| `tools/watchdog-spent-probe.py --late` on the build | the sleeper's dump falls in the sleeper, test 403 |

**The pair, not one test, is what checks the runner.** A single test
that arms the watchdog itself clears the fired state whatever the runner
does, so it passes under a kick-only runner (Greptile, #266).
`watchdog-spend` leaves the watchdog fired, and only the runner's arm
between the two tests clears it. The check does not depend on
`cosmofs-replay` still taking longer than 8 s.

The quiet mode is a test-only flag read by `watchdog_check`, and
`sched_watchdog_arm` clears it. It exists so the pair does not print a
real dump into every boot, which would reintroduce the noise the unit
removes.

**Planned mutations** (each alone, both architectures, boot confirmed):
- the per-test arm back to a kick: `watchdog-rearm` fails at entry, and
  the forbidden marker fires in `cosmofs-replay`;
- the runner arming every test at 8 s, ignoring `budget_ms`:
  `watchdog-rearm` still passes (its budget is the default), and the
  forbidden marker fires in `cosmofs-replay`;
- `cosmofs-replay`'s budget back to the default: the forbidden marker
  fires in every boot;
- the harness ignoring the budgets line: the boot fails `cosmofs-replay`
  at 8000 ms.

## Benchmarks

None. The dump's cost inside `cosmofs-replay` was not isolated.
`cosmofs-replay` ran 12.5 to 20.5 s across the six boots, with and
without the dump, which is noise at one-minute host loads of 4.6 to 23. No speedup
is claimed.

## Risks

- **A kicking test gets its dump late.** The five in-test kick sites
  mark progress, so a network test stuck after a kick is dumped one
  period after its last kick, not at its budget. The harness's budget
  still fails it. Removing the kicks is not proposed: each marks a wait
  that ends.
- **A composite test is watched at 20 s and 40 s,** not 8 s. That is
  what it is allowed to take. Both budgets sit far under the 180 s boot
  timeout.
- **The forbidden marker will fail boots the budget already fails**
  (a quiet test over its budget). A failure then has two lines where it
  had one. The marker's point is the regression it catches.

## Alternatives considered

- **Kick inside `cosmofs-replay`, once per image.** This removes this
  test's dump, but the next long test would spend the latch in the same
  way. The latch outliving the test that fired it is the defect, and
  this leaves it.
- **Clear the latch on every kick.** In-test kicks would then re-enable
  it mid-test, and a test that repeatedly goes quiet between kicks would
  print a dump each time. Once per test is the unit of report.
- **Raise the period above 20 s.** That hides the composites' dumps and
  also every ordinary test's. A hang in an 8 s test would be reported
  12 s later than the harness fails it, and after `cosmofs-replay` it
  would still not be reported.
