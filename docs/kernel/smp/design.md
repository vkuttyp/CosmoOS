# SMP: Design

## 1. Bring-up sequence

`smp_init()` (kernel/core/smp.c), on the BSP with interrupts enabled,
after `sched_init`:

```text
for each MADT processor entry whose hw id != arch_smp_boot_hw_id():
    cpu = next index (1, 2, ...); stop at CONFIG_MAX_CPUS
    pc = kzalloc(struct percpu); percpu_register(pc, cpu)
    stack = vm_kernel_alloc(16 KiB, GUARD|POPULATE); pc->boot_stack = stack
    rc = arch_smp_start_cpu(cpu, hw_id, stack + 16 KiB)
    if rc == 0: wait up to 500 ms for cpu_online(cpu), else log and continue
log "smp: N of M CPUs online"
```

`arch_smp_start_cpu(cpu, apic_id, stack_top)` (x86-64):

1. First call only: identity-map the trampoline page `0x8000` RX in the
   kernel tables and copy `x86_trampoline_start..end` there through the
   direct map.
2. Patch the data block: `cr3 = kernel_space.mmu.root`, `entry =
   x86_ap_entry`, `stack = stack_top`, `cpu = cpu`. Record the APIC id
   for the index (`x86_cpu_set_apic_id`).
3. Clear `ap_started[cpu]`. Send INIT (assert, deassert), `udelay(10 ms)`,
   SIPI with vector `0x08`, `udelay(200 us)`; if not started, a second
   SIPI and another `udelay(200 us)`. Poll `ap_started` for up to 100 ms.
4. Return 0 or `-ETIMEDOUT`.

After all APs are started, `arch_smp_finish()` unmaps the trampoline
page and shoots the mapping down.

The kernel root page table is allocated below 4 GiB
(`PMM_FLAGS_ZONE_DMA32`) because the 32-bit stage loads it into CR3
before long mode.

### Trampoline (`trampoline.S`)

Assembled into `.rodata` as position-independent code with all
addresses computed as `symbol - x86_trampoline_start + 0x8000`:

```text
.code16  cli; cld; ds=es=ss=cs; lgdt tr_gdtr; CR0.PE=1; ljmp 0x08:pm32
.code32  ds=es=ss=0x10; CR4.PAE=1; CR3=tr_cr3; EFER.LME|NXE=1;
         CR0.PG|WP=1; ljmp 0x18:lm64
.code64  ds=es=ss=0x10; rsp=tr_stack; rdi=tr_cpu; rbp=0; push 0; jmp *tr_entry
data     tr_gdt (null, code32, data32, code64), tr_gdtr, tr_cr3,
         tr_entry, tr_stack, tr_cpu
```

The AP runs from linear `0x8xxx` after paging is enabled, so the
identity mapping must be present until the `jmp *tr_entry` lands in the
kernel image.

### AP C entry (`x86_ap_entry(cpu)`)

Runs on the bootstrap stack with interrupts disabled:

1. `ap_started[cpu] = 1` (releases the BSP's wait).
2. `gdt_init_cpu(cpu)` (own GDT/TSS/double-fault stack, allocated by the
   BSP beforehand), `idt_load()`.
3. `arch_percpu_install(percpu_get(cpu))` — after the GDT load (S16).
4. `x86_cpu_enable_features()` (WP, NXE, PGE, SMEP, SMAP, UMIP).
5. `arch_irqc_init_cpu()` (LAPIC enable, record APIC id).
6. `timer_init_cpu()` (per-CPU queue, LAPIC tick).
7. `sched_start_cpu()`: run queue, idle thread, mark online, switch to
   idle. The idle thread frees `pc->boot_stack` on its first iteration.

`x86_cpu_init` (BSP) is split into `x86_cpu_identify()` and
`x86_cpu_enable_features()`; APs run only the latter, so the shared
CPU-info block is written once.

## 2. IPIs (`kernel/interrupt/ipi.c`)

```c
enum ipi_kind { IPI_RESCHEDULE, IPI_CALL, IPI_TLB_FLUSH, IPI_HALT, IPI_KIND_COUNT };
```

`ipi_init()` allocates one vector per kind (`arch_vector_alloc`) and
registers handlers with `interrupt_register`. `ipi_send(cpu, kind)` and
`ipi_broadcast_others(kind)` translate to `arch_ipi_send/broadcast`.

- **RESCHEDULE**: empty handler. The sender already set the target's
  `need_resched` under its run-queue lock; the target's interrupt-return
  path performs the switch.
- **CALL**: global mailbox `{ fn, arg, target, done }` under `call.lock`.
  `smp_call_function_single(cpu, fn, arg)` fills it, sends the IPI, and
  spins on `done` with interrupts enabled (asserted), timing out into a
  panic after 1 s. The handler runs `fn(arg)` in interrupt context and
  sets `done` with release semantics. Calling one's own CPU runs `fn`
  directly with interrupts disabled.
- **TLB_FLUSH**: handler calls `x86_shootdown_handle()`.
- **HALT**: handler marks the CPU offline and `arch_cpu_halt_forever()`.

## 3. TLB shootdown

```c
void arch_mmu_shootdown(const struct arch_mmu_context *ctx, vaddr_t va, size_t len);
```

```text
KASSERT(arch_irq_enabled())
if online CPUs == 1: local invalidate; return
spin_lock(shootdown.lock)             /* not irqsave: acks arrive as IPIs */
request = {va, len}; ack = 0; targets = online - 1
ipi_broadcast_others(IPI_TLB_FLUSH)
local invalidate
spin while ack < targets (with a 1 s timeout → panic naming the count)
spin_unlock
```

Handler: read `request`, `arch_mmu_invalidate` locally, atomic increment
`ack`. Statistics: initiated, acks received, flushes handled per CPU.

### VMM changes

`vm_kernel_free` and `vm_unmap_phys` no longer free frames or unmap under
the space lock in one step. Per chunk of up to 32 pages: lock, query and
record the frames, unmap (local invalidate), unlock, `arch_mmu_shootdown`
the chunk, free the frames. The region is unlinked under the lock only
after the last chunk. Mapping never needs a shootdown (not-present →
present). The trampoline identity mapping is created and removed with
the space lock held around the table edit and a shootdown after.

## 4. Scheduler changes

- `request_resched(rq)`: set `need_resched`; if `rq->cpu != this cpu`,
  `ipi_send(rq->cpu, IPI_RESCHEDULE)`.
- `thread_create_on(entry, arg, name, prio, cpumask)`: placement respects
  the mask; `thread_create` is the `CPUMASK_ALL` case.
- `idle_main`: frees `percpu->boot_stack` once, then loops.
- `sched_start_cpu` unchanged in shape; now actually called.

## 5. Stopping other CPUs

`smp_stop_others()`: `ipi_broadcast_others(IPI_HALT)` when more than one
CPU is online, then spin briefly (up to 10 ms) for `cpu_online` counts
to drop. `panic_common` calls it right after disabling interrupts and
claiming the panic (atomic compare-and-swap on `g_panicking`; a second
CPU that loses the race prints one line and halts itself).
`kernel_shutdown` calls it before the emulator exit.

## 6. Console lock

`console_write` takes `console.lock` (irqsave) unless `console_panic_mode`
is set, which `panic_common` sets before printing. Lines from different
CPUs no longer interleave mid-line in normal operation.

## 7. Per-CPU tables (`gdt.c`)

```c
struct x86_cpu_tables { struct gdt gdt; struct tss tss; vaddr_t df_stack; };
```

CPU 0 uses the static instance; `gdt_alloc_cpu(cpu)` (BSP, before start)
allocates the block with `kzalloc` and the 8 KiB double-fault stack with
`vm_kernel_alloc(GUARD|POPULATE)`; `gdt_init_cpu(cpu)` on the AP loads
them.

## 8. Failure modes

| Condition | Behaviour |
|---|---|
| AP never starts (no SIPI response) | `-ETIMEDOUT`; the trampoline entry is repointed at a halt stub, the page stays mapped, and bring-up stops so a late arrival can never find another CPU's stack and index (review finding, PR #4) |
| AP starts but never reaches online | logged after 500 ms, skipped (its stack and percpu leak) |
| more MADT CPUs than CONFIG_MAX_CPUS | extra ignored with a warning |
| shootdown not acknowledged in 1 s | panic listing acks/targets |
| smp_call_function_single timeout | panic |
| shootdown/call with interrupts disabled | KASSERT panic |
| concurrent panics | first wins; others print one line and halt |
| kernel root table above 4 GiB | prevented: allocated from DMA32 |
