# SMP: Testing

All SMP self-tests live in `kernel/scheduler/smptest.c` and run from
thread 0 on CPU 0 after `smp_init`, with the tick running on every CPU.
`make test` boots QEMU with `-smp 4` (`QEMU_SMP`, default 4);
`QEMU_SMP=1 make test` runs the same suite on one CPU, where each test
checks the single-CPU behaviour instead of skipping silently.

## Self-tests

Every test that creates threads joins them and then requires
`threads_settle(before)`: the thread count returns to its starting value
within 200 ms (the reaper frees exited threads asynchronously, SMP12).

### `smp-online`
| Check | Proves |
|---|---|
| `cpu_online(0)`; online count ≥ 1 | the boot CPU is accounted |
| online count == MADT processor count (unless > `CONFIG_MAX_CPUS`) | every reported AP came up |
| per CPU: `percpu_get(c)->cpu_id == c`, `rq`, `idle`, `timers` non-NULL | per-CPU initialisation completed |
| `boot_stack == 0` on every AP | idle threads freed their bootstrap stacks (SMP5) |

Single CPU: passes with count 1.

### `smp-affinity`
One thread per CPU via `thread_create_on(..., CPUMASK_OF(c))`; each
records `arch_cpu_id()` on entry, sleeps 2 ms, records it again
(`seen == c * 100 + c`). Also `thread_create_on` with an empty mask
returns NULL. Proves placement honours the mask and threads do not move.
Single CPU: one thread, CPU 0.

### `smp-parallel`
One spinning thread pinned per CPU for 50 ms, all counting iterations
(each publishes its count with an atomic store, and counts a stray for
any iteration that ran off its CPU). Every counter > 0 and no strays;
with more than one CPU, an observer thread pinned to CPU 0 watches CPU
1's counter and must see it advance: with both pinned and neither
straying, an advance seen from CPU 0 is two CPUs executing at once. This
replaced a ratio (`CPU 1 > CPU 0 / 4`) that said the same thing only on a
host sharing its CPUs evenly and failed on a correct kernel
(`docs/testing/flakes.md`). Proved: an AP that counts only while CPU 0
sleeps fails at `obs.advanced`. Single CPU: only the `> 0` and stray
checks apply.

### `smp-call`
`smp_call_function_single(c, record_cpu, &got)` for every online CPU;
`got == c`. There is no bound on the round trip: the call has its own,
one second, and panics past it (`ipi.c`), so the return is the check; a
`< 100 ms` used to follow and could distinguish nothing (a tick-driven
reply would be 4 ms). With more than one CPU, `ipi_count(IPI_CALL)` on
CPU 0 stays 0 because a call to the current CPU runs directly. Proves the mailbox, the IPI vector, and the
target-side handler.

### `smp-shootdown`
Map a RAM page through `vm_map_phys`, touch it on every CPU through
`smp_call_function_single` (loading the translation into each TLB), then
`vm_unmap_phys` and compare `arch_mmu_shootdown_stats` before and after:
`initiated` +1 and `acks_received` +(online − 1) on CPU 0 for the
single-chunk region; the address no longer translates. Single CPU:
`initiated` unchanged (local-only path). Proves SMP8's IPI leg and the
acknowledgement accounting; it does not prove a stale translation is
gone, which would require an access that faults.

### `smp-wake`
A thread pinned to another CPU (the target) blocks in `semaphore_down`.
The test posts once two things read true:
- the waiter's state reads `THREAD_BLOCKED` (a post that finds no waiter
  takes the fast path and sends no IPI);
- the target reads idle, `sched_cpu_load(target) == 0`.

The idle wait is needed because `waitqueue_prepare` sets `BLOCKED` before
the waiter switches out. A post in between finds the waiter itself still
current, at equal priority, so `sched_wake` owes no IPI. That was the
one CI failure (`docs/audit/next-subsystem-smp-wake.md`). A load of 0
cannot be read while the waiter is current, since the hint reads high,
never low (S29).

The claim is read from the kernel's own record, which `sched_wake` keeps
on the woken thread in debug builds:
- `wake_resched`: whether the wake asked the target to reschedule;
- `wake_ipi_base`: the target's handled `IPI_RESCHEDULE` count
  (`ipi_count_on`), taken under the target's lock before it sends.

The waiter copies both when it returns, with the count again. The test
asserts `on_cpu == target`, wake time ≥ post time, the wake asked for a
reschedule, and the target handled one after the snapshot.
- **After the snapshot, not after the block:** the waiter's count before
  it blocked was a superset. Any reschedule IPI to that CPU during the
  block raised it, including one owed to another wake.
- **"After the wake", not "this wake's":** reschedule IPIs carry nothing
  and coalesce, so no count can say which wake sent the one the target
  handled.
  - A wake that sends none fails.
  - A wake whose IPI is lost while another CPU's reaches the target
    within the wake's own latency would pass. Greptile named that
    residue on #263.
  - The handler's increment is atomic (`count` in `ipi.c`), because
    `ipi_count_on` reads it from another CPU.
- **If a thread became runnable on the target between the idle read and
  the post,** the wake asks for nothing. The record says so, and the
  round is repeated with a fresh waiter, up to 5 times.

The latency is printed.

Proved, each mutation alone on both architectures:

| mutation | result |
|---|---|
| no IPI in `request_resched` | fails at the count |
| `request_resched` never called | fails at the count |
| the idle wait removed, with the waiter held 20 ms between `BLOCKED` and its switch | fails: no round's wake found the target idle |
| the record not written | fails: no round's wake found the target idle |

With the idle wait in place, the same 20 ms hold passes. Without the
IPI, other tests fail too (`process-user`'s 15 s bound on both
architectures; on x86-64 also `mutex`, `process-spawn` and
`hid-keyboard`), because liveness then rides on the tick; `smp-wake` is
the one that names the cause. Skipped in a non-debug build, which writes
no record. Single CPU: logged as not exercised.

### `smp-ticks`
Snapshot `percpu_get(c)->ticks` on every online CPU, sleep 40 ms,
require between 5 and 40 ticks of progress per CPU (nominal 10 at
250 Hz; TCG jitter allowed). Proves each AP's LAPIC timer is running.

### `smp-mutex`
Two threads per CPU, each 300 lock/unlock cycles with a busy delay inside
the critical section; an atomic `inside` counter detects overlap. Final
count == threads × 300, no violation, mutex unlocked. Proves mutual
exclusion under genuinely parallel contention (the Phase 3 `mutex` test
only had one CPU).

## Hang watchdog

`selftest_run_all` arms `sched_watchdog_arm(8 s)` and kicks before every
test. If a test makes no progress for 8 s, the boot CPU's tick prints:

```
[WATCHDOG] no progress for 8002 ms; scheduler state:
cpu 0: online current 'idle' queued 0 switches 6 bitmap 0x0 need_resched 0 preempt 0 irq_depth 1 ticks 2111
cpu 1: online current 'idle' queued 0 switches 0 bitmap 0x0 need_resched 0 preempt 0 irq_depth 1 ticks 2107
...
 tid name                 state    pri cpu     run_ms   switch waiting_on
   1 kmain                blocked   32   0        447        2 -
   3 reaper               blocked   24   0          0        2 g_reap_wq
```

Reading it: every CPU `current 'idle'` with empty queues means nothing
is runnable, so a BLOCKED thread with `waiting_on -` is a lost wakeup.
That exact dump identified SMP11 during bring-up: `kmain` blocked on
nothing after a tick preempted it between `waitqueue_prepare` and
`waitqueue_finish`.

## QEMU monitor technique

When the harness times out, the first question is whether a CPU is
spinning or everything is idle. Start QEMU with
`QEMU_EXTRA="-monitor tcp:127.0.0.1:4471,server,nowait"`, and on a stall
send `info registers -a` (several samples 300 ms apart), then `cpu N`
and `x/48gx $rsp` for a suspect CPU. `RFL=...206 ... HLT=1` on every CPU
is the idle signature; a CPU with `RFL=...002` (IF clear) and `HLT=0`
across samples is spinning with interrupts off. Symbolise addresses with
`llvm-symbolizer --obj=out/x86_64-debug/kernel/kernel.elf`. The stall
that led to SMP11 showed all CPUs halted with one transient sample in
`lapic_eoi` (the tick), i.e. a lost wakeup rather than a deadlock.

## Waits and time bounds

A test waits for the property, never for an interval (`threads_settle`,
the waits for `THREAD_BLOCKED` and an idle target in `smp-wake`). An upper bound on elapsed
time is kept, restated as an observable, or widened and labelled
`LOAD-SENSITIVE` and listed in `docs/testing/flakes.md`, which says which
and why; this suite has none listed, since its three were all restated.

## Measured results

| Run | Result |
|---|---|
| `make test` (debug, `-smp 4`) | PASS, 27/27 self-tests, ~3 s |
| `make BUILD=release test` (`-smp 4`) | PASS |
| `QEMU_SMP=1 make test` | PASS, 27/27 |
| 24 consecutive debug boots after the SMP11 fix | 24 × exit 33, no stall (before the fix roughly one in three stalled) |
| `make test-crash` (`-smp 4`) | PASS: panic stops the other CPUs first |
| `make host-test`, `make analyze`, `make reproducible` | PASS, clean, byte-identical |

Calibration under TCG on the development host: TSC ≈ 1.0 GHz, LAPIC
timer ≈ 62–67 MHz after divide-by-16; APs reuse the boot CPU's values.

## Gaps and planned tests

- No stress test aimed at the preempt-versus-block window (many
  threads blocking and waking under a fast tick for seconds); the
  repeated-boot loop is the current evidence.
- Shootdown correctness is inferred from acknowledgement counts, not
  from a stale-translation access; a test that maps, touches on another
  CPU, unmaps, and then expects a fault on that CPU needs a recoverable
  kernel fault path.
- No load balancing, so no test for it; no CPU hotplug.
- The tick-count bounds (5–40 ticks) are loose for TCG and would be
  tightened on hardware or with KVM/HVF (`QEMU_ACCEL`); the wake latency
  is no longer a bound at all (it is printed, and the IPI is counted).
- The AP-fails-to-start path is exercised only by review.
