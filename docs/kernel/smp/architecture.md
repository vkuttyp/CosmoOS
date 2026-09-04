# SMP: Architecture

Phase 3, part 2. Brings the remaining CPUs online and makes the kernel
correct when several CPUs run at once.

## 1. Purpose

Start every application processor (AP) the firmware reports, give each
its own per-CPU state, descriptor tables, local interrupt controller,
tick, run queue, and idle thread; provide inter-processor interrupts
(IPIs) for rescheduling, synchronous function calls, TLB shootdown, and
halting; and make the paths that were single-CPU by omission (TLB
invalidation, console output, panic, shutdown) correct on many CPUs.

## 2. Where it sits

```text
   kernel_main            smp_init() after sched_init, before self-tests
        ▼
   kernel/core/smp.c      enumerate CPUs (ACPI), allocate percpu + boot
                          stack per AP, start each, wait for online
        │ arch_smp_start_cpu
        ▼
   kernel/arch/x86_64/smp.c        INIT-SIPI-SIPI, trampoline patching,
   kernel/arch/x86_64/trampoline.S 16→32→64-bit real-mode entry
        │ x86_ap_entry
        ▼
   per-CPU init: GDT/TSS, IDT, percpu (GS), CPU features, LAPIC, tick,
   then sched_start_cpu() → idle loop

   kernel/interrupt/ipi.c   IPI kinds → vectors; reschedule, call,
                            TLB flush, halt handlers
   kernel/arch/x86_64/mmu.c arch_mmu_shootdown(): local flush + IPI + acks
```

## 3. Responsibilities

- **Bring-up** (`kernel/core/smp.c`, arch `smp.c`, `trampoline.S`):
  sequential start of each AP with a timeout; per-AP `struct percpu`,
  bootstrap stack, descriptor tables, double-fault stack.
- **IPIs** (`kernel/ipi.h`, `kernel/interrupt/ipi.c`): symbolic kinds
  (`IPI_RESCHEDULE`, `IPI_CALL`, `IPI_TLB_FLUSH`, `IPI_HALT`) mapped to
  dynamic vectors at init; `smp_call_function_single()` runs a function
  on another CPU and waits.
- **Scheduler** (`sched.c`): a wake or new thread on another CPU's queue
  kicks that CPU with `IPI_RESCHEDULE`; APs run the same tick and policy.
- **TLB shootdown** (`arch/mmu.h`): `arch_mmu_shootdown()` invalidates a
  range on every online CPU and waits for acknowledgements. The VMM
  performs unmaps under its lock, then shoots down and frees frames
  after releasing it.
- **Stopping** (`panic.c`, `shutdown.c`): the panicking or shutting-down
  CPU halts all others with `IPI_HALT` before printing or exiting.
- **Console** (`console.c`): a spinlock serialises lines; panic bypasses
  it so a halted holder cannot block the report.

## 4. Non-responsibilities (later)

- Load balancing and migration after creation; CPU hotplug/offlining.
- x2APIC mode, MSI, IOAPIC affinity changes at runtime.
- TSC synchronisation checks between sockets; per-CPU clock offsets.
- RCU/epoch reclamation; reader-writer locks; lock-order checker.
- Lazy TLB and per-address-space shootdown filtering (all kernel
  mappings are global, so every CPU is a target).

## 5. Interfaces

| Header | Provides |
|---|---|
| `kernel/smp.h` | `smp_init`, `smp_call_function_single`, `smp_stop_others` |
| `kernel/ipi.h` | `enum ipi_kind`, `ipi_init`, `ipi_send`, `ipi_broadcast_others` |
| `arch/smp.h` | `arch_smp_boot_hw_id`, `arch_smp_start_cpu` |
| `arch/mmu.h` (added) | `arch_mmu_shootdown`, `arch_mmu_shootdown_stats` |
| `kernel/thread.h` (added) | `thread_create_on` (affinity mask) |

## 6. Data structures

Per-AP: `struct percpu` (heap), bootstrap stack (arena, freed by the
idle thread once it runs on its own stack), `struct x86_cpu_tables`
(GDT, TSS, double-fault stack). Trampoline: a copied blob at physical
`0x8000` with a data block (CR3, entry, stack, CPU index) patched by the
BSP. Shootdown: one global request (`va`, `len`, `ack` counter) under a
spinlock. Call: one global mailbox (`fn`, `arg`, `done`) under a
spinlock.

## 7. Concurrency model

Bring-up is sequential from the BSP with all other CPUs idle. IPI
handlers run in interrupt context and take no locks except the
run-queue lock through `sched_wake` (never happens: the reschedule
handler does nothing; the interrupt-return path acts on `need_resched`).

`arch_mmu_shootdown` and `smp_call_function_single` wait for other CPUs
with interrupts **enabled** and assert it, because a target that is
itself spinning on a lock with interrupts off can only respond if the
initiator did not disable interrupts first. Consequently neither may be
called under an irqsave spinlock; the VMM restructures its free paths
accordingly.

Lock order additions: `shootdown.lock` and `call.lock` are leaves taken
with interrupts enabled; `console.lock` is a leaf taken irqsave.

## 8. Memory ownership

The BSP allocates each AP's percpu block, bootstrap stack, and tables;
the percpu block and tables live forever; the bootstrap stack is owned
by the AP's idle thread after the switch and freed by it. The trampoline
page is reserved low memory owned by the PMM (never allocatable) and is
identity-mapped only during bring-up.

## 9. Error handling

An AP that does not report within the timeout is logged and skipped;
the kernel continues with the CPUs that came up. A shootdown or call
that is not acknowledged within a generous bound panics with the CPUs
that failed to answer, because continuing would leave stale
translations or a wedged CPU.

## 10. Performance considerations

Shootdown broadcasts to all CPUs and waits for all; it is used only on
unmap, which is rare. Reschedule IPIs are sent only when the target is
idle or running lower priority. Nothing is measured yet.

## 11. Security considerations

The trampoline executes from a fixed low physical page with a W^X
identity mapping (RX) that exists only during bring-up. APs enable the
same protections (WP, NXE, SMEP/SMAP/UMIP where available) before
running any generic code.

## 12. Testing strategy

Boot under QEMU with `-smp 4`: every reported CPU online; threads pinned
to each CPU run there; parallel spinners make concurrent progress;
cross-CPU wake latency bounded; function calls land on the target;
shootdown acknowledgement counts match; per-CPU ticks advance; mutex
contention across CPUs stays correct. See `testing.md`.

## 13. Future extensibility

Per-CPU data structures and IPI kinds are the base for load balancing,
RCU grace periods, and user address spaces (per-space shootdown
filtering with a CPU mask of users). AArch64 implements `arch/smp.h`
with PSCI `CPU_ON` and `arch_ipi_*` with GIC SGIs.
