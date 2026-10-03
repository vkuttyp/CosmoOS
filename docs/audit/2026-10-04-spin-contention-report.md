# Observed spin contention — 2026-10-04

This increment continues the lockdep milestone on
`milestone/lockdep-spin-contention`, based on PR #307's `2ee2d783`.
[CI run 37155497752](https://github.com/vkuttyp/CosmoOS/actions/runs/37155497752)
passed both architectures at that baseline. PR #307 was still open when
this branch was created; no merge is implied. User changes to
`reviewer.yaml` and `prompts/Next-Milestone.md` are outside the increment.
The pre-implementation scope is recorded in the October 3 lock-discipline
audit's October 4 addendum.

## Phase 1: observation boundary

A self-test-only per-CPU atomic pointer names the innermost actual spin
wait. `lock_common` publishes after its first failed exchange and restores
the previous pointer after acquisition. Only that CPU writes the slot,
with preemption disabled. An interrupt that also contends temporarily
replaces the interrupted wait; an uncontended nested lock leaves it alone.
Remote tests compare identity without dereferencing the pointer and retain
their locks and pinned workers until all accesses have finished.

The observer allocates nothing, takes no tracked lock, changes no public
lock layout or module ABI, and is not exported to modules. SELFTEST=0
removes its storage and instrumentation, independently of LOCKDEP. It
provides a test rendezvous, not a production ownership/wait-for graph,
a stopped-owner detector, or NMI writer support. Measured acquisitions
include its instrumentation and the owner's polling overhead.

## Phase 2: contention-test ordering

The old `lockdep-contention` held a remote lock for 20 ms and scheduled a
callback for 5 ms later. A delayed waiter could miss that window; an early
callback did not prove execution during contention. The holder now waits
for the callback's observed contention, bounded by 1 s. An early callback
rearms without taking its test lock.

Once the callback observes L's failed exchange, it contends on a second
lock held by the remote owner. The owner observes that nested wait and
releases the second lock. The callback verifies restoration of the outer
wait, then takes M. Both observed waits are required for success. The
existing M-to-L order check still requires the IRQ-usage report rather
than a phantom inversion. The test retains PR #307's release/cancel/join
order on every returning failure; missing readiness or exit fails stop.

## Phase 3: contended spin measurements

`lockdep-spin-bench` exercises both public plain and irqsave acquisitions
with identical workloads under LOCKDEP=0/1. It pins the owner and a new
waiter per round to distinct CPUs. UP logs a skip, since a nonpreemptible
spin owner cannot safely wait for another thread on its own CPU.

For each path, two warmups precede nine samples. The owner observes a
failed exchange before holding another 1 ms, writing protected data and
releasing. The waiter times only its acquisition call on its own CPU.
Each round checks exclusion, CPU ownership, data handoff, cleared wait
state, and the expected IRQ/preemption state during and after ownership.
The owner joins the waiter before checking failure conditions. A missing
exit panics while retaining the live thread and stack probe.

These are total acquisition times under a controlled workload. They
include the hold, observer, scheduling, interrupts and validator work;
the 1 ms is not subtracted. They neither isolate lockdep overhead nor
establish worst-case or native-hardware latency. No timing ratio is a pass
criterion. Global graph state is never reset and class capacity is unchanged.

The owner-side busy guards use `clock_raw_ns` elapsed on their pinned
CPU. Migration-safe deadlines can fall back to a tick counter on hosts
without a common clock; if both test CPUs have IRQs masked, that counter
cannot enforce a bound. These loops cannot migrate and therefore use the
hardware-backed local clock. The readiness and exit waits retain their
existing context-appropriate bounds. A two-CPU, forced-non-common-clock
probe withholds the irqsave wait observation to check this failure path.

## Phase 4: validation

The first exploratory AArch64 SMP build, before adding the nested-wait
assertion, passed 423 self-tests and the full harness. Its logs remain
`out/spin-contention-aarch64-on4{,-result}.log`.

`tools/spin-contention-probe.py` mutates temporary clones only. Its early
callback control establishes a callback before contention and delays the
waiter another 50 ms. Restoring a 20 ms holder guard must fail after safe
cleanup; the new protocol must pass. Missing-observer and lost-restoration
controls exercise the observation boundary. Benchmark controls require
safe cleanup after a missed observation or fail-stop retention after a
missing exit. Exact diagnostics, panic shutdown and failure exit are
required, rather than accepting any crash. The existing cleanup probe
now overlays the observer sources and uses a fresh retained output path.

The first matrix AArch64 LOCKDEP=0 SMP boot completed all 423 tests
and the shell but failed `syscall-fuzz`'s duration budget: 8,240 ms against
8,000 ms, with a watchdog dump. The child completed the expected 20,000
calls; `kmain` was waiting on its completion. Both spin measurements passed.
The shape is already documented in the flake history, but no cause is
assigned from that resemblance alone. Logs remain
`out/spin-final-aarch64-off4{,-result}.log`; an unchanged-image retry is
kept separately as `out/spin-final-aarch64-off4-retry{,-result}.log`:
the full harness passed, with `syscall-fuzz` taking 3,601 ms.
No unrelated budget was adjusted.

The six-configuration matrix exercised the observer, nested-wait test and
benchmark before the final busy-guard clock refinement. All six have
passing full boots (the AArch64 off/SMP retry is described above). The
first targeted build of the refinement referenced private `clock_time_ns`
and correctly failed compilation. The guards were corrected to the
existing public `clock_raw_ns`, with its same-CPU contract documented.
The build failure remains in
`out/spin-probe-aarch64-early-initial-build-result.log` and
`out/spin-probe-aarch64-early/run-loryx6yk/build.log`; it is not a test pass.
Targeted probes validate the refined source. Full two-CPU boots exposed
additional suite limitations described below; final full-suite validation
uses the established four-CPU configuration.

The six matrix configurations each completed 423 self-tests and the
post-test harness. The UP runs explicitly skipped cross-CPU spin timing.
Logs are `out/spin-final-<arch>-<configuration>{,-result}.log`, with the
separate retry suffix above for AArch64 off/4.

| Architecture | Configuration | Full harness |
|---|---|---|
| AArch64 | LOCKDEP=1, 4 CPUs (`on4`) | PASS, 139.1 s |
| AArch64 | LOCKDEP=1, 1 CPU (`on1`) | PASS, 135.1 s |
| AArch64 | LOCKDEP=0, 4 CPUs (`off4-retry`) | PASS, 125.3 s after the retained failure |
| x86-64 | LOCKDEP=1, 4 CPUs (`on4`) | PASS, 126.1 s |
| x86-64 | LOCKDEP=1, 1 CPU (`on1`) | PASS, 120.3 s |
| x86-64 | LOCKDEP=0, 4 CPUs (`off4`) | PASS, 109.7 s |

After the raw-clock guard correction, the final implementation
(`95dc0286`) passed all 423 self-tests and the full harness with four CPUs
and LOCKDEP=1 on both architectures: AArch64 in 135.3 s and x86-64 in
125.2 s. Logs: `out/spin-refined-<arch>-on4.log` and
`out/spin-probe-<arch>-final4-result.log`. These runs used
`gmake -j4 ARCH=<arch> LOCKDEP=1 QEMU_SMP=4 test BOOT_LOG=<log>`.

Both `gmake -j4 ARCH=<arch> BUILD=release kernel` builds passed.
`llvm-nm` found `spin_test_waiting_on` and `g_test_waiting` in both debug
kernels and neither in either release kernel; the check is retained in
`out/spin-observer-symbols.log`. Release defaults to SELFTEST=0/LOCKDEP=0;
the observer's source guards depend only on SELFTEST. Release kernels were
built and inspected, not booted in this increment. Python syntax checks
for both probe tools and `git diff --check` also passed. No new remote CI
run is claimed for this local branch.

All twelve controlled probes passed their required outcome on the refined
source. Except for the positive `early` control, success means the exact
deliberate failure and shutdown were observed; these are not full-suite
passes. Each tool prints its retained artifact directory in
`out/spin-probe-<arch>-<mode>-result.log`.

| Probe | Architectures | Required outcome |
|---|---|---|
| `early` | AArch64, x86-64 | Callback before contention; delayed contender still passes and cleans up |
| `early-short-hold` | AArch64, x86-64 | Restored 20 ms guard fails `timer_ran` after cleanup |
| `nested-restore` | AArch64, x86-64 | Clearing the outer observation trips the callback assertion |
| `missing-wait` | x86-64 | Withheld observer fails `timer_ran` after cleanup |
| `bench-unobserved` | x86-64 | Observation guard expires, releases and joins before failing |
| `bench-irqguard` | x86-64, 2 CPUs | Same cleanup with IRQs masked and forced non-common clock |
| `bench-exit` | x86-64 | Missing exit panics with thread and probe retained |
| Existing `missed-timer` cleanup probe | AArch64, x86-64 | No held lock or live stack timer on returning failure |

The x86-64 short-hold probe needed the harness's existing firmware retry:
its first attempt produced no loader banner in 30 s. The second boot
reached the required kernel diagnostic. This was not a kernel-test retry.
The short-hold mutation restores only that guard, not the whole historical
test implementation.

Two AArch64 full-suite attempts with two CPUs passed the new contention
checks but failed elsewhere. Both exhausted `virtio-remove-inflight`'s
four overlap attempts and placed all eight `sched-spread` workers on one
CPU; the retry also failed `sched-balance-pair`'s yielding-pair assertion.
The VirtIO test's `other_cpu_than` excludes the calling CPU and the
submitter CPU, then falls back to the submitter CPU when no third CPU
exists. Thus its nonpreemptible read-side holder and remover share a CPU;
the remover cannot enter teardown until that holder stops. All logged
teardown stamps followed the corresponding section-exit stamps.
The scheduler failures have not been root-caused or compared with a
baseline two-CPU boot. No two-CPU full-suite pass is claimed, and these
tests' bounds and assertions were not changed. The two-CPU forced-clock
spin guard probe above remains a separate passing result. Logs:
`out/spin-final-aarch64-on2{,-retry}.log` and
`out/spin-probe-aarch64-final2{,-retry}-result.log`.

Representative four-CPU acquisition measurements from the matrix are
below (nanoseconds; two warmups, nine measured acquisitions per path).
The AArch64 disabled values come from the passing retry.

| Architecture | LOCKDEP | Path | Minimum | Median | Maximum |
|---|---|---|---:|---:|---:|
| AArch64 | 1 | plain | 1,002,992 | 1,004,000 | 1,036,000 |
| AArch64 | 1 | irqsave | 1,004,000 | 1,006,000 | 1,074,000 |
| AArch64 | 0 | plain | 1,004,000 | 1,082,992 | 1,466,992 |
| AArch64 | 0 | irqsave | 1,006,000 | 1,018,000 | 4,110,000 |
| x86-64 | 1 | plain | 1,006,983 | 1,042,405 | 6,206,861 |
| x86-64 | 1 | irqsave | 1,002,935 | 1,009,007 | 1,052,525 |
| x86-64 | 0 | plain | 1,000,501 | 1,001,502 | 1,002,504 |
| x86-64 | 0 | irqsave | 1,002,504 | 1,003,505 | 2,938,407 |

The 6.2 ms x86 plain sample and slower disabled AArch64 medians illustrate
why these observations cannot support an isolated lockdep overhead ratio.

## Remaining scope

General callback-wait dependencies, raw IRQ save/restore pairing, global
held-state snapshots, general NMI/#MC writers and cross-CPU raw-lock wait
cycles remain open. So do stopped-owner spin waits, broader contention
workloads, priority-inheritance measurements, graph-size sweeps and
native-hardware costs. This increment does not complete the whole milestone.
