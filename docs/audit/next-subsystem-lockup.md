# NEXT SUBSYSTEM — a hang that names its program counter

Date: 2026-09-14. Tree: `main` at 4d5acdb (after PR #134, the
wake-preempt unit). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §4 and §3.

**Built: PR #136 (2026-09-14).** The design below is as proposed; the
sections "As built" and "As run" record what the build changed and
measured. Differences from the plan, each found by building rather than
reading:

1. **An AArch64 leaf function had no frame record.** At `-O1` clang omits
   the frame in a leaf, so a walk from a PC inside one (a spinning loop
   is a leaf) skipped its caller and showed the thread trampoline where
   the spinner's caller should be. The AArch64 kernel is now built with
   `-mno-omit-leaf-frame-pointer` (`build/arch/aarch64.mk`); x86-64 keeps
   the leaf frame already. Every backtrace on AArch64 gains a frame it
   used to lose.
2. **A masked CPU cannot acknowledge a TLB shootdown**, whose waiter
   panics after one second (`arch_mmu_shootdown`). A test that masks a
   CPU stops it before joining any thread whose stack teardown needs
   the acknowledgement, and keeps every mask well under a second; the
   first run of the busy test found this by panicking.
3. **The API shapes**: `lockup_sample_all(self, timeout, &answered)`
   returns whether it got the slot; `lockup_answer(frame, nmi)`;
   `lockup_set_thresholds(soft, hard, expected)` -- an expected report's
   line says `expected`, which is how the self-tests' own reports pass
   the harness's forbidden marker while a real one fails the run;
   `lockup_get_stats`, `lockup_reporter`. The requester's own sample is
   labelled `self`. `lockup_watch_target` lives in `lockup_core.h` so
   the host test includes it as is.
4. **The no-latch bug-proofs time out rather than count two reports**: a
   report per tick, each with a 5 ms sample wait inside the tick and a
   trace on the console, floods the log and the run hits the harness
   timeout at 184 s. That is what the latch prevents, recorded as run.
5. **The busy test's "refused at once" bound is 1 ms**, half the winner's
   hold, not 100 µs: an interrupt landing on the loser's CPU in between
   costs tens of microseconds under TCG, and 63-75 µs was the refusal's
   own cost. The property is "did not wait for the slot", and the hold
   is what a waiter would take.
6. **The tick bench** measures the two stores by a million-iteration
   loop (5 ns per tick on x86-64, 14 on AArch64, under TCG) and the
   tick's entry-to-hook mean from `CONFIG_SELFTEST` accounting in
   `tick_isr` (8 µs and 16 µs), not a with-and-without comparison, which
   a store that costs nanoseconds against a handler that costs
   microseconds cannot resolve.
7. **The harness prints its symbol table** only when the run failed or a
   panic was expected: a passing run's log carries the lockup
   self-tests' own samples, forty addresses of noise. The table is
   `address  function  file:line`, repo-relative, without the `+offset`.
8. **Steps 1-3 landed as one commit** (the sample, the dump and the
   detectors share `lockup.c`); step 4 and step 5's guard as the next.
9. **The spin, found.** The `#ifndef` guard made `make EXTRA_CFLAGS=-DNET_WORKER_PRIO=31 test` the reproduction; it hung on the seventh boot on x86-64, and the tool built for it did the diagnosis in one dump: the watchdog's block carried the sample and eight more, 250 µs apart, of the CPU whose worker was running -- every one in the worker's wait condition (`mbufq_len`, `netif.c:705`), its dequeue (`mbufq_dequeue`, `netif.c:708`) or the wait's exit path, none in a packet or a work item. The queue's count said non-empty over a list that was empty, the case the report had called impossible by the lock. It is impossible by the lock; it is not impossible by a **second enqueue of an mbuf already on the queue**: `mbufq_enqueue` cleared `m->nextpkt` before taking the lock, which cuts the list behind a queued mbuf and orphans the rest, and the count stays. The second enqueue was the *reorder test's* loopback filter (`net-tcp-reorder`, `nettest.c`): it held a copied segment in a plain global and re-injected it with a read-then-clear that two CPUs -- the sending thread and a worker sending an ACK -- could both win. At priority 32 the spinning worker was invisible: an equal-priority thread still gets its slice, the suite passes, and one boot in five has burned a CPU since the reorder test landed. At 31 it starved `net-steer`'s injector, and the hang named it. A second reproduction on boot 3 of the next run, with the profile in place, showed the same eight-sample shape on CPU 2's worker.

**Subsystem: a per-CPU sample of the running program counter that the
scheduler dump, the self-test watchdog and two new lockup detectors
print; then the network worker's latent spin, to be reproduced with
it, diagnosed from it and fixed.** Nothing in this report is built; the
migration plan is the plan, and the "as run" and "as built" sections
are filled by the implementation pull request. The wake-preempt unit found a hang it
could not diagnose: with the network worker one priority above its
feeder, one x86-64 boot in five stopped in `net-steer` with the worker
`running` on CPU 3 for eight seconds and the injector thread pinned
there never scheduled. The watchdog's dump said everything about that
CPU except the one thing a diagnosis needs -- *where* the worker was --
and the record kept of it elides the `ticks` column, so it is not even
known whether CPU 3 was still taking interrupts. The kernel throws the
answer away every four milliseconds: the tick handler receives the
interrupted frame and discards it (`kernel/timer/timer.c:208-211`), the
four IPI handlers receive it and discard it (`kernel/interrupt/ipi.c`),
and the panic path already walks a frame into a stack trace
(`kernel/core/panic.c:40-54`) that no dump but the panic's uses. This
unit keeps the answer: two stores per tick (the PC and its time), an IPI (an NMI on x86-64)
that asks a CPU to record its own frame and stack, a dump that prints
them, a soft-lockup detector on every CPU and a hard-lockup detector on
the next online CPU, and a harness that turns the addresses into
`function+offset (file:line)` with the ELF it already builds. The spin
is then chased with the tool built for it: `NET_WORKER_PRIO` made
overridable from the command line, the boot repeated at 31 until the
dump names the loop, the fix made from the mechanism the program counter
shows, and a test that builds that mechanism deterministically.

**Inventory entries this unit is to close** (struck through in its
documents commit when built, not before). §4 "a latent spin in the
network worker that a priority above its feeder exposes ... a diagnosis
needs the running thread's PC, which the dump does not carry" -- the
tool in every outcome, the diagnosis and the fix if the reproduction in
step 5 succeeds (that step states what is recorded if it does not); §3
"the watchdog fires once, from CPU 0, no NMI path, no hard/soft-lockup
detection" (the NMI path on x86-64, both detectors; "fires once" stays
by design, see Risks). It would advance §2.8 "panic symbolisation is
still address-only" on the harness side only: the kernel still prints
addresses; the harness resolves them. It leaves
open, and names as the next step, an NMI-class interrupt on AArch64
(GICv3 pseudo-NMI), without which a CPU spinning with interrupts masked
on that architecture answers nothing and the dump can only say so.

**Why this, of the inventory's open items.** It is the only entry that
is a *hang seen on this tree*, a correctness gap in the constitution's
first sense, and it is the direct successor of the last unit: the
measurement that vetoed priority 31 also said "the spin, if it exists,
is masked by the tick at 40 and 32", and a masked spin is a hang waiting
for the next scheduling change. The other §3-4 correctness entries --
the tick-bound grace period, the never-tested straggler IPI and
`blk_unregister` window, `close()` without write-back errors, the 1 KiB
`read` -- are each real and each testable, but none of them has an
observed failure on record, and none is a prerequisite of the others.
This one, unfixed, would take the next hang's diagnosis back to reading
code, which is what the last unit did and what did not find it.

## Problem

The watchdog dump the last unit recorded, 8 s into `net-steer` on
x86-64 with the worker at priority 31
(`docs/audit/next-subsystem-wake-preempt.md`, "The worker's priority"):

```
[WATCHDOG] no progress for 8002 ms; scheduler state:
cpu 0: online current 'idle'    queued 0 ... need_resched 0 preempt 0 irq_depth 1
cpu 3: online current 'netrx/3' queued 1 ... bitmap 0x100000000 need_resched 0 preempt 0 irq_depth 0
 tid name       state    pri cpu  run_ms  switch waiting_on
   1 kmain      blocked   32   0    9473    4430 completion-wq
  11 netrx/3    running   31   3      93     345 -
 167 steer-inj  ready     32   3       0       0 -
```

Everything in it is consistent with two different failures, and it
cannot tell them apart:

1. **The worker is spinning with interrupts on.** `preempt 0` and
   `irq_depth 0` say it holds no spinlock and is not in a handler;
   `need_resched 0` is expected, because the injector at 32 does not
   outrank 31 (`sched_wake`, `kernel/scheduler/sched.c:318`), and the
   round-robin tick gives way only to an equal or higher priority
   (`kernel/scheduler/policy_rr.c:58-63`). A thread that outranks every
   other on its CPU leaves it only by blocking. So *some* loop in the
   worker's path never blocked for eight seconds.
2. **CPU 3 stopped taking interrupts.** A region that saved interrupts
   off and never restored them, or a stalled local timer, looks the same
   from CPU 0: `current` is whoever was last switched in, and every
   per-CPU field is whatever it was when the interrupts stopped. The
   `ticks` column would decide this -- it is printed
   (`sched.c:407-412`) -- but the record in the report replaced it with
   "...".

`run_ms 93` decides nothing either: `run_time_ns` is charged at
switch-out (`sched.c:231`), so a thread that has not switched for eight
seconds shows the run time it had before, not the eight seconds.

Reading did not find the loop, and the reading was not shallow. The
worker's loop is four lines (`kernel-services/network/netif.c:699-711`):
`wait_event` on "a packet is queued or work is pending", a bounded
dequeue, `run_work`. The wait cannot spin: `waitqueue_prepare`, test the
condition, block (`kernel/include/kernel/wait.h:57-66`). The dequeue
cannot disagree with the condition: `mbufq_len` and the links change
under the same lock (`kernel-services/network/mbuf.c:313-343`), so "len
says one, head says none" does not exist. `run_work` unlinks each item
under the work lock and clears its `queued` flag before running it
(`netif.c:656-670`), so it drains unless an item requeues itself
unconditionally -- which would spin at priority 40 too. There is no
yield-based wait anywhere in the kernel to starve a lower thread (zero
callers of `thread_yield` outside tests, checked 2026-09-14). And the
test's own hook does nothing but copy 58 bytes and count
(`nettest.c:1846-1872`). Every candidate that can be excluded by reading
has been; what remains is timing-dependent (one boot in five, one
architecture) and needs the program counter.

**What the kernel has and throws away.** Every interrupt handler in this
kernel receives `struct arch_trap_frame *frame`
(`kernel/include/kernel/interrupt.h:33`), and `arch_trap_frame_pc`
(`kernel/include/arch/trap.h:42`) reads the interrupted program counter
from it. The tick handler's third statement is `(void)frame`
(`timer.c:211`). The reschedule, call, TLB-flush and halt IPI handlers
each begin with `(void)frame` (`ipi.c:35-63`). `arch_backtrace(pcs, max,
from)` walks the interrupted context from a frame on both
architectures (`kernel/arch/x86_64/backtrace.c:47`,
`kernel/arch/aarch64/backtrace.c:34`; the build keeps frame pointers,
`build/toolchain.mk:36`), and `backtrace_print(frame)` in `panic.c:40`
prints it -- from the panicking CPU's own frame only. Nothing asks
another CPU for its frame; nothing records one between panics.

**What the watchdog is.** It exists only while the self-tests run:
`selftest_run_all` arms it at 8 s and kicks it before each test
(`kernel/core/selftest.c:600-606`); it is checked from CPU 0's tick
(`sched.c:379-386`), fires once (`g_watchdog_fired`, `sched.c:364-376`)
and prints `sched_dump`. Outside the self-tests, a CPU that stops
scheduling is noticed by nobody until the harness timeout, and in a
release build by nobody at all. The audit named this on 2026-09-05 ("the
watchdog fires once, from CPU 0, no NMI path, no hard/soft-lockup
detection", 6.2) and it is unchanged.

**What the harness does with an address.** Nothing. `run_boot_test.py`
takes `--image` and `--log` (`tests/boot/run_boot_test.py:363-364`); no
file under `tests/`, `scripts/`, `build/`, `.github/` or the Makefile
runs a symboliser (checked 2026-09-14; the only binutil named besides
the compiler and linker is `llvm-nm`, `build/toolchain.mk:18`). The
kernel ELF the harness would need is the Makefile's `$(KERNEL_ELF)`,
built before every boot test.

## Current implementation

| piece | where | what it does today |
| --- | --- | --- |
| tick handler | `kernel/timer/timer.c:208-221` | counts `pc->ticks`, runs expired timers, calls the tick hook with `now` only; discards the frame |
| tick hook | `kernel/include/kernel/timer.h:74`, `sched.c:379` | `sched_tick(now)`: the watchdog check on CPU 0, then the policy's tick |
| watchdog | `sched.c:341-376` | armed/kicked/disarmed by the self-test runner; CPU 0's tick fires it once; `sched_dump` |
| the dump | `sched.c:402-415`, `thread.c:253-266` | per CPU: online, current, queued, switches, restore-preempts, bitmap, need_resched, preempt, irq_depth, ticks; per thread: state, priority, CPU, run_ms (stale for the running thread), switches, wait queue |
| IPIs | `kernel/interrupt/ipi.c` | four kinds, vectors bound at init so sending takes no lock (`ipi.c:85-88`); handlers discard the frame |
| interrupt vectors | `kernel/interrupt/interrupt.c:70-95` | one handler per vector (`-EBUSY` for a second) |
| NMI, x86-64 | `kernel/arch/x86_64/trap.c:105-121` | the paranoid path: IST stack, per-CPU pointer recovered, dispatched with the frame; the `selftest-nmi` probe sends a software NMI to its own CPU (`trap.c:149-178`); the LAPIC has no NMI *delivery* (`lapic.c:42-44` defines fixed, INIT and SIPI only) |
| NMI, AArch64 | -- | no NMI-class interrupt anywhere under `kernel/arch/aarch64` (GICv3 pseudo-NMI is not configured) |
| backtrace | `kernel/core/panic.c:40-54` | `backtrace_print(frame)`: `arch_backtrace` from a frame, `#i %p` lines, "(outside kernel text)" tag |
| harness | `tests/boot/run_boot_test.py` | required and forbidden markers; no ELF, no symboliser |
| the worker | `netif.c:699-711, 739` | `NET_WORKER_PRIO` is `SCHED_PRIO_DEFAULT`, a plain `#define` with no override |

## Why it matters

- **A hang without a program counter is a hang diagnosed by reading**,
  and the last unit is the measurement of how well that works: a
  four-line loop, every candidate excluded, no diagnosis. The next hang
  will not be simpler.
- **The spin is real and masked, not absent.** At 32 the tick gives the
  starved thread its slice, so whatever loops in the worker gets
  interleaved rather than stuck; the count of delivered packets is fine
  and nothing fails. Any later change that puts the worker above its
  feeders -- a real-time policy (§2.3), a priority for the network
  under load -- reopens it as a hang.
- **Production has no detector.** The watchdog is a self-test fixture.
  A release kernel whose CPU 3 stops scheduling runs on until something
  external notices. The audit's "no hard/soft-lockup detection" has
  been open since 2026-09-05.
- **The dump is the one artefact a hang leaves**, and it is read by a
  person from a CI log after the machine is gone. Every field that
  costs two stores per tick to keep and would have decided the last
  diagnosis belongs in it.
- **Addresses are read by people.** `#3 0xffffffff8012a4c0` becomes
  `worker_main+0x40 (netif.c:707)` with a tool the toolchain already
  installs; the harness is where the ELF and the log meet.

## Design

### A CPU records its own frame; another CPU asks and prints

The rule is stated once: **a CPU's interrupted context is recorded by
that CPU, in its own handler, into its own per-CPU buffer, without
taking a lock or printing; whoever asked reads the buffers and prints.**
It follows from what an NMI may do (nothing that can block, nothing
that can take a lock the interrupted code holds -- the console's
included) and from what a frame-pointer walk needs (the walker's checks
use `this_cpu()->current` and the current thread's stack,
`backtrace.c:27-32`; walking another CPU's live stack from the outside
would race its every push).

```c
/* kernel/include/kernel/lockup.h */
#define LOCKUP_TRACE_MAX 16

struct cpu_sample {
    uint64_t  want;                /* request pending for this CPU (set by the asker) */
    uint64_t  seq;                 /* request this answer belongs to; 0 = never answered */
    uintptr_t pc;                  /* interrupted program counter */
    uintptr_t sp;
    uintptr_t trace[LOCKUP_TRACE_MAX];
    unsigned  depth;
    bool      nmi;                 /* answered from the NMI path (x86-64) */
    uint64_t  when_ns;
};

/* Ask every online CPU but the caller for its frame; wait at most
 * timeout_ns in total for the answers (the caller's own frame comes
 * from `self`, which may be NULL when the caller is a thread). Never
 * sleeps; safe from the tick. Returns false at once, sending nothing,
 * when another CPU's report is in progress; true with the mask of CPUs
 * that answered, and the reporter slot held until the paired
 * lockup_print_samples returns. */
bool lockup_sample_all(const struct arch_trap_frame *self, uint64_t timeout_ns, cpumask_t *answered);

/* Print the samples of `answered` and, for the others, the tick-sampled
 * last pc and its age; then release the reporter slot. */
void lockup_print_samples(cpumask_t answered);
```

**The tick sample.** `tick_isr` stores `arch_trap_frame_pc(frame)` and
`now` into `pc->last_tick_pc` / `pc->last_tick_ns` before anything
else (two stores; the frame is already in a register). This is the
free, always-present half: what a CPU was doing at its last tick, and
*when* that was. A CPU whose `last_tick_ns` is eight seconds old was not
taking ticks, and the dump says so instead of printing a stale field as
if it were current.

**The request.** There is one reporter at a time, and nobody waits to
become it. `lockup_sample_all` claims `g_reporter` (0 = free, else CPU
id + 1) with one compare-and-swap; a caller that finds it taken returns
`false` at once, having sent nothing and waited for nothing. It does
not spin for the slot because the holder may be waiting with interrupts
off (the tick is where the detectors run), and a second CPU spinning
on it with interrupts off would stop its own ticks and, on AArch64,
its own sample answers -- a diagnostic that manufactures the stall it
diagnoses. The reporter increments `g_sample_seq`, writes it into each
target's `sample.want`, sends every sample interrupt -- on x86-64 an
NMI (`arch_ipi_send_nmi(cpu)`, new, the LAPIC ICR with delivery mode
NMI, `4 << 8`, alongside the existing fixed/INIT/SIPI modes in
`lapic.c`); on AArch64 the new `IPI_SAMPLE` kind, an ordinary SGI
(`arch_ipi_send_nmi` returns `false` there and the generic code falls
back) -- and then waits **once, for all targets together**: acquire
loads of every target's `sample.seq` until each equals the request or
`timeout_ns` has passed in total. The bound is total, not per target:
5 ms whether 3 CPUs or 63 are asked. A thread caller waits with
interrupts on (the claim is an atomic, not a lock) and takes its own
ticks and interrupts meanwhile; the watchdog and the detectors call
from the tick with interrupts already off, which is why the bound is
small and single. The slot is held until `lockup_print_samples`
returns, so no later request overwrites a buffer under the printer. The
caller's own sample is taken directly from `self` when the caller is a
handler (the watchdog is in CPU 0's tick and has the frame), or from a
fresh `arch_backtrace(NULL)` when the caller is a thread. A detector
that finds the slot taken still prints its one-line report (and, for a
soft lockup, its own frame's trace, which needs no request) with
`sample in progress on cpu J` in place of the samples.

**The answer.** On both architectures the handler is the same function
`bool lockup_answer(frame)`: if this CPU's `sample.want` equals its
`sample.seq` there is no request pending *for this CPU* (an NMI or SGI
from elsewhere) and it returns `false`; otherwise it fills `pc`, `sp`,
`trace` from `arch_backtrace(..., frame)`, `nmi`, `when_ns`, stores
`seq` with release and returns `true`. A nested NMI inside the answer
sees the same pending request and writes the same values. On AArch64
the `IPI_SAMPLE` handler is `lockup_answer` behind the ordinary IPI
plumbing. On x86-64 `x86_trap_paranoid` calls `lockup_answer` for
`X86_TRAP_NMI` and then **dispatches exactly as today whenever a
handler is registered on the vector** (`interrupt.c:79-80` allows
one): a registered NMI handler sees every NMI, sample or not, so the
`selftest-nmi` probe and any NMI consumer added later are never
bypassed and nothing they own is swallowed. Only with *no* handler
registered does the answer decide the outcome: an NMI that answered a
request pending for this CPU returns; one that did not falls to
`arch_trap_unhandled` and panics, as today. The residual is stated
rather than hidden: on a CPU with no registered NMI handler, an
unrelated NMI landing inside the microseconds between a request and
its answer is taken as the sample and its own cause is not reported --
where today it is a panic without a cause either. This residual is
the architecture's, not the design's: an x86 NMI carries no vector and
no source, so no handler can tell a sample NMI from another NMI in the
same window by anything but the pending request, and every kernel that
samples by NMI lives with the same window (Linux's `nmi_cpu_backtrace`
answers and returns "handled" on exactly this test). What the design
controls it does control: the window is one NMI delivery long (`want`
is written immediately before the send, and the answer ends it), a
registered handler is never bypassed, and the outcome for an unowned
NMI inside the window -- a recorded frame and no panic -- is a stated
rule with a stated size. No source on this kernel's machines raises
one (no NMI watchdog, no SERR/PERR routing, no NMI IPI but this
unit's), and a source added later registers a handler and is then
never in the residual. The recorded frame is the interrupted context
in either case, which is the fact the sample exists to record.

**What answers and what does not.** On x86-64 a CPU answers whether its
interrupts are on or off, holding a spinlock or not, in a handler or
not: the NMI is the whole point. On AArch64 a CPU with interrupts
masked does not answer within the timeout, and the printout for it is
the tick sample with its age -- which for a spinner with interrupts off
is the moment it masked them, the last useful fact available. Both
outcomes are printed as what they are:

```
cpu 3: pc 0xffffffff80112a60 sp 0xffffc00000213f10 (nmi, 14 us ago)
  #0  0xffffffff80112a60
  #1  0xffffffff80112b9c
  ...
cpu 2: no answer in 5 ms; last tick 8003 ms ago at pc 0xffffffff80104ee0
```

### The dump prints it

`sched_dump` gains, per CPU, the tick sample and its age on the `cpu
%u:` line and, when called through the watchdog or a lockup report,
the answered samples with their traces; `run_ms` for the running thread
becomes `now - last_start_ns` plus the accumulated time, so it is live.
The `[WATCHDOG]` block becomes: the line it prints today, the per-CPU
lines with `ticks` and `last tick N ms ago pc 0x…`, the thread table,
then one sample block per CPU. Nothing is removed and no existing
harness pattern changes; the block gets longer.

### Two detectors, one per CPU and one for the next online CPU

**Soft lockup: a CPU that has not switched while something waits.** In
`sched_tick`, on every CPU for itself: if `rq->current != rq->idle`
and `rq->nr_running > 0` and `rq->switches` is unchanged since the last
tick, the CPU's `stall_ns` grows by a tick; otherwise it resets. When it
reaches `g_soft_ns` (10 s) the CPU prints once per episode
`soft lockup: cpu N running 'name' for M ms with K runnable`, its own
`backtrace_print(frame)` (it is in its tick; the frame is its own), and
`lockup_sample_all` for the others. Under fixed priorities a runnable
thread that never runs for ten seconds while another runs is either a
strictly lower priority behind a spinner (the hang above) or a
scheduler bug (an equal priority gets a slice every 10 ms,
`policy_rr.c:52-63`); no thread in this tree does ten seconds of work
at a priority above another's, so the report is a bug report, not a
policy report. The episode ends when `switches` changes; a second
report needs a second episode.

**Hard lockup: a CPU that stopped ticking.** In `sched_tick`, on every
one of its ticks, each CPU `k` checks its **watch target**: the online
CPU with the next-higher id, wrapping -- `lockup_watch_target(mask, k)`
clears the bits at or below `k` in `cpu_online_mask()` and takes the
lowest set bit, or the mask's lowest bit if none is left. With two or
more CPUs online every online CPU has exactly one watcher whatever
holes the mask has (`{0,2,3}`: 0 watches 2, 2 watches 3, 3 watches 0);
a lone CPU's target is itself, and the check is skipped when the
target is the checker. The check: if the target's `ticks` differs
from the value seen at the last check, record it and reset `stall_ns`;
else `stall_ns += TICK_NS`; a target that changed (the mask changed)
resets. When `stall_ns` reaches `g_hard_ns` (10 s), `k` prints once per
episode `hard lockup: cpu J no tick for M ms; last tick at pc 0x…` and
runs `lockup_sample_all` -- on x86-64 the NMI reaches a CPU with
interrupts off and the report carries its live frame; on AArch64 it
carries the tick sample, which for an interrupts-off stall is the PC at
the last tick before the mask. The cadence is the tick, so a threshold
lowered by a test fires at the threshold plus one tick, not at the next
whole second; the cost is one load and one compare per tick. The kernel
ticks every online CPU at `CONFIG_HZ` (`smp-ticks` already asserts
it), so "no tick" is a stall, not idleness.

**Thresholds.** `lockup_set_thresholds(soft_ns, hard_ns)` is a test
hook (debug builds), not a sysctl: nothing but a test wants 200 ms.
Both are generous by design; see Risks for the load-sensitivity
argument.

**The harness** adds `soft lockup:` and `hard lockup:` to the forbidden
markers of every boot test, so a lockup fails the run with the CPU's
trace in the log instead of a bare timeout.

### The harness symbolises

`run_boot_test.py` gains `--kernel <elf>` (the Makefile passes
`$(KERNEL_ELF)`). After the run, for every line of the log matching a
frame (`#N 0x…`), a sample (`pc 0x…`) or a panic address, it runs
`$(LLVM_PREFIX)llvm-symbolizer --obj=<elf> --inlines=0` once over the
collected addresses and prints, in its own report below the marker
verdict, `0xffffffff80112a60  worker_main+0x40  netif.c:707`. The log
file is not rewritten (it is the run's evidence); the symbolised table
is the harness's addition. Missing symboliser or missing `--kernel`
means no table, never a failure. The kernel prints addresses as before:
a kernel-side symbol table is §2.8's item and is not proposed here.

### The reproduction, the diagnosis, the fix

`NET_WORKER_PRIO` becomes `#ifndef NET_WORKER_PRIO` / `#define ... SCHED_PRIO_DEFAULT` / `#endif`, so that

```
make EXTRA_CFLAGS=-DNET_WORKER_PRIO=31 test
```

builds the vetoed configuration without editing the tree
(`build/rules.mk:14` already appends `$(EXTRA_CFLAGS)` to every compile).
The boot is repeated on x86-64, up to 25 times, until the `[WATCHDOG]`
block with the sample appears (at the observed rate of one in five, 25
boots see it with probability above 0.99); the same on AArch64 for a
stated bound (five boots did not show it there). The `[WATCHDOG]` block
then carries CPU 3's program counter and trace, symbolised by the
harness, and the mechanism is read from it:

| the PC lands in | the mechanism it names | the fix's shape |
| --- | --- | --- |
| `worker_main`'s dequeue loop / `mbufq_dequeue` | the condition true with nothing to take -- impossible by the lock argument above unless a queue is not the one the condition looked at (`steer` chose one queue, the wake went to another) | the enqueue/wake pairing |
| `run_work` / a work function | an item that requeues itself, or a work function that does not return | the item's requeue condition |
| `wait_event`'s prepare/finish path | a wake that leaves the entry queued, so `prepare` never blocks | the wait queue |
| `steer_hook` / `m_copydata` / `m_freem` | the test's own hook, or a chain walk on an mbuf that points to itself | the mbuf |
| a spin with `irq_depth 0`, `preempt 0` **and** `last tick 8000 ms ago` | interrupts left off on CPU 3: an `arch_irq_save` region on the worker's path that returns without restoring (the preempt-wake unit added the restore-time preemption point on exactly this path) | the region |

The fix is made from the mechanism, and the test for it is built from
the mechanism -- the adversary constructed, not waited for
(`docs/audit/next-subsystem-suite-waits.md`'s rule) -- and bug-proofed
by reverting the fix. If 25 boots on each architecture do not reproduce
it, this unit lands without the fix, the inventory row stays with the
new evidence requirement replaced by the new tool, and the as-built
section says exactly that. The worker's priority stays at
`SCHED_PRIO_DEFAULT` either way: the last unit's decision was made on
throughput, which this unit does not touch.

### The §70 gate

**Correctness.** The sample answers exactly the question the last dump
could not: for each CPU, a program counter and stack recorded by that
CPU in its own handler, with the time it was recorded; for a CPU that
cannot answer, the last tick's PC and how old it is. The soft detector
fires on "no switch while something waits"; the hard detector on "no
tick"; each prints once per episode. Every claim is tested with a
spinner whose location is known.

**Concurrency.** The answer path writes only its own CPU's buffer and
takes no lock; the claim is `seq`, written last with release. The
request path claims the single reporter slot with one atomic and never
spins for it, sends, and waits once under a total bound; it is called
from the tick (interrupts off, `irq_depth 1`) and from threads. The
detectors read `rq->switches` and the watch target's `ticks` without locks
(monotonic counters; a torn read is a late report, not a false one).
The x86-64 NMI answer runs on the IST stack with the interrupted frame
pointing into the thread's stack, which is what `arch_backtrace`'s
`in_known_stack` accepts (`backtrace.c:27-32`); the walk reads a stack
its own CPU is not currently pushing to.

**Ownership / Lifetime.** Per-CPU buffers live in `struct percpu` for
the machine's lifetime; nothing is allocated or freed.

**Failure.** A CPU that does not answer is reported as such with its
tick age; the requester never waits past its bound. A second reporter
gets `false` and reports without samples. A nested NMI in the answer
writes the same values. A panic during a sample (a corrupt
frame pointer stops `arch_backtrace`, `backtrace.c:35-46`; it does not
fault) is not expected; if the walk did fault inside an NMI the panic
path's recursive-panic guard (`panic.c:63-75`) reports it.

**Security.** Kernel-internal; no user interface, no syscall, no
sysctl. The NMI delivery is an arch call used by two kernel callers.
The samples print kernel addresses to the kernel log, which the panic
path already does.

**Performance.** Two stores per tick per CPU (the tick sample), one
load and compare per tick (the watch target's ticks), one compare per tick
(the stall counter): measured by `net-bench` staying within its
run-to-run spread, and the tick's own cost by a self-test that prints
it (Benchmarks). The request costs one interrupt per CPU and happens
only on a report or in a test.

**Scalability.** Everything is per CPU; the request is O(CPUs) and
happens when the machine is already stuck. `LOCKUP_TRACE_MAX 16`
frames × 64 CPUs is 8 KiB of static per-CPU data.

**Portability.** The arch interface gains one function,
`bool arch_ipi_send_nmi(unsigned cpu)` in `arch/irqc.h`, true on x86-64
and false on AArch64, and one call from the x86-64 paranoid path into
generic code. Everything else is generic, including the fallback.

**Testing.** A spinner in a known function on a known CPU; the sample's
PC must lie in that function. Deterministic on both architectures with
the outcome stated per architecture (answered vs. tick-sampled), the
way the vGIC tests state theirs per host.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/lockup.h`, `kernel/core/lockup.c` | new: `struct cpu_sample`, `lockup_sample_all`, `lockup_answer`, `lockup_print_samples`, `lockup_watch_target` (pure, host-testable), the two detectors' per-CPU state and checks (`lockup_tick(frame, now)` called from `sched_tick`), `lockup_set_thresholds` (debug) |
| `kernel/include/kernel/percpu.h` | `last_tick_pc`, `last_tick_ns`, `struct cpu_sample sample`, the detectors' `stall_ns`, `last_switches`, `watch_target`, `watch_ticks` |
| `tests/host/test_lockup.c` | `lockup_watch_target` over masks with holes (the machine cannot make one: no CPU hotplug, inventory §3) |
| `kernel/timer/timer.c`, `kernel/include/kernel/timer.h` | the tick sample (two stores, first thing in `tick_isr`); the tick hook signature gains the frame: `timer_tick_hook_fn(uint64_t now_ns, struct arch_trap_frame *frame)` |
| `kernel/scheduler/sched.c` | `sched_tick(now, frame)`: `lockup_tick` after the watchdog check; `sched_dump` prints the tick sample and age, live `run_ms`, and the samples when asked; `watchdog_check` calls `lockup_sample_all(frame, 5 ms, &answered)` before the dump |
| `kernel/scheduler/thread.c` | `thread_dump_all` takes `now` for the live `run_ms` |
| `kernel/interrupt/ipi.c`, `kernel/include/kernel/ipi.h` | `IPI_SAMPLE`: handler `lockup_answer(frame)`; `ipi_send_sample(cpu)` tries `arch_ipi_send_nmi` first |
| `kernel/include/arch/irqc.h` | `bool arch_ipi_send_nmi(unsigned cpu)` |
| `kernel/arch/x86_64/lapic.c`, `irqc.c` | `ICR_DELIVERY_NMI (4u << 8)`; `arch_ipi_send_nmi` through `icr_send`, true |
| `kernel/arch/x86_64/trap.c` | `x86_trap_paranoid`: for `X86_TRAP_NMI`, `lockup_answer(frame)` first; return if it answered a request, else dispatch as today |
| `kernel/arch/aarch64/irqc.c` | `arch_ipi_send_nmi` returns false |
| `kernel/core/lockuptest.c`, `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | the tests below |
| `tests/boot/run_boot_test.py`, `Makefile` | `--kernel $(KERNEL_ELF)`; the symbolised table; `soft lockup:` / `hard lockup:` forbidden |
| `kernel-services/network/netif.c` | the `#ifndef` guard on `NET_WORKER_PRIO`; the per-CPU counters registered as a `sched_dump` hook (as built) |
| `kernel-services/network/mbuf.c`, `kernel/include/kernel/mbuf.h` | as built, where the PC led: `M_QUEUED`, a second enqueue refused, counted (`double_enqueues`) and said once; `nextpkt` cleared under the lock |
| `kernel-services/network/nettest.c` | as built: the reorder filter's state under a lock; the mechanism test `net-mbufq-double` |
| `kernel/scheduler/sched.c`, `kernel/include/kernel/sched.h` | as built: `sched_dump_register`, subsystem dump hooks printed after the thread table; the watchdog profiles every busy CPU (eight samples) |
| `docs/kernel/diagnostics/{design,api,invariants,testing}.md` | the sample, the detectors, the dump format, the harness table |
| `docs/kernel/scheduler/{design,api,testing}.md`, `docs/kernel/timer/api.md`, `docs/kernel/smp/{api,design}.md`, `docs/kernel/interrupt/api.md` | the hook signature, the dump, `IPI_SAMPLE`, the NMI delivery |
| `docs/kernel-services/network/design.md` | the spin's diagnosis and fix, under "The worker's priority" |
| `docs/testing/flakes.md` | what a load-induced lockup report looks like and how to read one (Risks) |
| `README.md`, `docs/README.md`, `docs/audit/2026-09-deferred-work-inventory.md` | the Status entry; the index; the strike-throughs |

**No change** to `struct thread`, to any wake or unlock path, to the
scheduler's policy, to the network stack outside the guard and the fix,
or to any user-visible interface.

## New APIs

Kernel-internal only.

| API | where | contract |
| --- | --- | --- |
| `bool lockup_sample_all(const struct arch_trap_frame *self, uint64_t timeout_ns, cpumask_t *answered)` | `kernel/lockup.h` | any context, never sleeps, never waits for another reporter (`false` at once if one is in progress); sends the sample interrupt to every other online CPU and waits once, under one total bound, for who answers; holds the reporter slot until `lockup_print_samples` |
| `bool lockup_answer(struct arch_trap_frame *frame)` (as built: `lockup_answer(frame, nmi)`, the second argument recorded into the sample) | `kernel/lockup.h` | handler side; records this CPU's frame if a request is pending for it and says whether it did; no locks, no printing |
| `unsigned lockup_watch_target(cpumask_t online, unsigned k)` | `kernel/lockup.h` | the online CPU with the next-higher id, wrapping; `k` itself when alone |
| `void lockup_print_samples(cpumask_t answered)` | `kernel/lockup.h` | prints each CPU's sample or its tick-sample-and-age; releases the reporter slot |
| `void lockup_tick(struct arch_trap_frame *frame, uint64_t now_ns)` | `kernel/lockup.h` | the two detectors' per-tick step; called by `sched_tick` |
| `void lockup_set_thresholds(uint64_t soft_ns, uint64_t hard_ns)` (as built: a third argument, `expected`, marks the next report as a test's so the harness's forbidden marker does not match it; `lockup_get_stats`, `lockup_reporter`, `lockup_sample_cpu` and `lockup_profile` were added) | `kernel/lockup.h`, debug builds | test hook |
| `bool arch_ipi_send_nmi(unsigned cpu)` | `arch/irqc.h` | deliver an NMI-class interrupt to `cpu` if the architecture has one; false means "use the ordinary IPI" |
| `IPI_SAMPLE` | `kernel/ipi.h` | the ordinary-priority sample interrupt |
| `timer_tick_hook_fn(uint64_t now_ns, struct arch_trap_frame *frame)` | `kernel/timer.h` | the hook receives the frame (one hook exists: `sched_tick`) |
| `run_boot_test.py --kernel <elf>` | harness | symbolise frame, sample and panic addresses in the harness report |

## Migration plan

1. **The sample.** `lockup.[ch]`, the per-CPU fields, the tick sample,
   `IPI_SAMPLE`, `arch_ipi_send_nmi` on both architectures, the
   paranoid-path call. Tests `lockup-sample`, `lockup-sample-irqoff`
   and `lockup-sample-busy`; the host test of `lockup_watch_target`.
   Boots green on both architectures.
2. **The dump.** `sched_dump` with the tick sample, the live `run_ms`,
   the samples on a watchdog fire. The self-test watchdog's block is
   checked by hand once by arming it under a deliberate spin (the
   `lockup-sample` spinner with the watchdog's timeout lowered) and
   quoted in the report as run.
3. **The detectors.** `lockup_tick`, thresholds, the harness's forbidden
   markers. Tests `lockup-soft`, `lockup-hard`, `lockup-quiet`. The
   whole suite on both architectures, five boots each, with no lockup
   line: the detectors' false-positive check on the real workload.
4. **The harness.** `--kernel`, the symbolised table; the panic boot
   test (`make test-panic`, `--expect-panic`) shows a symbolised trace
   in its report as the first use.
5. **The reproduction.** The guard; up to 25 boots per architecture at
   31; the block captured and quoted; the diagnosis; the fix; the
   mechanism test, bug-proofed by reverting the fix; the suite green at
   31 for five boots on each architecture (the veto on throughput
   stands; the hang must be gone).
6. **Documents.** As listed; the inventory rows struck through with
   this PR's number; the README Status entry; the index.

Each step is a commit that boots green on both architectures; the
report's "as built" banner records where each landed.

## Tests

In `kernel/core/lockuptest.c`, registered in `selftest.c` after
`smp-ipi-storm`. Each spinner is `static __noinline void spin_here(volatile bool *stop)` -- a
loop on `*stop` with `arch_cpu_relax()` -- and the check for "the PC is
in the spinner" is `pc >= (uintptr_t)spin_here && pc < (uintptr_t)spin_here + 128`:
the function is a dozen instructions on either architecture at `-O1`,
and a compile that grew it past 128 bytes would fail this test loudly
rather than let a wrong PC pass. Each test needs two CPUs and skips
with a reason at one (the affinity API exists: `thread_create_on`).

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `lockup-sample` | a thread pinned to CPU `k` spins with interrupts on; the test on another CPU calls `lockup_sample_all(NULL, 5 ms, &answered)`; it returns `true` and CPU `k` is in the mask, its `pc` is in `spin_here`, its `depth ≥ 2` and `trace[1]` is in the spinner thread's entry function; `when_ns` is within the call | answer with the *requester's* PC (the bug the design forbids: printing from the asker's view) → `pc` outside `spin_here`; skip the release on `seq` → not in the mask |
| `lockup-sample-irqoff` | the spinner masks interrupts (`arch_irq_save`) for 50 ms and spins; the sample is taken at 20 ms. **x86-64:** CPU `k` answers, `nmi == true`, `pc` in `spin_here`. **AArch64:** CPU `k` is not in the mask, and `lockup_print_samples` reports its tick sample with an age ≥ 20 ms (the last tick before the mask). Then the spinner restores and the next sample answers on both | x86-64: send the ordinary IPI instead of the NMI → no answer; AArch64: report the tick sample as live → the age check fails |
| `lockup-soft` | thresholds lowered to 200 ms soft; a priority-16 thread pinned to CPU `k` spins with interrupts on for 600 ms while a default-priority thread is created on CPU `k`; the detector's line names CPU `k`, the spinner's name and `1 runnable`, its trace's `#0` is in `spin_here`; exactly one report for the episode (the log line is counted through a test hook on the report path, not by grepping) | disable the `nr_running > 0` term → the `lockup-quiet` spinner (below) reports; disable the episode latch → two reports |
| `lockup-hard` | thresholds lowered to 200 ms hard; a thread on CPU `k` masks interrupts and spins 600 ms; `k`'s watcher (the online CPU below it, wrapping) reports `hard lockup: cpu k`, once, within 200 ms plus one tick of the mask (the check runs every tick), with (x86-64) a live NMI sample in `spin_here` or (AArch64) the tick sample and an age ≥ 200 ms; after the spinner restores, the target's `ticks` advance and a second 600 ms of normal running reports nothing | compare the wrong CPU's ticks → no report; check once a second instead of every tick → the 600 ms episode ends unreported (the finding this row was rewritten for); disable the episode latch → the 600 ms produce two reports |
| `lockup-sample-busy` | two threads on two CPUs call `lockup_sample_all` at the same moment (released by one flag); exactly one gets `true` and a non-empty mask; the other gets `false` within 100 µs of the flag, having sent nothing (`sample.want` on every CPU carries only the winner's sequence); no CPU's `ticks` stall during the round | let the loser spin for the slot → its elapsed equals the winner's wait; wait per target instead of once → on AArch64, with two CPUs spinning interrupts-masked (no answer), the winner's elapsed doubles to 10 ms where the total bound keeps it under 5 ms plus a margin (on x86-64 both answer by NMI, so this half of the row is AArch64's) |
| `test_lockup` (host) | `lockup_watch_target` over `{0,2,3}`, `{1}`, `{0,1,2,3}`, `{0,63}`: every online CPU has exactly one watcher, none watches an offline CPU, a lone CPU watches itself | `(k+1) mod n` in place of the mask walk → `{0,2,3}` leaves 2 unwatched |
| `lockup-quiet` | with the thresholds at 200 ms, a spinner alone on CPU `k` (nothing else runnable there) for 600 ms and an idle CPU for 600 ms produce no report of either kind; then the thresholds are restored to 10 s | the soft term inverted → a report |
| `lockup-tick-bench` | prints, asserts nothing: the tick handler's cost with and without the sample stores, measured as the median of 1 000 ticks' `clock_now_ns` deltas between entry and the hook call (a static counter the tick hook reads); the shape of `irqrestore-bench` | -- |
| the mechanism test (as built: `net-mbufq-double`, `kernel-services/network/nettest.c`) | three mbufs queued, the middle one enqueued again: refused, counted in `double_enqueues`, the count still three, drained in order to zero, the mbuf reusable once off the queue | remove the refusal in `mbufq_enqueue` -> fails at the refusal check itself (as run), ahead of the count reading four over a list of two |

**Vacuity, named in advance.** A sample test that passes because the
spinner never ran on CPU `k` (the affinity ignored) would show `pc` in
the *idle* loop; the range check catches it. A soft-lockup test whose
lower-priority thread was never created shows `0 runnable` and no
report; the test asserts the report *and* the `1 runnable` text. The
`lockup-quiet` test guards the detectors against a suite that passes
because they never fire: it is the positive tests' control.

The suite is then run five times per architecture with the thresholds
at their defaults: no `soft lockup:` or `hard lockup:` line in any run
(the harness's forbidden markers enforce this from step 3 on, so every
later boot is this check).

### As run

**The suite** (2026-09-14): 257 self-tests pass on both architectures,
every boot, before and after the harness change (258 once
`net-mbufq-double` landed, step 5); `make test-crash`
prints the symbol table below its verdict (the panic's `#0` resolves to
`kernel_main`, `kernel/core/main.c:216`, the crash test's write).

**The tests' own figures**: `lockup-sample` answers in 60-110 µs on
x86-64 (NMI) and about 100 µs on AArch64 (SGI); `lockup-sample-irqoff`
sees the tick sample 25-29 ms old at the 20 ms mark; `lockup-sample-busy`
refuses the loser in 50-75 µs and, with two masked targets, completes in
31 µs on x86-64 (both answered by NMI) and 5 022 µs on AArch64 (neither
answered; one bound, not two); `lockup-soft` reports at 200 ms;
`lockup-hard` reports at 200 ms of stall with the tick sample 199-205 ms
old.

**The bug-proofs** (each injected into `lockup.c`, the suite booted):

| injection | failed as |
| --- | --- |
| the answer from the handler's own walk, not the interrupted frame | `lockup-sample`: `pc` outside `spin_here` (and `-irqoff`, `-hard` with it) |
| no release store on `seq` | `lockup-sample`: `k` not in the mask |
| the ordinary IPI instead of the NMI (x86-64) | `lockup-sample-irqoff`: no answer through the mask |
| a fresh tick faked on the target (AArch64) | `lockup-sample-irqoff`: the age check |
| the loser spins for the slot | `lockup-sample-busy`: both get the slot in turn |
| a wait per target (AArch64) | `lockup-sample-busy`: two masked targets take 10 ms |
| the `nr_running > 0` term dropped | `lockup-quiet`: the lone spinner reports |
| the soft latch dropped | a report per tick; the run times out at 184 s |
| the watcher's own ticks compared | `lockup-hard`: never fires |
| the check every 250th tick | `lockup-hard`: never fires within the budget |
| the hard latch dropped | a report per tick; the run times out |
| `(k + 1) mod n` for the watcher (host) | `test_lockup`: CPU 2 unwatched in `{0,2,3}` |

**The spin, found.** The `#ifndef` guard made `make EXTRA_CFLAGS=-DNET_WORKER_PRIO=31 test` the reproduction; it hung on the seventh boot on x86-64, and the tool built for it did the diagnosis in one dump: the watchdog's block carried the sample and eight more, 250 µs apart, of the CPU whose worker was running -- every one in the worker's wait condition (`mbufq_len`, `netif.c:705`), its dequeue (`mbufq_dequeue`, `netif.c:708`) or the wait's exit path, none in a packet or a work item. The queue's count said non-empty over a list that was empty, the case the report had called impossible by the lock. It is impossible by the lock; it is not impossible by a **second enqueue of an mbuf already on the queue**: `mbufq_enqueue` cleared `m->nextpkt` before taking the lock, which cuts the list behind a queued mbuf and orphans the rest, and the count stays. The second enqueue was the *reorder test's* loopback filter (`net-tcp-reorder`, `nettest.c`): it held a copied segment in a plain global and re-injected it with a read-then-clear that two CPUs -- the sending thread and a worker sending an ACK -- could both win. At priority 32 the spinning worker was invisible: an equal-priority thread still gets its slice, the suite passes, and one boot in five has burned a CPU since the reorder test landed. At 31 it starved `net-steer`'s injector, and the hang named it. A second reproduction on boot 3 of the next run, with the profile in place, showed the same eight-sample shape on CPU 2's worker.

**The fix**, from the mechanism, in two places. The stack: an mbuf on a queue carries `M_QUEUED` (set and cleared under the queue's lock); a second enqueue is refused with the mbuf untouched, counted (`double_enqueues`) and said once (`WARN`), and `nextpkt` is cleared under the lock, not before it. The test: the filter's state is under a lock and the held copy leaves the lock before it is injected. **The mechanism test** is `net-mbufq-double`: three mbufs queued, the middle one enqueued again -- refused, counted, the count still three, drained in order to zero, the mbuf reusable once off the queue; bug-proofed by removing the refusal: the test fails at the refusal check itself, before the count could read four over a list of two.

**A second hang at 31, found by the five-boot check.** With the spin fixed, the fifth boot at 31 on x86-64 hung with every CPU idle: the keepalive test's server thread blocked in `accept` with one switch and no run time, the test thread joined on it. Not a spin -- the dump's samples all show the idle loop -- but the tool still named it: the thread table said what waited on what. `ksock_connect` returns when the *client* side is established, before the worker has transmitted the handshake's third segment; `net-tcp-keepalive` then installed its black-hole filter, which in that boot swallowed exactly that segment. The server side stayed in SYN_RCVD, its accept never woke, and the join at the end waited forever. A race of the test's, older than this unit, with a window the priority widens. Fixed by waiting for the property (the suite-waits rule): the passive side's `conns_passive` count, before the black hole goes up and before the FIN_WAIT_2 half's close.

**As run** (2026-09-14): with both fixes the suite passes on both architectures (258 self-tests). At 31, ten boots (five per architecture) with no hang: nine passed outright, one on AArch64 failed `net-harness`'s TCP exchange. The throughput veto on 31 stands and `net-harness`'s TCP exchange can still fail there (the collapse the last unit measured, not a hang). The priority stays `SCHED_PRIO_DEFAULT`.

## Benchmarks

1. **`lockup-tick-bench`**: the tick's entry-to-hook cost, median of
   1 000 ticks, before and after the two stores. Expected: within noise
   of each other (a frame field load and two per-CPU stores against a
   `clock_now_ns` and a timer-queue lock). Both figures are printed on
   both architectures.
2. **`net-bench`**, unchanged, five boots per architecture before and
   after: every figure within the run-to-run spread recorded on
   2026-09-14 in `docs/kernel-services/network/design.md` ("The worker's
   priority"). This is the check that the per-tick work moved nothing.
3. **The sample's round trip**: `lockup-sample` prints the time from
   request to last answer on the 4-CPU machine. Expected tens of
   microseconds under TCG; the 5 ms bound in the watchdog is then shown
   to be two orders above the real cost, not a guess.

## Risks

- **A false hard-lockup report on a loaded host.** QEMU runs each vCPU
  on a host thread; a starved host thread takes no guest ticks until it
  runs again (the `cpu1: up` flake of PR #112 was this). Ten seconds is
  chosen because CI's worst vCPU starvation on record is well under it,
  and the report line carries the watched CPU's own tick count and the
  stall's length so that a report in CI is readable as "the host starved
  vCPU 2 for 11 s" if that is what happened. `docs/testing/flakes.md`
  gets an entry saying exactly that, so a first CI sighting is filed as
  load-sensitive and investigated as this unit's false positive first.
  The soft detector cannot false-positive this way: it counts the
  victim's own ticks.
- **A spinner on AArch64 with interrupts off is not sampled live.** The
  dump says "no answer" and gives the tick sample's age; that is the
  truth of the architecture as configured, and the next step is named:
  GICv3 pseudo-NMI (a priority-mask discipline through every
  `arch_irq_save`/`restore` and the GIC's `PMR`), a unit of its own.
- **The NMI path is shared.** A registered NMI handler is dispatched
  for every NMI, sample or not; the answer only decides the
  unregistered case, and its residual (an unrelated NMI with no handler
  inside the request window, on machines that raise none) is stated in
  the design. The `selftest-nmi` probe (which registers on the vector
  and sends itself `int $2`) is the regression test that the dispatch
  still happens, run with a request pending for its CPU as well as
  without. An NMI arriving while the CPU is inside `kprintf` is exactly
  why the answer path prints nothing.
- **Two reports at once.** Every CPU may detect; one reports with
  samples and the others report without, never waiting for it
  (`lockup-sample-busy`). The cost is that a second simultaneous stall
  gets its one-line report and its own trace but no cross-CPU samples;
  the alternative, a queue of reporters spinning with interrupts off,
  is the cascade the design refuses.
- **The watchdog still fires once.** By design: after the first block
  the machine is hung and the harness times out; a second block adds
  nothing. The detectors are per episode for the same reason.
- **The reproduction may not reproduce.** One in five is an
  observation from five boots; the true rate may be lower. The plan
  bounds the attempt (25 boots per architecture) and states the outcome
  either way; the tool is the deliverable that does not depend on it.
- **Frame-pointer walks are as good as frame pointers.** A leaf function
  compiled without a frame, or assembly, gives a short trace; the PC is
  still right, and the PC is what the diagnosis needs. The panic path
  has lived with this since Phase 1.
- **The tick hook signature changes.** One hook exists; the change is
  mechanical and the compiler finds any other.

## Alternatives considered

- **Print from the target CPU.** The obvious shape (each CPU `kprintf`s
  its own trace when asked) deadlocks when the target was interrupted
  inside the console lock, and an NMI can interrupt anything. Record
  locally, print from the asker.
- **Walk the other CPU's stack from the asker.** Its stack is live; its
  frame pointer is in a register the asker cannot read; and the walker's
  bounds checks are written for the local thread. Only the CPU itself
  can walk its own interrupted context safely.
- **A kernel-side symbol table.** §2.8's item: a `.ksymtab`-like table
  of every text symbol, a lookup in the panic path, a size cost in the
  image. The harness has the ELF and the toolchain has the symboliser;
  the kernel keeps printing addresses. Not proposed here; not
  precluded.
- **A GDB stub or a kernel debugger.** §2.8, "eventually"; far more than
  a hang needs, and useless in CI.
- **Pseudo-NMI on AArch64 now.** Needed for a live sample of an
  interrupts-off spinner there; touches every interrupt save/restore
  path and the GIC priority scheme. Named as the follow-up; the fallback
  (tick sample and age) is honest meanwhile.
- **A sampling profiler.** The same mechanism (a periodic PC sample per
  CPU) is the seed of one; not proposed. The per-tick store is one
  sample, not a ring.
- **Fix the spin by reading harder.** The last unit read the four-line
  loop, this report read the queue, the wait, the work list, the hook
  and the yield sites, and the candidates that remain are
  timing-dependent. The program counter is cheaper than the next hour
  of reading and outlives this hang.
- **Leave the worker at 32 and forget the spin.** It is masked, not
  gone; the first policy that puts the worker above a feeder reopens it
  as a hang, and the dump that meets it would still say nothing.
