# NEXT SUBSYSTEM — a test's process outlives the test, and the next test counts it

> **Status: built (PR #251).** As designed, with these specifics:
>
> - `process_present`, `process_table_pids` and `process_describe`
>   (`kernel/process/process.c`); the reap hold is `process_test_hold_reap`,
>   `process_test_reap_held`, `process_test_release_reap` and the reaper's
>   side `process_test_reap_hook`, called from `thread_put` after
>   `process_last_thread_gone` in debug builds. It parks on a completion,
>   armed by a compare-and-swap on the pid.
> - `run_module` is `run_module_hooked` without a hook. It waits for its
>   own pid with 1 ms sleeps for up to 2 s, and on failure logs "its process
>   (pid N) was not released".
> - The runner's check is `selftest_leftover_processes(wait_ns, pids,
>   max)` (`kernel/core/selftest.c`), which decides and returns one locked
>   snapshot (`process_table_pids`), so the count and the pids agree. The
>   runner logs up to sixteen of the processes left
>   (`selftest: <test> left pid N 'name' (state, references, threads)`),
>   counts any beyond that, and fails the test with "a process it spawned outlived it (P33)". The
>   check itself logs nothing, because `process-leftover-named` finds a
>   process on purpose.
> - `process-gone-order` and `process-leftover-named` are registered after
>   `process-reaped`, share `spawn_held_zombie`, and skip in release
>   builds, like the other hold tests.
> - All 37 `elf_settle_processes` calls, and the helper, are gone.
> - Measured: every boot passes on both architectures (debug, release,
>   the aarch64 GIC boot, chaos). The new tests take 29–57 ms. One x86
>   chaos boot failed `sched-migrate-refuses` (its spinner refused as
>   "affinity"; no process involved) and passed on the rerun; it is
>   recorded in `flakes.md`.
>
> Mutations, each alone, with its boot confirmed:
>
> | mutation | result |
> |---|---|
> | `run_module` back to the count | `process-gone-order` FAIL, "the process it ran was not released", 2039 ms x86-64, 2052 ms aarch64 |
> | the runner's check finds nothing | `process-leftover-named` FAIL: "the check found 0 ... wanted pid 166" |
> | the hold does not wait | both new tests FAIL: "the held process had already left the table" |
> | `process_present` always false | both FAIL the same way. The same boot failed four host-networking tests (`net-hostinput`, `net-hoststate`, `net-output`, `net-flows-fw`), which run long before any process test and do not use `process_present`; three of them are the family recorded in `flakes.md` |
> | pid 8 (`dev-tty`'s) released 2.5 s late | **`dev-tty` FAIL**, "a process it spawned outlived it (P33)", with `dev-tty left pid 8 'init' (state 2, 1 references, 0 threads)`; **`dev-tty-none` passes**. The failure moves to the test that left the process |

## Problem

`run_module` (`kernel/process/proctest.c`) spawns `init` with a probe's
arguments, waits for it to exit, drops its reference, and then requires
the process table to come back to where it started:

```c
unsigned before = process_count();
...
while (process_count() != before && !clock_deadline_passed(deadline))
    sched_yield();
CHECK(process_count() == before);            /* proctest.c:254 */
```

That line has failed three times, in three different tests:

| when | where | test | the test before it |
|---|---|---|---|
| 2026-09-28 | CI run 36389174994 (`439be9fb`), aarch64 debug | `dev-tty-none ... FAIL ... line 254 (2107 ms)` | `dev-tty` |
| 2026-09-24 | local, aarch64 debug | `signal-group ... FAIL ... line 254 (2477 ms)` | `process-reaped` |
| 2026-09-22 | local, chaos boot, at the old 500 ms bound | `tty-isatty ... process_count() == before` | `tty-nosig` |

`docs/testing/flakes.md` recorded each as "a settle on an exact count"
and, for the last, widened the bound. The bound was not the problem.

### The mechanism

`process_count()` counts every process on the machine. The check holds
only if no *other* process is created or released between `before` and
the check. The CI log of the first sighting shows one that was:

```
[ INFO] process: pid 8 'init' created, entry 0x0000000000400190, 3 segments
[ INFO] process: pid 8 'init' exited with status 0 (12 syscalls)
SELFTEST: dev-tty          ... ok (20 ms)
[ INFO] process: pid 9 'init' created, entry 0x0000000000400190, 3 segments
[DEBUG] process: pid 8 'init' released
[ INFO] process: pid 9 'init' exited with status 0 (8 syscalls)
[DEBUG] process: pid 9 'init' released
SELFTEST: dev-tty-none     ... FAIL: check failed: process_count() == before at line 254 (2107 ms)
```

`dev-tty` passed before its process was released. `dev-tty-none` then
took `before` with pid 8 still counted (1). Pid 8 was released inside
its window, and its own pid 9 after it, so the count settled at 0 and
could never equal `before` again. The wait ran to its 2 s deadline.

A process exits and is released in two steps. The reaper thread
(`kernel/scheduler/thread.c`) runs `process_last_thread_gone`, which
marks the process `EXITED` and completes `p->exited`: that is what a
waiter wakes on. Then it drops the exited thread's reference to the
process. The release (`process_release`, which tears down the address
space and only then leaves the table) runs when the last reference
drops. That may be the waiter's `process_put` or the reaper's.
A test that waits for the exit and puts its reference can therefore
return while the reaper still holds the process. Its process is then
released during whatever test comes next.

The same check also **passes when it should not**. If the leftover
leaves first and the test's own process is still held, the count is
back at `before` with the wrong process gone.

### Measured

`tools/proc-settle-probe.py` instruments every `run_module` call: the
table at `before` and at the check, the settle time, the stages of the
release and who ran it. After every test it also logs any process still
in the table. It can also delay the reaper's reference drop, for every
process or for one pid. Every boot below is debug, one per architecture
per row unless the row says otherwise, with the boot confirmed.

| run | aarch64 | x86-64 |
|---|---|---|
| instrumented, 3 boots each (96 calls) | pass; table empty at all 96 `before`s; settle 11–52 ms (median 20), 126–139 ms for `init --selftest`; the release run by `kmain`, the test itself, in all 64 calls timed | pass; empty at all 96; 6–152 ms (median 15), 133–136 ms; `kmain` in all 64 |
| where a release goes | nearly all in `vm_space_destroy`: 12–29 ms (2128–3152 mapped pages), 128 ms for `--selftest` (2144) | 5–28 ms (2126–3150), 133 ms (2142) |
| no process left at any test's end | yes | yes |
| **pid 8's drop delayed 50 ms** (the CI order, forced) | **`dev-tty-none` FAIL, 2117 ms**; the log in the CI order line for line | **FAIL, 2094 ms**; the same order |
| every drop delayed 30 ms | the 12 tests below end with their process in the table; `tty-isatty` and others pass **by substitution** (a leftover gone, their own still held); `process-rlimit` FAIL (`run_module`, 2361 ms); `elf-txtbsy` FAIL (see below) | the same 12; `elf-txtbsy` FAIL |
| candidate: `run_module` checks its own pid + `run_tty_probe` waits for its own, pid 8 delayed | pass; `dev-tty` 76 ms, `dev-tty-none` 33 ms | pass; 82 ms, 36 ms |
| **the design** (below), pid 8 delayed | **pass**; the runner waits 76 ms after `dev-tty`, and more than 1 ms after no other test | **pass**; 67 ms |
| **the design**, every drop delayed 30 ms | **only `elf-txtbsy` fails**; the runner waits after 12 tests, at most 189 ms | **only `elf-txtbsy` fails**; at most 54 ms |

The 12 tests that return before their own process is released, once
the reaper is late (the same set on both architectures):
`tap-ready`, `signal-stop-restart`, `tty-raw`, `tty-nosig`, `dev-tty`,
`tty-intr`, `tty-stop`, `tty-ttin`, `ipc-fifo`, `process-spawn`,
`process-reaped`, `cwd-hold-native`. The test before each of the three
sightings is on the list.

### A second finding, not taken up here

`elf-txtbsy` kills its child, waits for the exit, puts its reference,
and then expects to be able to write the program's file. With the
reaper late, the write returns `-26` (`-ETXTBSY`) on both
architectures: the child's text mapping is still there. An exited
process keeps its **address space** until its last reference drops; only
its handles close at exit (`process_last_thread_gone`). The native
`waitpid` (`process_wait_child`) returns once the child is `EXITED`,
which the reaper sets before it drops its reference. So a program
that waits for a child and then rewrites the child's binary can be
refused, and the child's memory stays allocated after its status has
been collected. Linux releases the address space in `do_exit`, before
the parent is told.

This is a kernel behaviour, not a test defect, and moving the teardown
to exit needs its own audit of everything that reads `p->space` after
exit. It is recorded here, with its forced proof, as the next unit.
`elf-txtbsy` keeps its assertion, which is the right semantics. It has
no natural sighting.

### Why it matters

- Three failures in three tests, each blamed on the test that failed
  rather than the one before it. The bound widened for the third left
  the cause in place.
- The check the failures come from also passes vacuously, so it does not
  prove what it names: that the process `run_module` made was released.
- A process leaked for good would be caught only if the next test happened to be
  one of `run_module`'s, and then blamed on that test.

## Current implementation

- `run_module` (`proctest.c`) settles on `process_count() == before` for
  2 s with `sched_yield`. It has 18 call sites.
- `elf_settle_processes(procs0)` in the ELF tests settles on the same
  count, before every return (37 sites), with no check.
- `run_tty_probe`, `selftest_tty_intr` and the other spawners in the
  list wait for their process's exit and put their reference; none waits
  for the release.
- The runner (`kernel/core/selftest.c`) runs each test and prints its
  verdict; it checks nothing between tests.
- `process_lookup(pid)` answers "is this pid alive as far as a caller is
  concerned": it refuses a reaped process. Nothing answers "has this
  pid's release finished".

## Design

### 1. A process is named, not counted

`bool process_present(pid_t pid)` (`process.c`): true while `pid` is in
the table, that is, while its release has not finished. It takes no
reference. Pids are never reused (`g_next_pid++`), so a pid names one
process for the life of the machine.

`run_module` keeps its process's pid, and after its `process_put` waits,
up to 2 s with 1 ms sleeps, for `process_present(pid)` to go false, and
checks that. The machine-wide count goes. On failure it names the pid:
"its process (pid N) was not released".

### 2. Nothing a test made outlives it

The runner, after every test and before printing its verdict, waits up
to 2 s for the process table to empty. A test that leaves a process
fails ("a process it spawned outlived it (P33)"), and up to sixteen of
the pids left are logged with their name, state, references and threads
(any beyond that are counted). The wait counts in the test's duration, so
the per-test budget sees it.

This covers every spawner in one place, including the 12 in the list
and any written later. It moves the failure to the test that left the
process. `elf_settle_processes` becomes redundant (every call precedes
a return) and goes.

The rule is that no process outlives the test that made it. It holds
today: in every boot measured with the reaper undelayed, the table was
empty at every test's end, on both architectures.

### 3. The order, forced on every boot

A reaper hold armed by identity: `process_test_hold_reap(pid)` parks the
reaper between `process_last_thread_gone` and its reference drop for that
pid only, until the test releases it (a completion, not a sleep). It is
compiled only in debug builds, as the existing identity-armed hold
(`tcp_test_hold_callback`) is, and its tests skip in release builds. The reaper is one thread, so while it
is parked, every exit queued behind it waits too.

A new test, `process-gone-order`, uses it to make the CI order certain:

1. spawn A (`init --spin`), arm the hold on A, kill A, wait for its
   exit, put it. A is now exited and held: `process_present(A)` must be
   true (the hold is real, not vacuous);
2. note the count, then run B through `run_module` with a hook that,
   after B is spawned, releases A's hold. The reaper is serial, so A's
   release comes before B's exit is even completed;
3. `run_module`'s own check must pass (B gone, by pid), A must be gone,
   and the count must be one *below* the one noted: the order the old
   check fails on really happened.

A second new test, `process-leftover-named`, checks the runner's rule
directly. It holds a process's reap, calls the runner's check with a
short deadline, and requires it to name that pid. Then it releases the
hold and requires the check to come back empty.

### 4. The record

`flakes.md`: the three sightings attributed to this mechanism, with the
list, and fixed by this unit. The `tty-isatty` entry's widened bound is
explained as the wrong fix. Also the `elf-txtbsy` finding with its
forced proof and no natural sighting. `invariants.md` (process): **P33,
nothing a self-test spawns outlives it**, with the runner as its
enforcement. The inventory's §3 gains the address-space-at-exit
finding as a candidate unit.

### 5. The §70 gate

**Correctness.** A check that counted every process on the machine
becomes one about the process the test made; one that could pass by
substitution cannot.

**Concurrency.** The kernel gains one test hold, armed by pid, on the
reaper's path; unarmed, it is one load. `process_present` takes the
table lock, as `process_lookup` does.

**Ownership and lifetime.** `process_present` takes no reference and
dereferences nothing after the lock is dropped.

**Security.** None: the new function is kernel-internal.

**Failure.** Every failure names a pid. A process that never leaves is
reported, not waited on forever.

**Performance.** The runner's wait costs nothing when the table is
already empty (every test's end on an unmodified boot). With the reaper
forced late it is at most 189 ms after one test.

## Affected files

| file | change |
|---|---|
| `kernel/process/process.c`, `kernel/include/kernel/process.h` | `process_present`; the reap hold's entry points |
| `kernel/scheduler/thread.c` | the hold, between `process_last_thread_gone` and the reference drop |
| `kernel/process/proctest.c` | `run_module` by pid, with a test-only hook; `elf_settle_processes` removed; `process-gone-order`, `process-leftover-named` |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | the runner's rule; the two registrations |
| `docs/kernel/process/invariants.md`, `testing.md` | P33; the two tests; `run_module`'s check |
| `docs/testing/flakes.md`, `docs/audit/2026-09-deferred-work-inventory.md`, `README.md` | the record, the next unit, the Status entry |

## APIs

`bool process_present(pid_t pid)` (kernel-internal). The reap hold is
test-only. No syscall or ABI change.

## Migration plan

One PR. No on-disk or user-visible change.

## Tests

- `process-gone-order` and `process-leftover-named`, above, on every
  boot, both architectures.
- Mutations, each alone and with its boot confirmed:
  - `run_module` back to the count: `process-gone-order` fails;
  - the runner's rule made to find nothing: `process-leftover-named`
    fails;
  - the hold ignored: `process-gone-order`'s "A is present" fails;
  - `process_present` always false: the same check fails;
  - one spawner's leftover under the probe's delay, with the rule in
    place: the runner names the spawner, not the next test.
- `gmake host-test`, `gmake analyze`, debug and release boots and the
  GIC and chaos boots, on both architectures.

## Benchmarks

None: the runner's wait is zero when there is nothing to wait for, and
the release itself is unchanged.

## Risks

- **A test that means to leave a process.** None does today (measured:
  the table is empty at every test's end). One written later fails by
  name, and would need its own cleanup.
- **The runner's wait hides a slow release.** It waits at most 2 s, then
  fails. A release that slow is reported.
- **`elf-txtbsy` stays exposed** to the address-space finding. That is
  deliberate: waiting for the release there would write the defect into
  the test.

## Alternatives considered

- **A longer bound.** Already tried once (500 ms → 2 s). The check still
  fails with any bound, because the count never comes back.
- **Every spawner waits for its own process** (the measured candidate).
  It passes, but only covers the spawners someone remembered: 12 today. The runner's rule covers them all and names the one that
  forgot.
- **Assert the count is zero at `before`.** It would fail the next test
  instead of the one that left the process, which is the current problem
  in a different form.
- **Tear the address space down at exit** (the second finding). This
  would shorten the window, but not close it: the table entry still
  outlives the exit. It is a kernel change needing its own audit, so it
  is proposed as its own unit.
