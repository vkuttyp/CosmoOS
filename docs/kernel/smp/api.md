# SMP: API

Every function below is internal kernel ABI: it may change between
kernel versions and never reaches user space. Contracts follow the
constitution's section 52 fields.

## Shared contracts

- **Nothing sleeps.** Waiting is busy-waiting with interrupts enabled.
- **Interrupts-enabled requirement.** `arch_mmu_shootdown()` and
  `smp_call_function_single()` wait for other CPUs to acknowledge an IPI.
  A target that is spinning on a spinlock with interrupts disabled can
  only take that IPI once the lock holder releases; if the initiator
  disabled interrupts before waiting, and the target is spinning on a
  lock the initiator holds, neither side progresses. Both functions
  therefore `KASSERT(arch_irq_enabled())`, and callers must have released
  every irqsave spinlock first (the VMM does this in `region_teardown`).
- **Lock order additions.** `g_shootdown_lock` (kernel/arch/x86_64/mmu.c)
  and `g_call_lock` (kernel/interrupt/ipi.c) are leaves taken with
  `spin_lock` (preemption off, interrupts on). `g_console_lock`
  (kernel/core/console.c) is an irqsave leaf. Neither is ever taken by an
  interrupt handler.
- **Error convention.** Bring-up failures are logged and skipped;
  protocol failures (unacknowledged IPI, call to an offline CPU) panic.

## kernel/smp.h

### `void smp_init(void)`
- **Purpose**: start every application processor the MADT reports.
- **Inputs**: `acpi_madt_cpus()`, `arch_smp_boot_hw_id()`.
- **Outputs**: registered `struct percpu` for each CPU that came up; log
  line `smp: N CPUs online of M reported`.
- **Ownership/lifetime**: allocates each AP's `struct percpu` (kmalloc,
  permanent), bootstrap stack (arena, freed by that AP's idle thread via
  `percpu->boot_stack`), and arch tables (`arch_smp_prepare_cpu`,
  permanent). A CPU that never comes online keeps its allocations.
- **Concurrency**: boot CPU only, after `sched_init`, other CPUs idle.
- **Blocking**: busy-waits up to 500 ms per AP for `cpu_online`.
- **Interrupt context**: no; asserts interrupts enabled.
- **Failure modes**: `-ETIMEDOUT` from `arch_smp_start_cpu`, no memory,
  more than `CONFIG_MAX_CPUS` entries: logged with `kwarn`, CPU skipped.

### `void smp_call_function_single(unsigned cpu, smp_call_fn fn, void *arg)`
- **Purpose**: run `fn(arg)` on `cpu` and return after it completed.
- **Inputs**: online CPU index, non-NULL `fn`.
- **Outputs**: none; `fn` runs in interrupt context on the target with
  interrupts disabled. On the calling CPU it runs directly under
  `arch_irq_save`.
- **Ownership**: `arg` is the caller's; it must stay valid until return.
- **Concurrency**: one call in flight system-wide (`g_call_lock`, a
  mailbox of `g_call_fn`, `g_call_arg`, `g_call_done`).
- **Blocking**: spins on `g_call_done` with interrupts enabled, up to 1 s.
- **Interrupt context**: caller no (asserts); `fn` yes, so `fn` may only
  use interrupt-safe operations.
- **Failure modes**: panic if `cpu` is offline or does not answer in 1 s.

### `void smp_stop_others(void)`
- **Purpose**: halt every other online CPU (panic, shutdown).
- **Outputs**: `IPI_HALT` broadcast; waits up to 10 ms for
  `cpu_online_mask()` to shrink to the caller.
- **Concurrency/interrupt context**: usable with interrupts disabled; a
  target with interrupts off may never acknowledge, which is tolerated.
- **Failure modes**: none reported; returns after the bound.

## kernel/ipi.h

### `enum ipi_kind`
`IPI_RESCHEDULE`, `IPI_CALL`, `IPI_TLB_FLUSH`, `IPI_HALT`,
`IPI_KIND_COUNT`. Vectors are allocated at `ipi_init` from the dynamic
range and never exposed.

### `void ipi_init(void)`
- **Purpose**: allocate one vector per kind (`arch_vector_alloc`) and
  register handlers with `interrupt_register`.
- **Concurrency**: boot CPU, after `irq_init`, before `sched_init`.
- **Failure modes**: panic on vector exhaustion or registration failure.

### `void ipi_send(unsigned cpu, enum ipi_kind kind)` / `void ipi_broadcast_others(enum ipi_kind kind)`
- **Purpose**: deliver `kind` to one CPU or to all others.
- **Blocking**: spins only on the controller's ICR busy bit.
- **Interrupt context**: yes (used by `sched_wake` from timer callbacks).
- **Failure modes**: `KASSERT` on an invalid kind or before `ipi_init`.

### `uint64_t ipi_count(enum ipi_kind kind)`
IPIs of `kind` handled on the calling CPU; diagnostics only.

## arch/smp.h

### `uint32_t arch_smp_boot_hw_id(void)`
Local interrupt controller id (APIC id) of the boot CPU, from
`x86_cpu_apic_id(0)`; used to skip the BSP in the MADT list.

### `int arch_smp_prepare_cpu(unsigned cpu)`
- **Purpose**: allocate `cpu`'s GDT, TSS, and double-fault stack on the
  boot CPU (`gdt_alloc_cpu`).
- **Failure modes**: `-ENOMEM`.

### `int arch_smp_start_cpu(unsigned cpu, uint32_t hw_id, uintptr_t stack_top)`
- **Purpose**: copy and patch the trampoline, record the APIC id, send
  INIT then SIPI (twice if needed), wait for the AP to reach
  `x86_ap_entry`.
- **Inputs**: `cpu` in `[1, CONFIG_MAX_CPUS)`, the AP's APIC id, a
  16-byte-aligned stack top the AP will run on.
- **Blocking**: `udelay` 10 ms after INIT, 200 µs after each SIPI, then
  polls `g_ap_started[cpu]` for up to 100 ms.
- **Failure modes**: `-ETIMEDOUT`; panic if the trampoline page cannot be
  identity-mapped.
- **ABI**: the trampoline data block layout is private to
  `trampoline.S`/`smp.c` (offsets exported as `x86_trampoline_off_*`).

### `void arch_smp_finish(void)`
Unmap the trampoline identity page and shoot the translation down.
Requires interrupts enabled (it calls `arch_mmu_shootdown`).

## arch/mmu.h additions

### `void arch_mmu_shootdown(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)`
- **Purpose**: invalidate `[va, va+len)` on every online CPU and wait for
  acknowledgement.
- **Concurrency**: `g_shootdown_lock`; one request (`g_shootdown_va`,
  `g_shootdown_len`, `g_shootdown_acks`) at a time. With one CPU online it
  degenerates to `arch_mmu_invalidate`.
- **Blocking**: spins for `online - 1` acks, up to 1 s.
- **Interrupt context**: no; asserts interrupts enabled (see shared
  contracts).
- **Failure modes**: panic naming acks received and targets expected.

### `void arch_mmu_shootdown_stats(struct arch_mmu_shootdown_stats *out)`
Per-CPU counters: `initiated`, `handled` (flush IPIs served here),
`acks_received`. Diagnostics; used by the `smp-shootdown` self-test.

### `void arch_mmu_shootdown_ipi_handler(void)`
Body of the `IPI_TLB_FLUSH` handler; called by `kernel/interrupt/ipi.c`
on the target in interrupt context. Reads the pending range,
invalidates locally, increments the ack counter. Not for other callers.

## kernel/thread.h addition

### `struct thread *thread_create_on(entry, arg, name, priority, cpumask_t affinity)`
- **Purpose**: `thread_create` restricted to the CPUs in `affinity`.
- **Outputs**: the thread is placed on the least loaded online CPU in the
  mask at creation and never migrates.
- **Failure modes**: NULL if the mask contains no online CPU or on
  allocation failure. `thread_create` is `thread_create_on(...,
  CPUMASK_ALL)`.

## kernel/percpu.h additions

- `uintptr_t boot_stack`: an AP's bootstrap stack (arena address). Set by
  `smp_init`, cleared and freed by that CPU's idle thread on its first
  iteration. Zero on the boot CPU.
- `uint32_t hw_id`: local interrupt controller id, written by
  `smp_init` (BSP) and `x86_ap_entry` (APs). Diagnostics only.

## kernel/console.h addition

### `void console_set_panic_mode(void)`
Irreversibly stops `console_write` from taking `g_console_lock`. Called by
`panic_common` after `smp_stop_others`, so a halted CPU that held the
lock cannot block the report. Interrupt-safe.

## Arch-private (kernel/arch/x86_64/include/x86/)

- `int gdt_alloc_cpu(unsigned cpu)` / `void gdt_init_cpu(unsigned cpu)`
  (`gdt.h`): allocate and load a CPU's `struct x86_cpu_tables` (GDT,
  TSS, 8 KiB guarded double-fault stack). `gdt_init_cpu` resets the GS
  base; `arch_percpu_install` must follow it.
- `void x86_cpu_enable_features(void)` (`cpu.h`): WP, NXE, PGE, SMEP,
  SMAP, UMIP from the shared info block; APs call this instead of
  `x86_cpu_init`, which also identifies the processor and is boot-CPU only.
- `void x86_ap_entry(unsigned cpu)` (`cpu.h`): trampoline target; never
  returns (ends in `sched_start_cpu`).

## kernel/sched.h: hang watchdog

### `void sched_watchdog_arm(uint64_t timeout_ns)` / `void sched_watchdog_kick(void)` / `void sched_watchdog_disarm(void)`
- **Purpose**: if no kick arrives within `timeout_ns` while armed, the
  boot CPU's tick prints `sched_dump()` once (every CPU's run queue with
  `need_resched`, `preempt_count`, `irq_depth`, `ticks`; every thread
  with state and `waiting_on`).
- **Concurrency**: the timeout is published with release/acquire; the
  kick timestamp is a plain store from the kicking thread. One
  comparison per tick on CPU 0.
- **Interrupt context**: arm/kick/disarm anywhere; the dump runs in the
  tick handler.
- **Failure modes**: none; purely diagnostic. `selftest_run_all` arms it
  for 8 s and kicks before each test.
