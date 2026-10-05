# Two-CPU validation and scheduler/VirtIO root-cause investigation

Date: 2026-10-05. Branch `two-cpu-validation` from `main` at `7eb7b244`.
Scope: the first suggested increment in [docs/plan.md](../plan.md), §2:
the two-CPU VirtIO removal and scheduler failures, plus the recorded
timing observations as secondary items.

This file was kept as a live notebook while the work ran; the sections
below are the required structure, and §"Notebook" at the end keeps the
raw chronological record.

## 1. Executive Summary

(pending)

## 2. Repository Baseline

- Branch `two-cpu-validation`, created from `main` at
  `7eb7b2442281ce75bdcf0bb15be91d99caff79b7` (docs: make README a high-level
  overview).
- PR #308 is merged: `24ed5c45` (merge), implementation `95dc0286`
  ("test: observe spin contention and measure acquisition handoff"), CI
  follow-up `e324a62b` ("test: rendezvous spin contenders before masking
  interrupts"). Both commits are ancestors of HEAD; this work does not
  reimplement them.
- The observations under investigation come from the
  [October 4 report](2026-10-04-spin-contention-report.md), Phase 4: two
  AArch64 two-CPU full-suite attempts (`out/spin-final-aarch64-on2{,-retry}.log`).

## 3. Test Environment

| Item | Value |
|---|---|
| Host | Apple M1, 8 cores, 16 GiB, macOS 27.0.1 (arm64) |
| Compiler / linker | Apple clang 21.0.0, LLD 21.0.0 (swiftly) |
| QEMU | 11.1.1 (`qemu-system-x86_64`, `qemu-system-aarch64`), TCG |
| Make / Python | GNU Make 4.4.1, Python 3.14.3 |
| Build | `BUILD=debug` (default): `SELFTEST=1`, `LOCKDEP=1` (`build/config.mk`) |
| Guest memory | harness default (`QEMU_MEM`) |
| Guest CPUs | `QEMU_SMP=N` (default 4, `docs/development.md`) |
| Host load at start | load average 6.75; Chrome, a Parallels VM and a Virtualization.framework VM running, not controllable from this session |

Boots run one at a time; nothing else was built or booted while a
matrix ran. Every boot uses `gmake ARCH=<arch> QEMU_SMP=<n>
BOOT_LOG=<log> test` through `tools/two-cpu-matrix.sh`, which keeps
each boot's serial log and records the harness's own `boot-test:` verdict.

## 4. CPU-Count Matrix

(pending)

## 5. VirtIO Removal Investigation

(pending)

## 6. VirtIO Root Cause

(pending)

## 7. Scheduler Test Investigation

(pending)

## 8. `sched-spread`

(pending)

## 9. `sched-balance-pair`

(pending)

## 10. Scheduler Root Cause(s)

(pending)

## 11. Lockdep Observations

(pending)

## 12. Quiescence/Lifetime Observations

(pending)

## 13. Timing Investigations

(pending)

## 14. AArch64 Observations

(pending)

## 15. Kernel Bugs Confirmed

(pending)

## 16. Test Bugs Confirmed

(pending)

## 17. Environment/QEMU Findings

(pending)

## 18. Changes Implemented

(pending)

## 19. Regression Tests

(pending)

## 20. Validation Results

(pending)

## 21. Remaining Uncertainty

(pending)

## 22. `docs/plan.md` Changes

(pending)

## 23. Recommended Next Increment

(pending)

## Notebook

Chronological record. Times are host local (+03).

### 14:12 — Phase A started: unchanged `main` (`7eb7b244`), x86-64 then AArch64

Command: `tools/two-cpu-matrix.sh <arch> base 4 2`, then `... base 2 1 3 4`.
Images built once before the matrix (`gmake -j6 ARCH=<arch> image`); no
source change between boots.

x86-64, 2 CPUs, boots 1–2: both FAIL with the same two tests,
`virtio-remove-inflight` and `sched-spread`. `sched-balance-pair` passed in
both (yielding pair separated after 5 ms).

- `virtio-remove-inflight`: all four `irq-order` attempts report
  "the teardown did not overlap the read-side section", entered stamp
  after the section-end stamp each time (e.g. entered 648, ended 386), then
  `++order_attempts < 4` at `kernel/device/devtest.c:1726`.
- `sched-spread`: "8 threads that block after creation used 1 of 2 CPUs,
  8 of them on CPU 1".

Hypotheses formed from source reading before the AArch64 results:

- **H-V1 (VirtIO placement).** `rm_irq_order_pass` puts the submitter and
  remover on `cpu = other_cpu_for_blk()` and the holder on
  `other_cpu_than(cpu)`. With two CPUs and the test thread pinned on T,
  `other_cpu_for_blk()` is the other CPU O, and `other_cpu_than(O)` finds no
  CPU that is neither T nor O and returns O. Holder, remover and submitter
  all run on O. The holder disables preemption for `RM_HOLD_NS` (60 ms), so
  the remover cannot run until the section ends: no overlap is possible.
  Prediction: deterministic failure at two CPUs on both architectures, pass
  at three or more.
- **H-S1 (`sched-spread`).** `pick_cpu` (`kernel/scheduler/sched.c`) takes
  the strict minimum of `sched_cpu_load`, which counts a CPU's running
  thread. The creating test thread is running on its own CPU when it calls
  `thread_create`, so that CPU reads at least 1 and the other CPU of two
  reads 0: there is never a tie, the rotor never decides, and every worker
  goes to the non-creator CPU. Prediction: deterministic 8-of-8 on the CPU
  the creator is not on, at two CPUs.
- **H-P1 (`sched-balance-pair`, AArch64 only so far).** With two CPUs the
  only receiver for the pair's spare worker is the test thread's own CPU.
  The test polls with `thread_sleep_ms(2)`; timers expire in `tick_isr`
  (`kernel/timer/timer.c`) via `run_expired` *before* the tick hook runs
  `balance_tick`, so a sleeper that expires on a tick is already queued when
  its CPU asks whether it is idle. Unverified; x86 separated in 5 ms, so
  either the mechanism differs by architecture or H-P1 is wrong.
