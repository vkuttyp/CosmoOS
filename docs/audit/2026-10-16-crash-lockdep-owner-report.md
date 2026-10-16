# The crash test's lockdep owner — 2026-10-16

Main went red at `621f7f78` (#393's merge). AGENTS.md: a red merge run
takes priority, and a defect that turns main red is fixed. Base: main
`621f7f78`.

## The failure

Run 38819564952, job 116712603881 (`net-login test (x86_64)`), step `make
test-display`: `FAIL: panic-lockdep: the crash test's own panic: no line
matching "^KERNEL PANIC: crash\ test: lockdep's\ lock\ is\ held\ by\
another\ CPU.*$"`. The tree is the one `166a2ef3` (#393's head) passed in
its own run. The boot's serial log, from the job's artifact:

```text
[ INFO] crash test: panicking with lockdep's lock held elsewhere, on purpose

KERNEL PANIC: lockdep: graph raw lock re-entry on CPU 1
CPU: 1  context: interrupt  thread: 6 'idle'  irq_depth: 1  preempt_count: 1
```

## Mechanism

`CRASH_TEST=3` (the virtio-gpu unit's check e) leaves lockdep's own raw
lock held and panics, to show the screen gets a panic without a tracked
lock. `lockdep_crash_hold_raw` wrote the owner word as the next CPU's
(this CPU's id + 2). That CPU is running. When it entered lockdep -- here
an interrupt taking a tracked lock -- between the store and the panic's
`smp_stop_others`, `raw_lock` found its own id in the word and panicked
with "graph raw lock re-entry" before the crash test's panic. Whether a
CPU enters lockdep in that window is timing, so the same tree passed and
failed.

## Changes

Three commits, so the probe can restore exactly the owner word:

| Commit | Change | Files |
|---|---|---|
| `b44ed1c3` | the race made certain and observed, the owner word unchanged: a thread pinned to the next CPU takes a tracked lock after the hold, every run; lockdep's raw lock records the CPU it finds waiting on the held word (`CRASH_TEST=3` only, `lockdep_crash_waiter`); the crash path keeps interrupts off from the hold on (a timer interrupt's tracked locks would otherwise spin on the held word -- seen while building the fix, the first version hung after the crash-test line), waits 2 s for the waiter, and panics with a message the check refuses when no other CPU is online, the thread cannot be made or does not start, or no waiter is seen; check e requires the waiter (`CPU N waiting on it`). On its own this commit fails check e every run with the re-entry panic. | `kernel/core/main.c`, `kernel/core/lockdep.c`, `kernel/include/kernel/lockdep.h`, `tests/boot/display_test.py` |
| `bc740c38` | the owner word names CPU `CONFIG_MAX_CPUS`, which no CPU is: a CPU that enters lockdep spins, as on a lock a stopped CPU holds, until the panic stops it (the stop gives up after about 10 ms for a CPU that does not answer) | `kernel/core/lockdep.c`, `kernel/include/kernel/lockdep.h` |
| probe | `tools/crash-lockdep-owner-probe.py` | |

## Proof

`tools/crash-lockdep-owner-probe.py [--old] [--arch x86_64|aarch64]`
builds the debug `CRASH_TEST=3` image in a throwaway worktree, boots it
(`make run`) and judges by the serial log; `--old` reverse-applies
`bc740c38`'s `kernel/core/lockdep.c` (found by its subject, or named with
`--fix`), so the old owner word meets the certain race. The seam and the
crash path are generic code, so the probe runs on both architectures;
check e's screen read is x86-64's.

- x86-64 `--old`: `PROBE: PASS ... the next CPU's re-entry panic came
  first`; fixed: `crash test: lockdep's lock is held by another CPU (owner
  word 65, this CPU's 1, CPU 2 waiting on it)`.
- AArch64 `--old`: the re-entry panic first; fixed: `(owner word 65, this
  CPU's 2, CPU 3 waiting on it)`.
- `make BUILD=release test-display` (x86-64): check e PASS with the waiter.

The probe's first version failed in both modes before judging anything:
its worktree lay deep under `out/`, and test-display's QMP socket path
inside it passed macOS's 104-byte limit for a unix socket (`QEMU exited
(code 1) waiting for the first prompt`); worktrees now sit at
`out/clp-*`. Review (#394) asked for the race to be proven run (the waiter),
for both architectures, and for a fallback to the subject search (`--fix`).

## Validation

(filled in from the run)
