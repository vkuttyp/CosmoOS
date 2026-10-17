# The crash test's lockdep waiter — 2026-10-17

Main went red at `d4bf1e79` (#400's merge). AGENTS.md: a red merge run
takes priority, and a defect that turns main red is fixed. Base: main
`d4bf1e79`. It blocks the v1.0.4 tag, which waits for a green main.

## The failure

Run 39048054390, job 117440707868 (`net-login test (x86_64)`), step `make
BUILD=release test-display`: `FAIL: panic-lockdep: no other CPU waited on
lockdep's lock (CPU 1)`. Every other job of the run passed, and the same
job passed in #400's own run (39043900425). The serial logs were not kept:
the artifact upload failed (`Error: entry not supported`, the QMP sockets in
the directory). The check (`lockdep_held_elsewhere` in
`tests/boot/display_test.py`) refuses a waiter that is 0, this CPU's or the
owner's word. The owner word is 65 (CPU `CONFIG_MAX_CPUS`) and the waiter 1,
so the panic named this CPU, CPU 0, as the CPU waiting on the held word.

## Mechanism

`raw_lock` in `kernel/core/lockdep.c` stored into `g_crash3_waiter` the
CPU of every wait on lockdep's raw lock, whatever word held it, and nothing
cleared it. The crash path (`kernel/core/main.c`) waits until the record is
nonzero and reads it back. A wait from boot contention before the hold left
a nonzero record: the loop ended at once, and when the other CPU had not
yet entered lockdep (it spins on `go` and enters within microseconds, so
this is timing) the panic named whichever CPU waited last -- here CPU 0
itself. A stale wait by the other CPU would instead have passed check e
without the race having run.

## Changes

| Commit | Change | Files |
|---|---|---|
| `60857451` | the stale wait made certain, the record unchanged: the other CPU first holds lockdep's lock until this CPU has waited on it (`lockdep_crash_contend`; this CPU waits through `lockdep_get_stats`, and the crash path panics if it was not seen waiting), then enters lockdep 10 ms after the hold, so the crash path's 2 s wait, not that CPU's speed, orders its entry after the reads. On its own this commit fails check e every run, exactly as on CI. | `kernel/core/main.c`, `kernel/core/lockdep.c`, `kernel/include/kernel/lockdep.h` |
| `5920f2eb` | only a CPU that finds the held word (CPU `CONFIG_MAX_CPUS`'s) is recorded as the waiter | `kernel/core/lockdep.c`, `kernel/include/kernel/lockdep.h`, `docs/drivers/virtio/testing.md` |
| `12f9d8e2`, `6f7a2d95` | `tools/crash3-waiter-probe.py`, booting two CPUs as `test-display` does | |

Everything is under `CONFIG_CRASH_TEST == 3`, which requires a debug
build; every other build compiles the same code (`lockdep.c` gains an
include of `<kernel/timer.h>`).

## Proof

`tools/crash3-waiter-probe.py [--old] [--arch x86_64|aarch64]` builds the
debug `CRASH_TEST=3` image in a throwaway worktree, boots it with
`QEMU_SMP=2` (`make run`) and reads the panic line; `--old` undoes
`5920f2eb`'s one code change, the condition on the waiter record in
`raw_lock`, by text. It first reverse-applied the whole commit's
`kernel/core/lockdep.c`; review (Codex, PR #401) found that after a
squashed merge the matching commit's diff also holds the seam's helpers,
so the build would fail instead. The fixed runs below are unchanged by
that; `--old` was rerun with the text version on both architectures, with
the same results as the first version.

- x86-64: fixed `owner word 65, this CPU's 1, waiter 2`; `--old` `this
  CPU's 1, waiter 1` (the stale wait).
- AArch64: fixed `this CPU's 2, waiter 1`; `--old` `this CPU's 1, waiter
  1`.
- `make BUILD=release test-display` on `60857451` alone (x86-64): `FAIL:
  panic-lockdep: no other CPU waited on lockdep's lock (CPU 1)`, the CI
  line; on the fix, PASS in 30.1 s with `CPU 2 waiting on it`.

The first probe runs booted `make run`'s default CPU count, and `--old`
then reported waiter 3 and 4: a third CPU waiting on lockdep's lock after
this CPU's pre-hold wait overwrote the record. With more than two CPUs the
old record could therefore also pass check e with no CPU on the held
word; the fix records only that word, so the CPU count no longer matters.

## Validation

The validation matrix on `6f7a2d95`, every step PASS (the boot times are the
harness's own lines):

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test`, `fuzz`, `analyze` | pass (analyze clean, 7 baselined) | pass (clean, 7 baselined) |
| debug `test`, `QEMU_SMP=1` / `2` / `4` | 164.7 / 169.1 / 154.2 s | 153.5 / 175.3 / 169.4 s |
| `test-smp2` | 156.0 s | 171.1 s |
| `test-chaos` | 150.6 s | 172.6 s |
| `test-harness-retry` | 155.6 s | 170.3 s |
| `BUILD=release test` | 18.8 s | 22.1 s |
| `BUILD=release test-display` | 29.9 s (checks d and e) | 20.6 s |

The network harness passed in every debug boot.

Branch CI. The first two runs each failed one job in a boot that runs
main's code (neither builds `CRASH_TEST=3`), each recorded in
`docs/testing/flakes.md`:

- Run 39056570530 (`9ddc330b`): the AArch64 chaos boot timed out at 247 s
  with every self-test passed. `test-chaos` had kept its own 240 s when
  the other AArch64 boots moved to 300 s; over 57 AArch64 chaos boots of
  the last 60 CI runs the p95 is 228.3 s and the slowest pass 236.9 s.
  It now takes `BOOT_TIMEOUT` (300 s on AArch64, 240 s on x86-64
  unchanged), widened and labelled in the Makefile comment; local
  `ARCH=aarch64 test-chaos` PASS in 158.1 s with `timeout 300s`.
- Run 39057108969 (`c60f6e58`): `irqpoll-boost`'s eighth sighting, the
  same 290 ms gap, in x86-64's `test-crash` boot.

Review: Codex found that `--old` would not build after a squashed merge
(fixed, above); Qodo asked for a compile-time bound on
`g_crash3_waited` (answered: the bound is on a run-time CPU id, enforced
at SMP bring-up, as for lockdep's other per-CPU tables).
