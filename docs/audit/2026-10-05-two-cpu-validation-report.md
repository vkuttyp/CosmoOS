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

### 14:55 — Phase A complete: unchanged `main`, 20 boots

Logs: `out/matrix/base-<arch>-smp<N>-<rep>.{serial,result}`, rows in
`out/matrix/base-<arch>.tsv`. Verdicts are the harness's `boot-test:` lines.

| Arch | CPUs | Boots | Harness | Failing tests (boots failing / boots) | Boot s (min/median/max) |
|---|---|---|---|---|---|
| x86-64 | 1 | 2 | 2 PASS | — | 115.0 / 115.3 / 115.6 |
| x86-64 | 2 | 4 | 4 FAIL | `virtio-remove-inflight` 4/4, `sched-spread` 4/4 | 124.3 / 125.0 / 131.7 |
| x86-64 | 3 | 2 | 2 FAIL | `sched-spread` 2/2 | 124.5 / 125.4 / 126.2 |
| x86-64 | 4 | 2 | 2 PASS | — | 124.1 / 128.1 / 132.0 |
| AArch64 | 1 | 2 | 2 PASS | — | 117.2 / 124.4 / 131.5 |
| AArch64 | 2 | 4 | 4 FAIL | `virtio-remove-inflight` 4/4, `sched-spread` 4/4, `sched-balance-pair` 3/4 | 128.8 / 138.8 / 141.1 |
| AArch64 | 3 | 2 | 2 FAIL | `sched-spread` 2/2 | 127.5 / 128.4 / 129.3 |
| AArch64 | 4 | 2 | 2 PASS | — | 129.8 / 132.0 / 134.1 |

Every boot completed all 423 self-tests and reached the post-test harness;
no hang, timeout or panic. Per-test observations:

- `virtio-remove-inflight`: at two CPUs every `irq-order` attempt (16 of 16
  across eight boots) reported the teardown entering after the holder's
  section ended. At three and four CPUs it passed first time in all eight
  boots, with stamps in the required order, e.g. "entered the queue teardown
  at 600 with a read-side section open, the section ended at 601 and only
  then did the slot walk begin, at 602".
- `sched-spread`: two CPUs, 8 of 8 on one CPU in all eight boots (CPU 1 in
  five, CPU 0 in three). **Three CPUs: "used 2 of 3 CPUs, 6 of them on CPU 1"
  in all four boots** — so H-S1 as first stated (three CPUs suffice) is
  wrong. Four CPUs: "used 3 of 4 CPUs, at most 4 on any one" in all four
  boots: the test's bound is `worst > N/2`, i.e. `> 4`, so four CPUs pass
  *at* the bound with no margin, and in no boot did the fourth CPU receive a
  worker.
- `sched-balance-pair`: x86-64 separated the yielding pair in 3–6 ms at
  every CPU count. AArch64 separated in 0–4 ms at three and four CPUs; at two
  CPUs it separated after 511 ms once and was "NOT separated after
  1002–1003 ms" in three boots. The spinning half stayed together everywhere.
- `syscall-fuzz` (fixed seed `20260905`, 20,000 calls): x86-64 3,087–3,350 ms,
  AArch64 3,548–3,839 ms, against the 8,000 ms budget. No excursion; CPU
  count makes no visible difference (AArch64 1 CPU 3,628/3,718; 2 CPUs
  3,548–3,839; 4 CPUs 3,659/3,661).
- `net-bench`: 1,212–3,487 ms, all passing.
- Lockdep: the only lockdep reports in all twenty serial logs are the
  suite's own `expected report:` lines (15 at one CPU, 17 at two or more,
  identical per configuration). No unexpected report.
- Other warnings are identical per configuration. The one count that
  changes with CPUs — "unhandled interrupt vector" (2 at one CPU, 4 at two, 6 at three,
  8–9 at four) — follows the IRQ unregister/writer tests that run per CPU
  (first one right after `irq-writers`), which is already the baseline.

#### Second look at `sched-spread`

History settles why the premise drifted. The rotation was added by
`d2babd03` ("Step 2: ties rotate"), when `pick_cpu` compared `nr_running`,
which does **not** count a CPU's running thread: the creator's CPU tied with
the idle ones and the commit measured "4 of 4 CPUs". The load-balancer unit
later made `pick_cpu` read `sched_cpu_load` (S29,
`docs/kernel/scheduler/design.md` §3b), which counts the running thread. From
then on the creator's CPU never ties, and the scan

```c
unsigned start = rotor++ % n;
for (i = 0; i < n; i++) { c = (start + i) % n; if (load(c) < best_load) best = c; }
```

hands the creator's turn to the next CPU in scan order. With the creator on
CPU 0: at three CPUs, `start` 0 and 1 both pick CPU 1 and `start` 2 picks
CPU 2 — 2:1, i.e. 6 and 2 of 8, exactly what all four boots show. At four
CPUs, CPU 1 gets 2 of 4 turns, 4 of 8, exactly the bound. At two CPUs there
is one eligible CPU and all eight go there. Revised hypothesis **H-S1'**: the
failures are what the current policy (least loaded, running thread counted,
ties rotated by a scan offset) produces deterministically; the test's
`worst > N/2` bound was derived when every CPU tied and was never re-derived
after S29. To be confirmed by the creator-CPU diagnostic.

### 15:05 — Light instrumentation (`7d4b55f9`), 8 boots

Two diagnostics, no assertion change: `sched-spread` logs the creator's CPU
mask, and `sched-balance-pair` logs the balancer's existing machine-wide
counters (`sched_balance_stats`) as a delta over the yielding pair's
widen window. Logs: `out/matrix/instr-<arch>-smp<N>-<rep>.serial`.

`sched-spread` (8 of 8 boots fail as before):

| Boot | Creator mask | Result |
|---|---|---|
| AArch64 2 CPUs ×4 | `0x1`, `0x2`, `0x2`, `0x1` | 8 on CPU 1, 0, 0, 1: always the other CPU |
| x86-64 2 CPUs ×2 | `0x1`, `0x1` | 8 on CPU 1 |
| AArch64 3 CPUs | `0x2` | 5 on CPU 2, 2 of 3 CPUs used |
| x86-64 3 CPUs | `0x4` | 5 on CPU 0, 2 of 3 CPUs used |

The creator stayed on one CPU throughout, received no worker in any boot,
and the pile is always on the CPU after the creator's in scan order. H-S1'
holds. (5 rather than 6 at three CPUs: the rotor's phase at the first
creation differs per boot; 5 or 6 both exceed the bound of 4.)

`sched-balance-pair` — the deciding data:

| Boot | Yielding result | Scans in window | Pulls | No candidate | Refused (any reason) |
|---|---|---|---|---|---|
| AArch64 2/1 | separated after 168 ms | 7 | 1 | 6 | 0 |
| AArch64 2/2 | NOT separated, 1003 ms | 32 | 0 | 32 | 0 |
| AArch64 2/3 | separated after 4 ms | 1 | 1 | 0 | 0 |
| AArch64 2/4 | NOT separated, 1000 ms | 31 | 0 | 31 | 0 |
| x86-64 2/1 | separated after 5 ms | 1 | 1 | 0 | 0 |
| x86-64 2/2 | **NOT separated, 999 ms** | 32 | 0 | 32 | 0 |
| AArch64 3 | separated after 4 ms | 1 | 1 | 0 | 0 |
| x86-64 3 | separated after 2 ms | 1 | 1 | 0 | 0 |

- **x86-64 fails too** (1 of 2 here, after 0 of 4 at baseline): the failure
  is not architecture-specific. Baseline + instrumented, two CPUs: AArch64
  5 of 8 boots failed, x86-64 1 of 6.
- In every failing window the balancer scanned ~32 times in one second,
  every scan "no candidate", nothing refused. 32/s is exactly two CPUs ×
  250 Hz / `SCHED_BALANCE_TICKS` (16): only the busy-CPU periodic scans
  ran. The receiving CPU (the test thread's) never once scanned as idle.
  A busy-path scan computes `mine` = 1 (the test thread, queued or
  running) against the pair's 2, and `2 < 1 + 2` is "no candidate"
  (`balance_tick`, `kernel/scheduler/sched.c`).
- In every passing window there is exactly one scan and one pull, within a
  tick or two: one idle tick on the receiver, and it pulled at once.

Mechanism (H-P1, now supported by the counters): the test thread polls with
`thread_sleep_ms(2)` on the receiving CPU. Timers expire only in
`tick_isr` (`kernel/timer/timer.c`), and `run_expired` runs *before* the
tick hook calls `sched_tick` → `balance_tick`. A sleeper armed within a
tick of its deadline therefore expires on the next tick and is queued
before that tick's balancer asks "am I idle". After each wake the thread
re-arms at once, so its next deadline (2 ms) lands before the following
tick (4 ms): the poll phase-locks to the tick and the receiver is never
idle when it looks. Only the first sleep after the widen starts at a
random phase; if it is armed within 2 ms of a tick, that tick is idle and
the pull happens (1 scan, 1 pull). That predicts roughly half of two-CPU
windows failing, matching 6 of 14. With three or more CPUs another idle
CPU exists that the test thread does not occupy, so it pulls on its first
tick — matching every passing three- and four-CPU boot.

Classification: **CONFIRMED TEST BUG** — the observer's own wake-ups
occupy the only receiving CPU at the balancer's decision point. The
balancer behaved as documented (idle CPU every tick; busy CPU every 16
ticks at a difference of two). A policy note follows in §10.
