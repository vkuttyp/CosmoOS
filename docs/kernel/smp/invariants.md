# SMP: Invariants

Each invariant names how it is checked: an assertion or panic in the
code, a self-test, or review (no mechanism yet).

## Bring-up

**SMP1. Application processors are started one at a time.** `smp_init`
starts an AP, waits for it to come online (or time out), then starts the
next. The trampoline data block at `0x8000` is shared, so two APs in
flight would read each other's stack and CPU index. Check: review of
`kernel/core/smp.c` (single loop, no concurrency).

**SMP2. The per-CPU pointer is installed after the GDT load on every
CPU.** `gdt_init_cpu` reloads the segment registers, which resets the GS
base; `x86_ap_entry` calls `arch_percpu_install` immediately after it and
before anything that takes a spinlock or calls `arch_cpu_id`. Same rule
as S16 for the boot CPU. Check: review; a violation faults at address 0
on the first per-CPU access.

**SMP3. The kernel root page table lies below 4 GiB.**
`arch_mmu_context_init` allocates it with `PMM_FLAGS_ZONE_DMA32` because
the trampoline loads it into CR3 from 32-bit protected mode, where only
32 bits are addressable. Check: `KASSERT(cr3 < (1ULL << 32))` in
`trampoline_patch`.

**SMP4. The trampoline page is identity-mapped, read-execute, only
during bring-up.** `trampoline_install` maps `0x8000` RX in the kernel
tables; `arch_smp_finish` unmaps it and shoots the translation down after
the last start attempt. No other identity mapping exists in kernel space.
Check: review; `smp-shootdown` proves the shootdown path works.

**SMP5. An AP's bootstrap stack is freed by that AP's idle thread.**
`smp_init` records it in `percpu->boot_stack`; `idle_main` frees it on
its first iteration, after `sched_start_cpu` has switched to the idle
thread's own stack. Nothing else frees it. Check: `smp-online` verifies
`boot_stack == 0` on every AP.

**SMP6. `x86_cpu_init` runs once; APs run `x86_cpu_enable_features`.**
The shared `struct x86_cpu_info` is written by the boot CPU only.
Check: review.

## Interrupts and shootdown

**SMP7. Shootdown and cross-CPU calls run with interrupts enabled.**
Both wait for other CPUs, and a target spinning on a lock with interrupts
off can answer only if the initiator did not disable interrupts. Check:
`KASSERT(arch_irq_enabled())` in `arch_mmu_shootdown` and
`smp_call_function_single`.

**SMP8. Every unmap shoots down after releasing the space lock and
before freeing frames.** `region_teardown` works in chunks of
`TEARDOWN_CHUNK_PAGES` (32): under `kernel_space.lock` it records the
frames and unmaps (local invalidation), releases the lock, calls
`arch_mmu_shootdown`, then frees the frames. A frame is never reusable
while a stale translation to it may exist on another CPU. Check: review
of `vm_kernel_free`/`vm_unmap_phys`; `smp-shootdown` counts one
initiation and `online - 1` acknowledgements per chunk.

**SMP9. A reschedule IPI is sent only when it can matter.**
`request_resched` sets the target's `need_resched` and sends
`IPI_RESCHEDULE` only if the target is another online CPU; `sched_wake`
and `sched_enqueue_new` request it only when the target is idle or
running lower priority. Check: review; `smp-wake` bounds the wake
latency of an idle CPU to under 2 ms, well below `TICK_NS`.

**SMP10. IPI handlers take no lock that an initiator holds while
waiting.** `ipi_reschedule` does nothing, `ipi_call` runs the mailbox
function, `ipi_tlb_flush` invalidates and increments an atomic,
`ipi_halt` halts. Check: review of `kernel/interrupt/ipi.c`.

## Scheduler

**SMP11. Preemption never treats the interrupted thread as blocked.**
`waitqueue_prepare` marks the current thread BLOCKED before the condition
is evaluated; if a tick or IPI preempts the thread inside that window,
`schedule_internal(preempt = true)` re-queues it regardless of the
transient state. Only a voluntary `schedule()` with state BLOCKED hands
the thread to its wait queue. Found by the hang watchdog during bring-up:
thread 0 was left BLOCKED on no run queue and no wait list after a tick
preempted it between `waitqueue_prepare` and `waitqueue_finish` while the
reaper's wake had set `need_resched`; every CPU then idled in `hlt` and
roughly one in three four-CPU boots stalled. When the re-queued thread
runs again its state is RUNNING, so a subsequent `sched_block_current`
acts as a yield, the `wait_event` loop re-prepares and re-checks the
condition, and no wakeup is lost. Check: 24 consecutive stall-free
four-CPU boots after the fix; the watchdog stays armed during self-tests.

**SMP12. Exited threads are reaped by the reaper thread.**
`sched_finish_switch` may run with interrupts disabled (resumed inside a
trap handler), and freeing a stack needs a shootdown, so it hands the
thread to `thread_reap_later`; the reaper thread calls `thread_put` in
ordinary context. Consequence: `thread_count()` settles asynchronously
after `thread_join`; tests use `threads_settle`. Check: review;
`smp-affinity` and the Phase 3 tests observe the count return to
baseline within 200 ms.

**SMP13. A wake of a thread that is not BLOCKED is a no-op** (unchanged
from S-series): with SMP this is what makes a wake racing a preempted
re-queue harmless. Check: `sched_wake` returns false; test `waitqueue`.

**SMP14. The idle thread is the only thread that runs when a queue is
empty, and it never sits on a queue.** APs enter it through
`sched_start_cpu` from a context that is never resumed. Check: review;
`sched_dump` shows `queued 0` with `current 'idle'` on an idle CPU.

## Stopping

**SMP15. A panic is claimed once, stops the others, then prints.**
`panic_common` claims `g_panicking` with a compare-and-swap, calls
`smp_stop_others`, then `console_set_panic_mode` before the first line.
A concurrent panicker prints one line and halts; a recursive one prints
one line, requests emulator exit, and halts. Check: review; the crash
test exercises the single-CPU path with four CPUs online.

**SMP16. `kernel_shutdown` stops other CPUs before the emulator exit.**
Otherwise a running AP could keep writing the serial log after the
verdict. Check: review; the boot test's exit code arrives after the last
log line.

**SMP17. Console output is serialised except in panic mode.**
`console_write` takes `g_console_lock` irqsave; panic mode bypasses it
permanently. Check: review; no interleaved lines in the self-test logs.

## Gaps (documented, not invariants)

- Threads never migrate; `pick_cpu` balances at creation only.
- No CPU offlining or hotplug; `IPI_HALT` is one-way.
- Shootdown targets every online CPU because all kernel mappings are
  global; per-space filtering arrives with user address spaces.
- The AP online timeout (500 ms) and the IPI acknowledgement timeout
  (1 s) are tuned for TCG, not measured on hardware.
