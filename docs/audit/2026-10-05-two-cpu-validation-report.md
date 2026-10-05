# Two-CPU validation and scheduler/VirtIO root-cause investigation

Date: 2026-10-05. Branch `two-cpu-validation` from `main` at `7eb7b244`.
Scope: the first suggested increment in [docs/plan.md](../plan.md), §2:
the two-CPU VirtIO removal and scheduler failures, plus the recorded
timing observations as secondary items.

This file was kept as a live notebook while the work ran; the sections
below are the required structure, and §"Notebook" at the end keeps the
raw chronological record.

## 1. Executive Summary

Unchanged `main` (`7eb7b244`) fails deterministically with two CPUs on
both architectures, and with three CPUs as well. All three failures
under investigation are **test defects**; no kernel defect was found, and
no kernel, scheduler-policy or VirtIO-synchronization code was changed.

| Observation | Classification | Root cause |
|---|---|---|
| `virtio-remove-inflight` (2 CPUs, 8/8 boots) | CONFIRMED TEST BUG (two layers) | (a) the holder, remover and submitter were all placed on the one non-test CPU, so the nonpreemptible holder ran first and the removal could not overlap it; (b) once the removal ran elsewhere, a submitter preempted inside `blk_submit` behind the holder kept `bd->submitting` raised, and `blk_unregister`'s drain — correctly — waited for the section to end |
| `sched-spread` (2 CPUs 8/8, 3 CPUs 4/4) | CONFIRMED TEST BUG (with a two-CPU topology premise) | the `worst > N/2` bound predates S29; placement counts the running creator, so its CPU never ties and the scan hands the creator's turn to the next CPU (6 or 5 of 8 at three CPUs, exactly 4 of 8 at four). With two CPUs there is one eligible CPU and no tie to observe |
| `sched-balance-pair` (2 CPUs, AArch64 5/8, x86-64 1/6) | CONFIRMED TEST BUG | the observer polled with a 2 ms sleep on the only receiving CPU; timers expire in the tick before `balance_tick` runs, so the poll phase-locks to the tick and the receiver is never idle when the balancer looks |
| `syscall-fuzz` excursions | TIMING SENSITIVITY (not reproduced) | 3,087–3,921 ms in every boot here against 8,000 ms; the recorded 8,240–9,641 ms excursions remain unexplained |
| `thrtest` / `cwdtest` timing | TIMING SENSITIVITY (latent, not observed) | `cwdtest --held` bounds seam waits by 200,000 yields, a count standing in for a duration |
| `net-bench` long run | UNRESOLVED (not reproduced) | 1.2–3.5 s in every boot |
| slirp resets | not exercised | no reset in this session; nothing to add to the existing diagnosis |
| AArch64 virtio-console loss | UNRESOLVED (not reproduced) | 0 harness failures in 35 AArch64 captures |

After the test fixes, every boot at one, two, three and four CPUs passes
the full suite on both architectures (§20), and four negative-control
mutations (nine boots) fail each fixed test for its stated reason (§19).
CI does not boot two CPUs (`.github/workflows`: every `test*` job uses the
default `QEMU_SMP=4`), which is why these never appeared there.

## 2. Repository Baseline

- Branch `two-cpu-validation`, created from `main` at
  `7eb7b2442281ce75bdcf0bb15be91d99caff79b7` (docs: make README a high-level
  overview).
- PR #308 is merged: `24ed5c45` (merge), implementation `95dc0286`
  ("test: observe spin contention and measure acquisition handoff"), CI
  follow-up `e324a62b` ("test: rendezvous spin contenders before masking
  interrupts"). Both are ancestors of HEAD; this work does not
  reimplement them.
- The observations come from the
  [October 4 report](2026-10-04-spin-contention-report.md), Phase 4: two
  AArch64 two-CPU full-suite attempts (`out/spin-final-aarch64-on2{,-retry}.log`),
  recorded in `docs/plan.md` §2 and inventory §7.
- Commits on this branch: `7d4b55f9` (diagnostics only), `637f2793`
  (the three test fixes), `9a765c5c` (submitter parking), `9d6a0e0d`
  (log-line split).

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
| Tick | 250 Hz (`CONFIG_HZ`), slice 10 ms, `SCHED_BALANCE_TICKS` 16 |
| Host load | load average 6.75 at start; Chrome, a Parallels VM and a Virtualization.framework VM running throughout, outside this session's control |

Boots ran strictly one at a time; nothing was built or booted while a
matrix ran, and kernel sources were not edited while a boot ran. Every
boot uses `gmake ARCH=<arch> QEMU_SMP=<n> BOOT_LOG=<log> test` through
`tools/two-cpu-matrix.sh`, which keeps each boot's serial log and records
the harness's own `boot-test:` verdict. Timing numbers here are from a
loaded, shared host and are not scalability evidence.

## 4. CPU-Count Matrix

Unchanged `main`, 20 boots (`out/matrix/base-*`; full per-test lines in
the notebook, 14:55):

| Test | x86 1 | x86 2 | x86 3 | x86 4 | A64 1 | A64 2 | A64 3 | A64 4 |
|---|---|---|---|---|---|---|---|---|
| boots | 2 | 4 | 2 | 2 | 2 | 4 | 2 | 2 |
| harness | PASS | **FAIL** | **FAIL** | PASS | PASS | **FAIL** | **FAIL** | PASS |
| `virtio-remove-inflight` | skip (one CPU) | **FAIL 4/4** | pass | pass | skip | **FAIL 4/4** | pass | pass |
| `sched-spread` | skip | **FAIL 4/4** (8 on one) | **FAIL 2/2** (6 on one) | pass at bound (4) | skip | **FAIL 4/4** | **FAIL 2/2** | pass at bound |
| `sched-balance-pair` | skip | pass (4–5 ms) | pass | pass | skip | **FAIL 3/4** | pass | pass |
| `syscall-fuzz` ms | 3,200–3,230 | 3,118–3,350 | 3,087–3,130 | 3,114–3,131 | 3,628–3,718 | 3,548–3,839 | 3,600–3,687 | 3,659–3,661 |

No boot hung, timed out or panicked; every one ran all 423 self-tests.
With the instrumented build (`7d4b55f9`, 8 more boots) x86-64 also failed
`sched-balance-pair` once at two CPUs (§9).

After the fixes see §20.

## 5. VirtIO Removal Investigation

**The test.** `virtio-remove-inflight` (`kernel/core/selftest.c` registry;
`selftest_virtio_remove_inflight`, `kernel/device/devtest.c`) pins the test
thread (S25) and runs four passes over a dedicated removal disk: `held`,
`unheld`, `irq-order` and `held-inside`. Only `irq-order`
(`rm_irq_order_pass`) is CPU-count sensitive. Its contract, from its own
comment and `drivers/virtio/virtio_blk.c` `vblk_remove`: the removal must
release the queue's interrupt (`virtq_free` → MSI-X mask, `pci_msix_release`
→ `synchronize_irq`, a grace period) **before** it walks the slot table,
because a completion handler is a quiesce read-side section
(`kernel/include/kernel/interrupt.h`). The adversary is a thread holding a
read-side section (`rm_holder_main`: `quiesce_read_lock`, preemption
disabled, spinning up to `RM_HOLD_NS` = 60 ms). The evidence is stamps
from one sequentially consistent counter (`blk_test_tick`).

**Intended timeline** (the contract, confirmed against the source):

```
CPU H (holder)                       CPU R (removal)
quiesce_read_lock, stamp ENTER
                                     stamp REMOVE_START
                                     blk_unregister: gone; drain submitting
                                     reset device
                                     stamp BEFORE_IRQ
                                     virtq_free -> synchronize_irq (waits)
spin ... stamp EXIT, unlock
                                     grace period ends; stamp WALK
                                     complete leftovers -EIO; stamp BOUNDARY
                                     free DMA; memory freed at last blkdev_put
```

Required: ENTER < REMOVE_START < BEFORE_IRQ < EXIT < WALK. The old test
asserted only BEFORE_IRQ < EXIT (as "caught", retried up to four times)
and EXIT < WALK.

**Old placement** (`main`): the test thread on T; `cpu =
other_cpu_for_blk()` = the next online CPU O; the submitter and a
dedicated remover thread on O; the holder on `other_cpu_than(O)`. That
helper returns a CPU that is neither the caller's nor O, and O itself when
there is none — always, with two CPUs. So holder, remover and submitter
all ran on O; the holder disabled preemption for 60 ms; the remover ran
only after it. All 16 attempts across eight two-CPU boots: entered
~200–260 stamps after the section ended.

**Minimum CPU requirement (§7 of the brief).** The overlap needs exactly
two independently running contexts: the holder, and the removal. The
coordinator does not need its own CPU — it can *be* the removal, because
the test thread is free while the holder thread holds the section. The
separate remover thread dates from the first version (`21f4288d`, PR #199),
when the test thread itself held the section; once the holder became a
thread, the remover's justification ("so that the test thread can hold a
read-side section across it") no longer applied. The submitter's role in
this pass is to have filled the device before the removal; it does not
need to run during the section. **Two CPUs are sufficient** (conclusion A,
with the architecture correction of C).

**Second layer.** With the removal moved onto the test thread (`637f2793`),
two-CPU attempts still missed, now by 2–3 stamps (16 of 16). `blk_unregister`
(`kernel/block/blk.c`) publishes `gone` and then yields until
`bd->submitting` is zero — the drain of submits already inside the driver
(`docs/kernel/quiesce/design.md`, "Block devices"). With two CPUs the
holder shares O with the submitter; a submitter preempted mid-`blk_submit`
holds `submitting` up behind the nonpreemptible holder, and the drain
cannot complete until the section ends. Evidence: the `share-cpu` control
(parking disabled) logs "the unregister drain waited 18,578–20,338 times"
in every missed attempt on both architectures — the drain yielding for the
whole 60 ms section.

## 6. VirtIO Root Cause

CONFIRMED TEST BUG, two layers: (a) a placement helper whose fallback put
the adversary and the removal on one CPU; (b) the adversary sharing a CPU
with a thread that could be inside the driver's submit section, which the
removal is required to drain first.

Kernel behaviour verified correct throughout (§12): no new lookup reaches
the device after `blk_unregister`; existing submits are drained; the
interrupt is released and a grace period waits for the open read-side
section; only then are the slots walked; nothing completes after the
boundary; the device memory is released at the last `blkdev_put`. Fixing
layer (b) in the kernel (not draining) would break that protocol.

Fix (`637f2793`, `9a765c5c`, `kernel/device/devtest.c`):

- the removal runs on the pinned test thread; the remover thread and its
  struct are gone;
- the holder goes to `other_cpu_than(cpu)` — a third CPU when one exists,
  so the submitter keeps running beside the removal; the submitter's CPU
  otherwise — and `RM_CHECK(hold_cpu != arch_cpu_id())` asserts it never
  shares the removal's CPU;
- when the holder shares the submitter's CPU, the submitter is parked
  between submits by a `pause`/`paused` handshake before the holder starts
  and resumed after the removal (it is then refused, as in the other
  passes);
- the holder stamps its entry; the pass asserts
  ENTER < REMOVE_START < BEFORE_IRQ (overlap proven) and EXIT < WALK (the
  claim); the drain's wait count is logged.

The four-attempt retry on a missed overlap is unchanged: it is the
pre-existing guard for a host delay between the holder's entry and the
removal reaching the teardown (60 ms of slack). In the 16 post-fix boots
no attempt missed.

## 7. Scheduler Test Investigation

| Test | Property it proves | CPUs needed | Workers | Affinity | Success condition |
|---|---|---|---|---|---|
| `sched-spread` (`kernel/scheduler/smptest.c`) | **initial placement**: `pick_cpu` rotates ties, so threads created on an idle machine are not all born on one CPU | creator + at least two CPUs that can tie, i.e. ≥ 3 (derived §8) | 8, each blocked before the next is created | none (creator now pinned) | not every worker on a single CPU |
| `sched-balance-pair` | **balancing mechanism and S26**: a yielding (movable) pair sharing a CPU is separated by the pull balancer; a spinning pair (queued thread always PREEMPTED) is not | 2 (pair CPU + a receiver) | 2 per half | pinned to one CPU, then widened to all | yielding pair on two CPUs within 1 s; spinning pair together for 1 s |

Decision paths traced (all `kernel/scheduler/sched.c`):

- **Initial placement**: `thread_create` → `sched_enqueue_new` →
  `pick_cpu`: start at `g_pick_rotor++ % n`, take the strictly least
  `sched_cpu_load` among online CPUs in the affinity mask. `pick_cpu` has
  no other caller, so the rotor advances once per creation.
- **Load**: `sched_cpu_load` = `nr_running` + 1 if the CPU is running a
  non-idle thread (S29), an unlocked hint.
- **Balancing**: `sched_tick` → `balance_tick` after the tick's own unlock.
  A CPU scans every tick if idle (`load == 0 && current == idle`), else
  every `SCHED_BALANCE_TICKS` (16) ticks; it tries the busiest few CPUs at
  `busiest >= mine + 2`; `sched_migrate_from(..., min_gap 2)` re-checks the
  gap under both locks and asks the policy for a migratable thread (not
  PREEMPTED, S26).
- **Timer expiry order**: `tick_isr` (`kernel/timer/timer.c`) runs
  `run_expired` **before** the tick hook (`sched_tick`).

Conditions that legitimately keep workers on one CPU: affinity; an
eligible CPU being busier; a queued thread being PREEMPTED (S26); a gap
below two; and (found here) a receiver whose own sleeper expires on every
tick never scanning as idle.

## 8. `sched-spread`

Creator-CPU diagnostic (`7d4b55f9`): in all eight instrumented boots the
creator stayed on one CPU and received **no** worker; the pile was always
on the CPU after the creator's in scan order (creator CPU 1 → 5 on CPU 2;
creator CPU 2 → 5 on CPU 0; two CPUs → all 8 on the other).

Mechanism: since S29 placement counts the creator's running thread, so the
creator's CPU never ties with an idle one. The scan's start rotates, but
when it starts on the creator's CPU, the first idle CPU after it wins.
With the creator on CPU 0 and three CPUs, starts 0 and 1 both choose CPU 1
and start 2 chooses CPU 2: 2:1, i.e. 5–6 of 8 depending on the rotor's
phase. Four CPUs: CPU 1 gets 2 turns in 4, 4 of 8 — the old bound exactly,
so four-CPU passes had no margin. Two CPUs: one eligible CPU, all 8.

History: the rotation (`d2babd03`) was written when `pick_cpu` read
`nr_running`, under which every CPU tied ("4 of 4 CPUs"). The load-balancer
unit's S29 changed what placement reads; the test's premise ("every CPU
ties") and its bound were not re-derived.

Classification: CONFIRMED TEST BUG at three CPUs (and fragile at four); a
TEST TOPOLOGY ASSUMPTION at two CPUs, now encoded. **Two CPUs cannot
represent the scenario**, proved from the policy: with one eligible CPU,
least-loaded placement sends every worker there whatever the tie rule, so a
working rotation and a broken one produce identical placements.

Fix (`637f2793`): the creator is pinned (`thread_pin_self`); eligible CPUs
are the online CPUs other than the creator's; fewer than two eligible skips
with that reason; the assertion is the defect's own signature, `distinct
< 2` (every worker on one CPU), and the result line reports the creator's
share. The `worst > N/2` bound is removed rather than re-derived: under the
current policy a correct rotation legitimately yields 6 of 8 on one CPU at
three CPUs, so any count bound would encode the rotor's skew rather than
the property.

Policy-quality note (EXPECTED POLICY BEHAVIOR, not changed): the tie
rotation is not uniform when a non-tied CPU's turn passes to its
successor. A thread on CPU k creating blocking threads biases them toward
CPU k+1. Nothing documented promises uniformity (`docs/kernel/scheduler/design.md`
§3b; `d2babd03` says only "ties rotate"); recorded for the plan.

## 9. `sched-balance-pair`

Baseline: x86-64 separated in 4–5 ms in all four two-CPU boots; AArch64
"NOT separated after 1002–1003 ms" in three of four and after 511 ms in
the fourth. With the balancer's existing counters logged over the window
(`7d4b55f9`), x86-64 failed too (1 of 2) — two CPUs failed in 6 of 14 boots
across both architectures.

| Window | Scans | Pulls | No candidate | Refused |
|---|---|---|---|---|
| every failing window (3) | 31–32 | 0 | all | 0 |
| every fast pass (4 at two CPUs, plus three-CPU) | 1 | 1 | 0 | 0 |

32 scans per second is two CPUs × 250 Hz / 16: only busy-CPU periodic
scans ran. The receiver never once scanned as idle, and its busy-path scan
reads `mine` = 1 (the test thread) against the pair's 2: `2 < 1 + 2`, no
candidate. The test thread is that receiver: it polled with
`thread_sleep_ms(2)`; timers expire only in `tick_isr`, before
`balance_tick`, so a sleep armed within a tick of its deadline is queued
before the balancer asks. After each wake the thread re-arms at once, so
the next deadline (2 ms) precedes the next tick (4 ms): the poll
phase-locks to the tick. Only the first sleep after the widen starts at a
random phase, an idle tick about half the time — matching 6 of 14. With
three or more CPUs another CPU is idle and pulls on its first tick.

Classification: CONFIRMED TEST BUG (observer perturbation — the counter
pattern in `docs/testing/flakes.md`'s "a probe must not wake what it
counts"). The balancer behaved exactly as documented.

Fix (`637f2793`): each `bal_worker` may carry a `left` completion and a
`home` CPU; it completes `left` the first time its loop runs elsewhere.
`balance_pair` blocks in `wait_for_completion_timeout(&left, 1 s)` — one
timer, at the deadline — and polls for "two real CPUs" only once a worker
has moved. Both halves wait the same way, so the yield remains the only
difference. Budget (1 s) and both assertions unchanged. After the fix all
two-CPU windows show 1 scan, 1 pull, 0–5 ms.

Policy note (EXPECTED POLICY BEHAVIOR, recorded): a CPU that runs any
thread sleeping in tick-aligned periods shorter than a tick never balances
as idle, and its periodic scan requires a difference of two; so one
movable extra thread elsewhere can wait indefinitely while that CPU is
nearly idle. This follows from the documented S27–S29 rules and the tick
order; whether to sample idleness differently is a policy decision for a
later increment.

## 10. Scheduler Root Cause(s)

No scheduler defect. Both scheduler failures are test defects:
`sched-spread`'s premise and bound predate S29; `sched-balance-pair`'s
observer occupied the only receiver. Invariants reviewed against the
evidence and source:

| Invariant | Evidence |
|---|---|
| runnable tasks are not lost | every worker in both tests ran and was joined; `threads_settle` passed; no stall-detector line ("READY on this queue") in any boot |
| affinity respected | pair workers ran only on their pinned CPU until widened (the premise requires both to report it); no worker ran outside its mask |
| no duplicated execution / runqueue ownership / migration locking | `migrate_locked` KASSERTs and lockdep were silent in all boots; no new kernel path |
| balancing makes progress where promised | 1 pull within a tick whenever a receiver was idle (§9) |
| idle CPUs receive work where policy allows | as above; the policy-quality notes in §§8–9 are preferences, not violations |

Not changed: `pick_cpu`, `balance_tick`, `sched_migrate_from`, quiescent
points. PR #308 is not implicated: the mechanisms are independent of the
spin-contention code, and a pre-#308 comparison was not needed because
current-`main` evidence names each mechanism.

## 11. Lockdep Observations

All boots ran with `LOCKDEP=1`. In every serial log the only lockdep
reports are the suite's own `expected report:` lines — 14 per one-CPU
boot, 17 per multi-CPU boot, identical across repetitions and before and
after the fixes. No unexpected lock-order, IRQ-safety, sleep-in-atomic,
recursion or bad-release report. (This says nothing about scheduler
policy.) The new test code takes no new lock: the submitter park is two
atomic flags and `sched_yield`; the pair signal is one `complete()` from a
preemptible thread.

## 12. Quiescence/Lifetime Observations

The removal's order (§5 timeline) holds in every passing attempt, at every
CPU count: section entry before the removal started, the teardown entered
with the section open, the walk after the section ended, nothing completed
after the boundary, `releases` unchanged until the test's last
`blkdev_put`. The `teardown-late` control (interrupt released after the
walk, the original defect) fails the EXIT < WALK check on both
architectures, so the pass still detects a broken grace-period order.
Holder sections remain bounded (60 ms) with preemption disabled and IRQs
enabled; no tick-gap or lockup report. No quiescent-point semantics
changed. The scheduler fixes do not touch `schedule()`, idle, preemption
or migration paths.

## 13. Timing Investigations

- **AArch64 `syscall-fuzz`**: fixed seed `20260905`, 20,000 calls. Every
  boot in this session: AArch64 3,545–3,921 ms, x86-64 3,087–3,414 ms,
  budget 8,000 ms; no dependence on CPU count. The recorded excursions
  (8,305, 9,641, 8,240 ms; `docs/testing/flakes.md`) were not reproduced
  in 35 AArch64 matrix boots plus one chaos boot. Both earlier sightings were beside a concurrent
  x86 boot or in a probe run; this session never ran two boots at once.
  TIMING SENSITIVITY, cause still unestablished; budget unchanged.
- **`thrtest`**: its loop counts (2,000 errno rounds, 200 TLS yields, 400
  heap iterations) are workload sizes asserted per iteration, and its
  waits use clock deadlines (`PAIR_BUDGET_NS`, `CV_TIMED_BUDGET_NS`,
  `HEAP_BARRIER_NS`). No speed assumption found that encodes correctness.
- **`cwdtest`**: the `--held` modes wait for the seam state with
  `spins > 200000` yields as the bound (`userland/tests/cwdtest.c`) — a
  count standing in for a duration, the shape `docs/audit/next-subsystem-suite-waits.md`
  converted elsewhere. Not failing in any boot here; TIMING SENSITIVITY
  (latent), left for the suite-waits follow-up.
- **`net-bench`**: 1,212–3,487 ms in every boot; the 71 s run is not
  reproduced. UNRESOLVED.
- **slirp resets**: none observed; nothing to add.
- **AArch64 virtio-console**: the harness requires the boot-complete line
  on the virtio console; 0 failures in 35 AArch64 captures (68 in all). UNRESOLVED
  (not reproduced); the distinction between console transport loss and a
  kernel test failure stands.

## 14. AArch64 Observations

Both scheduler mechanisms and both VirtIO layers reproduce on AArch64 and
x86-64 alike; no architecture-specific cause. `sched-balance-pair`'s
higher AArch64 failure rate (5/8 vs 1/6) is consistent with the phase of
the first post-widen sleep differing by architecture, not with a different
mechanism: the failing counters are identical. AArch64 boots run 5–10 s
slower in total and `syscall-fuzz` ~15% slower. The fixes were validated
on both (§§19–20).

## 15. Kernel Bugs Confirmed

None.

## 16. Test Bugs Confirmed

1. `virtio-remove-inflight` `irq-order`: holder and remover co-located at
   two CPUs (placement fallback).
2. `virtio-remove-inflight` `irq-order`: holder co-located with a submitter
   that can be inside the driver's drained submit section.
3. `sched-spread`: premise and bound predating S29 (fails at three CPUs,
   zero margin at four); two-CPU premise not encoded.
4. `sched-balance-pair`: observer polling on the only receiving CPU.

Also fixed: the `irq-order` result line exceeded `KLOG_LINE_MAX` (256)
after the extra stamps; `other_cpu_than`'s comment said it avoided CPU 0
(it avoids the caller's CPU).

## 17. Environment/QEMU Findings

None attributable. The host was loaded throughout (two other VMs, a
browser); results were nonetheless deterministic for every mechanism
above. Recorded baseline noise, not investigated: "unhandled interrupt
vector" warnings scale with the CPU count (2/4/6/8–9) and follow the IRQ
writer/unregister tests; identical per configuration.

## 18. Changes Implemented

| Commit | Files | Change |
|---|---|---|
| `7d4b55f9` | `kernel/scheduler/smptest.c`, `tools/two-cpu-matrix.sh` | diagnostics: balancer counter delta in `sched-balance-pair`, creator CPU in `sched-spread`; the boot-matrix runner |
| `637f2793` | `kernel/device/devtest.c`, `kernel/scheduler/smptest.c` | the three test fixes (§§6, 8, 9) |
| `9a765c5c` | `kernel/device/devtest.c`, `tools/two-cpu-probe.py` | submitter parking; drain-wait diagnostic; negative-control probe |
| `9d6a0e0d` | `kernel/device/devtest.c` | the result split over two log lines |

No kernel (non-test), driver, scheduler-policy or synchronization code
changed. No timeout, worker count, budget or retry count changed.

## 19. Regression Tests

`tools/two-cpu-probe.py` builds HEAD with one deliberate defect in a
throwaway git worktree and requires the named test to fail for the stated
reason. All nine required outcomes were seen (`out/two-cpu-probe/*/`):

| Mode | Defect | Arch, CPUs | Required failure | Seen |
|---|---|---|---|---|
| `teardown-late` | `vblk_remove` releases the interrupt after the slot walk (the original defect) | x86-64 2, AArch64 2 | `held_until < walk` | yes, both |
| `share-cpu` | no submitter parking | x86-64 2, AArch64 2 | `++order_attempts < 4`, and "drain waited N" with N > 0 | yes, both (N = 18,578–20,338) |
| `no-rotate` | `pick_cpu` scans from CPU 0 every time | x86-64 3, x86-64 4, AArch64 3 | "piled onto one CPU" | yes (8 of 8 on one CPU each) |
| `no-balance` | `SCHED_BALANCE=0` | x86-64 2, AArch64 2 | `y == PAIR_APART` | yes, both |

Together with the unchanged-`main` baseline (the old tests failing for
the mechanisms above), each fixed test is shown to (a) reach its intended
state — the overlap stamps, the creator's CPU, the 1-scan/1-pull window —
and (b) fail when the property it guards is broken.

## 20. Validation Results

Final tree `9d6a0e0d`, run as one chain after `gmake clean` for both
architectures (`out/final/*.log`; matrix boots `out/matrix/final-*`).
Every row ran; the verdict is the tool's own line.

| Step | Command | Result |
|---|---|---|
| Clean | `gmake ARCH=<arch> clean` | rc 0, both |
| Debug build | `gmake -j6 ARCH=<arch> all` | rc 0, both (277 compile steps on x86-64: a full build) |
| x86-64, 1 CPU | `tools/two-cpu-matrix.sh x86_64 final 1 1` | PASS 116.1 s |
| x86-64, 2 CPUs ×3 | `... x86_64 final 3 2` | PASS 120.7 / 126.6 / 126.9 s |
| x86-64, 3 CPUs | | PASS 128.1 s |
| x86-64, 4 CPUs | | PASS 128.6 s |
| AArch64, 1 CPU | | PASS 128.7 s |
| AArch64, 2 CPUs ×3 | | PASS 131.4 / 135.1 / 139.0 s |
| AArch64, 3 CPUs | | PASS 134.9 s |
| AArch64, 4 CPUs | | PASS 150.0 s |
| Release build + boot | `gmake -j6 ARCH=<arch> BUILD=release all test` | x86-64 PASS 16.7 s, AArch64 PASS 20.3 s |
| Chaos migrator (4 CPUs) | `gmake -j6 ARCH=<arch> BUILD=debug test-chaos` | x86-64 PASS 129.9 s, AArch64 PASS 131.8 s; all 423 self-tests, the three changed tests included |
| Host tests | `gmake -j6 ARCH=x86_64 host-test` | rc 0 |
| Static analysis | `gmake -j6 ARCH=<arch> analyze` | "static analysis: clean", both. Two analyzer warnings print in code this branch does not touch (`sched.c:662` shift width; `sched-balance-affinity`'s `t[2]` read uninitialised on a failed first create in `smptest.c`); pre-existing and not gating |
| Lockdep | every debug boot above, `LOCKDEP=1` | only `expected report:` lines: 14 at one CPU, 17 otherwise, identical to baseline |
| Negative controls | `tools/two-cpu-probe.py` | 9 of 9 required failures (§19) |

Counted over the whole session on the fixed tree (`637f2793` onward, 40
debug boots): the three changed tests passed in every boot at one, three
and four CPUs; at two CPUs `sched-spread` skipped with its reason in all
20 boots and `sched-balance-pair` passed in all 20; `virtio-remove-inflight`
passed in all 12 two-CPU boots from `9a765c5c` on (it failed in the 8
between `637f2793` and `9a765c5c`, §5). Counted by script over
`out/matrix/{fix,fix2,final}-*.serial`. In the final matrix the two-CPU `irq-order` lines read "submitter
parked beside the holder; drain waited 0 times", and the three- and
four-CPU lines "on its own CPU", drain waits 3–10.

Not run: physical hardware; `test-gic`, `test-guard`, `test-crash`,
`test-wxn`, `test-harness-retry`, `fuzz` and `reproducible` (CI jobs whose
subjects this branch does not touch); CPU counts above four; remote CI.

## 21. Remaining Uncertainty

- The `syscall-fuzz`, `net-bench` and virtio-console observations were not
  reproduced, so their causes remain unestablished.
- The two-CPU `sched-balance-pair` failure rate (6 of 14) is a small sample;
  the mechanism is shown by the counters, not by the rate.
- The fixed `irq-order` pass still relies on the removal reaching the
  teardown within the holder's 60 ms; on a heavily loaded host a miss is
  retried (bounded at four), as before.
- Placement skew (§8) and tick-aligned-sleeper idle masking (§9) are
  policy observations, not measured for their effect on real workloads.
- No physical hardware; QEMU TCG only. No CPU counts above four.

## 22. `docs/plan.md` Changes

- §2: the two-CPU VirtIO and scheduler items are checked, each with its
  root cause and a link here; the original wording is kept. The
  syscall-fuzz, timing and virtio-console items stay open, with this
  session's non-reproduction recorded in place.
- §4: a new conditional item records the two policy observations (§§8–9)
  without proposing a change.
- §12: the CPU-matrix item records one- to four-CPU local passes and that
  CI still boots only four.
- "Suggested next increments": item 1 struck through as done, with the
  two-CPU CI boot named as the way to keep it done.

Also updated: inventory §7's two-CPU entry (struck through, resolved, kept),
the October 4 entry in `docs/testing/flakes.md` (attribution appended), and
`docs/history/subsystem-units.md` (one entry).

## 23. Recommended Next Increment

1. Add a two-CPU (and possibly three-CPU) debug boot to CI, now that it
   passes: without it, a placement fallback like the one above can regress
   silently, since every CI boot uses four CPUs.
2. Then the plan's next item: quiescence memory-order models and targeted
   negative controls.
3. Separately, if desired: decide whether placement should rotate
   uniformly among tied CPUs, and whether idle balancing should see through
   tick-aligned sleepers (§§8–9) — policy changes needing their own
   measurement.

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
  suite's own `expected report:` lines (14 at one CPU, 17 at two or more; first
  counted as 15 with a broader pattern, corrected at 17:25,
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

### 16:30 — First fix (`637f2793`), 18 boots

Logs: `out/matrix/fix-<arch>-smp<N>-<rep>.serial`.

| Arch | CPUs | Boots | Harness | Notes |
|---|---|---|---|---|
| x86-64 | 1 | 1 | PASS | `sched-spread` skips: "0 CPU(s) besides the creator's" |
| x86-64 | 2 | 4 | 4 FAIL | only `virtio-remove-inflight` (below) |
| x86-64 | 3 | 2 | 2 PASS | spread "2 of 3 CPUs, at most 6", creator got 0 |
| x86-64 | 4 | 2 | 2 PASS | spread "3 of 4, at most 4", creator got 0 |
| AArch64 | 1 | 1 | PASS | as x86-64 |
| AArch64 | 2 | 4 | 4 FAIL | only `virtio-remove-inflight` |
| AArch64 | 3 | 2 | 2 PASS | spread "2 of 3, at most 5" |
| AArch64 | 4 | 2 | 2 PASS | spread "3 of 4, at most 4" |

- `sched-balance-pair`: separated in 0–5 ms in all 16 multi-CPU boots,
  including all eight two-CPU boots (each: 1 scan, 1 pull). The spinning
  half stayed together everywhere.
- `sched-spread`: skips with the stated reason at one and two CPUs; passes
  at three and four with the creator's CPU receiving no worker.
- `virtio-remove-inflight` at three and four CPUs: the five stamps in
  order in all eight boots, e.g. "a read-side section opened at 191 on cpu
  2; the removal started at 192 on cpu 0 and entered the queue teardown at
  196 with it open, the section ended at 197 and only then did the slot
  walk begin".
- `virtio-remove-inflight` at two CPUs still failed all 16 attempts, but
  **differently**: the teardown was now entered 2–3 stamps after the
  section ended (e.g. entered 431, ended 429), against ~200–260 at
  baseline. The removal now started promptly on the test thread's CPU and
  was held up *inside* the removal before the teardown stamp.

Mechanism: `vblk_remove` begins with `blk_unregister`
(`kernel/block/blk.c`), which publishes `gone` and then spins until
`bd->submitting` is zero — the drain of submits already inside the
driver, a documented part of the lifetime protocol
(`docs/kernel/quiesce/design.md`, "Block devices"). With two CPUs the
holder runs on the submitter's CPU. When the submitter is preempted at a
slice end inside `blk_submit` (most of its time is spent there; an MMIO
notify is a TCG exit), `submitting` stays raised, the nonpreemptible
holder takes the CPU, and the drain cannot finish until the section ends.
The teardown stamp follows a few stamps later, as observed. With three or
more CPUs the submitter has its own CPU and finishes its submit at once.

Classification: **CONFIRMED TEST BUG** (second half). The kernel is right
to wait: weakening the drain would let `vblk_remove` free state under an
in-progress submit. Fix (`9a765c5c`): when the holder must share the
submitter's CPU, the submitter is first parked between submits through a
`pause`/`paused` handshake, so it is not counted in `submitting`, and
resumed after the removal (it is then refused, as in the other passes).
The drain's wait count (`blk_test_unregister_spins`, existing) is now
logged with each attempt. A negative control removes the parking
(`tools/two-cpu-probe.py --mode share-cpu`) and must fail with the drain
seen waiting.

### 17:20 — Parking fix (`9a765c5c`), 10 boots

Logs: `out/matrix/fix2-<arch>-smp<N>-<rep>.serial`.

| Arch | CPUs | Boots | Harness | `irq-order` attempts needed |
|---|---|---|---|---|
| x86-64 | 2 | 3 | 3 PASS | 1 each |
| x86-64 | 3 | 1 | PASS | 1 |
| x86-64 | 4 | 1 | PASS | 1 |
| AArch64 | 2 | 3 | 3 PASS | 1 each |
| AArch64 | 3 | 1 | PASS | 1 |
| AArch64 | 4 | 1 | PASS | 1 |

These are the first full-suite two-CPU passes in this investigation: all
423 self-tests and the post-test harness. The stamps at two CPUs, e.g.
x86-64: section opened 146 on CPU 1, removal started 147 on CPU 0, teardown
entered 149, section ended 150, walk 151; AArch64: 214 (CPU 1), 215 (CPU
0), 217, 218, 219. No `retrying` line in any of the ten boots.

The new result line exceeded the log's 256-byte line (`KLOG_LINE_MAX`)
and lost its tail (counts and the submitter's placement); split over two
lines in `9d6a0e0d`. Nothing asserted was in the lost tail.

### 17:25 — Negative controls and final validation

Nine controls, nine required failures (§19). Final chain on `9d6a0e0d`
from `gmake clean`: twelve matrix boots, release and chaos boots on both
architectures, `host-test` and `analyze`, all passing (§20). One
correction made while counting: one-CPU boots carry 14 `expected report`
lockdep lines, not 15 as first written (a broader pattern had matched
other lines); baseline and fixed trees agree at 14.
