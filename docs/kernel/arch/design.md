# x86-64 Architecture Layer: Design

This document walks the implementation files in the order the CPU
executes them, then covers the cross-cutting decisions.

## entry.S: from loader to C

Entry state is defined by `boot/protocol/cosmoboot.h`: long mode, paging
on with the kernel at its link address, `RDI` = HHDM-virtual pointer to
`struct cosmoboot_info`, interrupts disabled, a loader-owned stack.

`_start` does four things and nothing else:

1. `cli; cld` (belt and braces; the loader already cleared IF).
2. `lea x86_boot_stack_top(%rip), %rsp` moves onto the 64 KiB boot stack in
   `.bss.boot_stack`. The loader's stack is `COSMOBOOT_MEM_LOADER_RECLAIMABLE`
   and must not be used after this point.
3. `pushq $0; pushq $0` builds a fake outermost frame (return address 0,
   saved `rbp` 0). `backtrace.c` stops at a zero return address, so every
   walk terminates without special-casing the boot thread. The two pushes
   also keep `rsp` 16-byte aligned so the `call` below satisfies the SysV
   ABI.
4. `call x86_start` with `RDI` untouched.

The same file emits `.note.cosmoboot` (name `COSMO`, type
`COSMOBOOT_NOTE_TYPE`, descriptor `COSMOBOOT_VERSION`) so the loader can
verify the protocol version before jumping. `cosmoboot.h` guards its C
declarations with `__ASSEMBLER__` for exactly this include.

## start.c: initialisation order

```
arch_console_early_init()   serial probe + console sink
gdt_init()                  GDT, TSS, IST1 double-fault stack
idt_init()                  256 gates, lidt
pic_init_masked()           remap to 32-47, mask all
x86_cpu_init()              CPUID, WP/NXE/PGE/SMEP/SMAP/UMIP
kernel_main(info)
```

The order is the design: console first so every later failure is
visible; GDT before IDT because the IDT's `#DF` gate references IST1 in
the TSS; IDT before anything that could fault; PIC before `sti` ever
happens so a spurious IRQ 7 cannot land on vector 15 (`#PF`-adjacent
reserved) or IRQ 0 on `#DE`; CPU features last because they are the only
step that can change semantics (SMAP) rather than just install tables.

## cpu.c: identification and protection

`x86_cpu_init` reads CPUID leaves 0, 1, 7, 0x80000000-0x80000004 and
0x80000007 into `struct x86_cpu_info` (vendor, family/model/stepping with
the extended-family/model fixups, brand string with leading-space strip,
and feature bits NX, SMEP, SMAP, UMIP, PGE, APIC, x2APIC, FSGSBASE,
invariant TSC). It then asserts CR0.WP, EFER.NXE (when NX exists), and
adds CR4.PGE/SMEP/SMAP/UMIP as supported. Doing this in the kernel rather
than trusting the loader keeps the protection state independent of which
loader was used.

## fpu.c: FPU and SIMD state

The kernel is compiled `-mgeneral-regs-only` on every architecture and
never touches x87, SSE or AVX registers itself; user threads and guests
do, so their state must be owned explicitly (`arch/fpu.h`):

- **CPU policy**, asserted on every CPU by `x86_fpu_init_cpu` from
  `x86_cpu_enable_features` (the boot CPU inherits whatever the firmware
  left, an AP starts with CR4 = PAE only): CR0.EM = 0, MP = 1, NE = 1,
  TS = 0; CR4.OSFXSR = 1 and OSXMMEXCPT = 1; with XSAVE, CR4.OSXSAVE = 1
  and XCR0 = x87 | SSE | AVX (| AVX-512 as a unit) as supported, identical
  on every CPU so a thread's area has one layout wherever it runs. The
  area size comes from `CPUID.(0DH,0):EBX` for that XCR0 (832 bytes with
  AVX, 512 for FXSAVE) and heterogeneous CPUs are refused.
- **Ownership**: a thread owns state iff `thread->fpu != NULL`;
  `arch_fpu_alloc` gives a user thread a 64-byte-aligned area holding
  the reset image (FCW 0x37F, MXCSR 0x1F80, XSTATE_BV 0) before it can
  run; `arch_fpu_free` runs from `thread_put`. Kernel threads own nothing.
- **Eager switching**: `arch_thread_switch_prepare(prev, next)` calls
  `x86_fpu_switch`: XSAVE (not XSAVEOPT: the area may be restored on
  another CPU) of the outgoing owner's registers into its area, XRSTOR of
  the incoming owner's. No lazy ownership, no `#NM` juggling, no
  cross-CPU state: the registers a kernel thread runs with are stale and
  unreadable by construction (the compiler flag).
- **Guests** (`svm.c`): around every VMRUN the owner thread's state is
  saved, the vCPU's own area loaded, the guest's XCR0 installed, and
  afterwards the guest's registers captured and the owner's restored (a
  kernel-thread owner gets the reset image so nothing of the guest stays
  live). XSETBV is intercepted and emulated with the hardware's rules
  against the host's XCR0, since XCR0 is not in the VMCB.

Tests: `fpu-switch` (two state-owning kernel threads pinned to one CPU
trade patterns across 400 yields), `hv-guest-fpu` (a fresh guest sees the
reset state, the owner keeps its own), and `init --selftest` spawning two
`--fpu-partner` processes. Both the FXSAVE path (`qemu64`) and the XSAVE
path (`-cpu qemu64,+xsave,+avx`) are exercised.

## lapic.c: the ICR write pair

The xAPIC ICR is two 32-bit registers and the write to ICR_LO sends
with whatever ICR_HI holds. `icr_write_pair` disables local interrupts
around `wait-idle, write HI, write LO`: a tick handler on this CPU that
wakes a thread pinned elsewhere sends its own IPI from interrupt context,
and between the two writes of a cross call in progress it would redirect
that call. The trailing wait for idle happens with interrupts on. Nothing
else writes the ICR, and an NMI handler (which cannot be masked) must not
send IPIs. Verified by `smp-ipi-storm`: 300 ms of cross calls to every
other CPU against a timer that wakes threads on those CPUs every tick.

`cpu.c` also implements `arch/cpu.h` and `arch/irq.h`:
`arch_irq_save` reads RFLAGS then `cli`; `arch_irq_restore` executes `sti`
only if IF was set in the saved value, so nested save/restore pairs
compose. `arch_cpu_wait_for_interrupt` uses `sti; hlt; cli` when
interrupts are off so the wake-up cannot be lost between the two
instructions (the `sti` shadow covers `hlt`).

## gdt.c: selectors and TSS

| Selector | Descriptor |
|---|---|
| `0x00` | null |
| `0x08` `GDT_KERNEL_CODE` | `0x00AF9A000000FFFF` 64-bit code, DPL 0 |
| `0x10` `GDT_KERNEL_DATA` | `0x00CF92000000FFFF` data, DPL 0 |
| `0x18` `GDT_USER_DATA` | `0x00CFF2000000FFFF` data, DPL 3 |
| `0x20` `GDT_USER_CODE` | `0x00AFFA000000FFFF` 64-bit code, DPL 3 |
| `0x28` `GDT_TSS` | 16-byte available-TSS descriptor, type 0x9 |

User data precedes user code because `SYSRET` loads `SS = STAR[63:48] + 8`
and `CS = STAR[63:48] + 16`; with `STAR[63:48] = 0x10` that yields
`SS = 0x18` and `CS = 0x20` with RPL 3 added by the CPU. Fixing the layout
now means Phase 4 sets one MSR and touches no descriptors.

The TSS (`struct tss`, 104 bytes, `_Static_assert`ed) holds
`ist[0]` = top of `g_df_stack` (8 KiB, 16-byte aligned) and
`iomap_base = sizeof(tss)` (no I/O bitmap). `gdt_init` loads the GDT with
`lgdt`, reloads `CS` via `pushq/lea/lretq`, sets `DS/ES/SS` to
`GDT_KERNEL_DATA` and `FS/GS` to 0, then `ltr`. `gdt_set_kernel_stack`
exists so Phase 4 can set `rsp0` without touching this file.

## idt.c: gates

All 256 entries point at `x86_isr_stubs + vector * X86_ISR_STUB_SIZE`.
Attributes:

- interrupt gate type 0xE (IF cleared on entry), present, DPL 0 →
  `GATE_INTERRUPT_DPL0 = 0x8E`;
- vector 3 (`#BP`) uses `GATE_INTERRUPT_DPL3 = 0xEE` so `int3` from
  ring 3 is permitted;
- the paranoid vectors get their own IST stacks: vector 8 (`#DF`)
  `IST_DOUBLE_FAULT = 1`, vector 2 (NMI) `IST_NMI = 2`, vector 18 (`#MC`)
  `IST_MACHINE_CHECK = 3`, vector 1 (`#DB`) `IST_DEBUG = 4`; `gdt.c`
  allocates the four 8 KiB stacks per CPU (static for the boot CPU,
  guarded kernel allocations for the APs) and `gdt_ist_top(ist)` exposes
  them to diagnostics and the `trap-paranoid` self-test.

`idt_load` is separate from `idt_init` so additional CPUs can `lidt` the
shared table.

## isr.S: uniform stubs and the common path

A `.rept 256` loop emits one 16-byte-aligned stub per vector:

```
[pushq $0]        omitted for 8,10,11,12,13,14,17,21,29,30 (CPU pushed an error code)
pushq $vec
jmp isr_common
```

The longest stub (`pushq $0` 2 bytes, `pushq $imm32` 5 bytes, `jmp rel32`
5 bytes) is 12 bytes, so 16 is safe. `idt.c` computes addresses instead
of storing a 2 KiB table.

`isr_common` pushes `rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15` in that
order, so from the lowest address upward the stack reads
`r15 … r8, rbp, rdi, rsi, rdx, rcx, rbx, rax, vector, error_code, rip, cs,
rflags, rsp, ss`, which is exactly `struct arch_trap_frame`. It then
`cld`, `mov %rsp, %rdi`, `call x86_trap_dispatch`, pops in reverse,
`add $16, %rsp`, `iretq`.

### The paranoid path (`isr_paranoid`)

`isr_common` decides SWAPGS from the saved CS: ring 3 arrived with the
user's GS base. Four vectors cannot use that rule because they can
arrive at any instruction, including two windows where CS is already
the kernel's but GS or RSP is not: the SYSCALL entry between `swapgs`
and the kernel-stack load (`syscall_entry.S`), and the SYSRET exit
after `popq %rsp`. `#DB` (1), NMI (2), `#DF` (8) and `#MC` (18) are
routed by their stubs to `isr_paranoid`: the IDT put them on an IST
stack, so RSP is trusted; the entry reads `MSR_GS_BASE` and swaps only
if it is not a kernel address (every per-CPU block is in the higher
half, a user GS base never is), remembering the decision in `rbx`
(callee-saved across the C call, restored from the frame afterwards).
`x86_trap_paranoid` counts as interrupt context and dispatches the
handler; it never preempts and never delivers a kill, because the
interrupted context may be the scheduler holding a run-queue lock or a
system call with the user's stack live. An unregistered paranoid vector
panics through `arch_trap_unhandled` like any exception. NMI handlers
must not send IPIs (see `lapic.c` below). Verified by the `trap-paranoid`
self-test: a software NMI from kernel context lands on the NMI IST stack
with `irq_depth 1`, and one raised after a `swapgs` (the user GS base
live, as inside the SYSCALL window) still recovers the per-CPU block and
hands the original GS state back.

### Stack alignment at interrupt entry

In 64-bit mode the CPU aligns `rsp` to 16 before pushing the five-word
frame (`ss, rsp, rflags, cs, rip`), leaving `rsp ≡ 8 (mod 16)`. The stub
adds two words (`≡ 8`), `isr_common` adds fifteen (`120` bytes, `≡ 0`).
`call` pushes one more, so `x86_trap_dispatch` sees `rsp ≡ 8 (mod 16)`,
which is what the SysV ABI requires at function entry. No manual
alignment is needed and none is done.

## pic.c: quiescing the 8259A

ICW1-4 remap the master to `X86_VECTOR_IRQ_BASE` (32) and the slave to 40,
then both mask registers are set to 0xFF. Even masked, a floating IRQ line
can produce a spurious IRQ 7 or 15; `pic_is_spurious` reads the in-service
register to tell, and `pic_eoi` sends the correct acknowledgement (master
only for spurious IRQ 15, none for spurious IRQ 7). `trap.c` calls
`pic_eoi` after dispatch for vectors 32-47 so handlers never touch the
controller.

## trap.c: dispatch and diagnostics

`x86_trap_dispatch(frame)` calls `interrupt_dispatch(frame->vector, frame)`
then performs the PIC EOI for the legacy range. Policy for unregistered
vectors lives in `arch_trap_unhandled`: exceptions (vector < 32) call
`panic_frame` with the exception name; anything else increments a counter
and logs, with spurious legacy IRQs demoted to debug level.

`arch_trap_vector` maps `enum arch_trap_kind` to vector numbers
(`ARCH_TRAP_BREAKPOINT → 3`, `ARCH_TRAP_PAGE_FAULT → 14`, and so on) so
generic code never contains an x86 number.

`arch_trap_frame_dump` prints the vector and name, error code, `RIP/CS/
RFLAGS/RSP/SS`, all general registers, `CR0/CR3/CR4`, and for `#PF` also
`CR2` with the error-code bits decoded (present/write/user/reserved-bit/
instruction-fetch).

## serial.c: the early console

COM1 (`0x3F8`) is configured for 115200 8N1 with FIFOs enabled. Presence is
detected by writing `0xA5` in loopback mode and reading it back; on
failure `g_present` stays false and `serial_putc` returns immediately, so
a machine without a UART does not hang. `serial_write` converts `\n` to
`\r\n`. `arch_console_early_init` runs the probe and registers
`g_serial_sink` with `console_register`.

## backtrace.c: frame-pointer walk

Every object is built with `-fno-omit-frame-pointer`, so each frame is
`{ saved rbp, return address }`. `frame_ok` accepts a frame only if it is
non-null, 16-byte aligned, entirely inside `[__kernel_start, __kernel_end)`
(every stack lives in the image until the allocator exists), and strictly
above the previous frame. The walk also stops at a zero return address
(the fake frame from `entry.S`). When a trap frame is supplied, entry 0 is
its `rip` and the walk starts from its `rbp`.

## shutdown.c: emulator exit

`arch_emulator_exit(code)` does `outl(0xF4, code)`. QEMU's
`isa-debug-exit` device (enabled by `scripts/qemu-run.sh`) then exits with
status `(code << 1) | 1`; codes `0x10`/`0x11` map to 33/35. Current QEMU
processes the request asynchronously, so the instruction returns and the
caller must still halt; on hardware the write is simply ignored.

## linker.ld: image layout

`KERNEL_VMA = 0xFFFFFFFF80000000` (top 2 GiB, matching `-mcmodel=kernel`).
Four program headers: `text PT_LOAD R X`, `rodata PT_LOAD R`,
`data PT_LOAD RW`, `notes PT_NOTE`. Sections are placed in that order with
`ALIGN(4096)` between segments; `.note.cosmoboot` and
`.note.gnu.build-id` sit inside the rodata segment and are also listed in
`notes`. Symbols `__kernel_start/__kernel_end`, `__text_*`, `__rodata_*`,
`__data_*`, `__bss_*` bound each region and are declared in
`kernel/include/kernel/kernel.h`. `KEEP` protects `.text.entry` and the
boot note from `--gc-sections`. `.eh_frame`, `.comment`, `.note.GNU-stack`
and `.interp` are discarded.

## Phase 3 additions

**percpu.c.** `arch_percpu_install` writes `MSR_GS_BASE` (0xC0000101)
with the address of the CPU's `struct percpu`, whose first field points
to itself; `arch_percpu_get` is `mov %gs:0, %rax` and `arch_cpu_id`
reads `%gs:offsetof(struct percpu, cpu_id)`. Hazard: any `mov %ax, %gs`
replaces the GS base with the descriptor base (0), so `gdt_init` must
run before `percpu_init_boot` and GS is never reloaded afterwards
(`x86_start` orders console → GDT → per-CPU → IDT → PIC → CPU features,
and nothing before the per-CPU install may take a lock). There is still
no SWAPGS: with no user mode the kernel value stays in `GS_BASE`; when
user mode arrives it moves to `KERNEL_GS_BASE` and the trap entry swaps.

**switch.S / context.c.** `arch_context_switch(from, to)` pushes
rbp, rbx, r12–r15, stores rsp into `from->sp`, loads `to->sp`, pops, and
`ret`s. `arch_context_init` builds the first-run frame at a 16-byte
aligned `stack_top`: `top-8` entry, `top-16` `&x86_context_start`,
six zero callee-saved slots down to `top-64` = saved sp. After the six
pops and the `ret`, rsp = `top-8`; `x86_context_start` pops the entry
(rsp = top), pushes a zero return address (rsp = `top-8`, i.e. 8 mod 16)
and jumps, which is exactly the ABI state at a function entry after a
`call`. The zero return address terminates frame-pointer backtraces;
`arch_backtrace` now also accepts frames inside the current thread's
stack (`thread_stack_contains`).

**lapic.c.** xAPIC MMIO mode: the register page is mapped once with
`vm_map_phys(base, PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC)`; every access
goes through `lapic_read`/`lapic_write` so x2APIC (detected, not
enabled) is a two-function change. Init: ensure `MSR_APIC_BASE` enable
bit, TPR 0, all LVTs masked, ESR cleared, SVR = enable | vector 255,
timer divide 16. Timer: one-shot raw count for calibration, periodic
with a vector for the tick. ICR: fixed IPIs to a physical APIC id or
all-excluding-self, plus INIT and SIPI for the SMP work.

**ioapic.c.** Indirect `IOREGSEL`/`IOWIN` access, one entry per
discovered controller (max 8), entry count from the version register,
everything masked at `ioapic_add`. `ioapic_route` writes the 64-bit
redirection entry (vector, fixed delivery, physical destination in the
high dword, trigger/polarity from `ARCH_IRQ_*`, masked); mask/unmask
toggle bit 16; a spinlock serialises the register pair.

**irqc.c.** Implements `arch/irqc.h`: dynamic vectors 48–238 from a
bitmap under a spinlock, the CPU index → APIC id table
(`x86_cpu_apic_id`), routing to a CPU's APIC id, and
`arch_irqc_eoi` (nothing for exceptions and vector 255, PIC EOI for
32–47, LAPIC EOI otherwise).

**timer.c (arch).** `arch_timer_calibrate` gates PIT channel 2 for
11 932 ticks (10 ms), starts the LAPIC timer one-shot from
`0xFFFFFFFF`, samples `lfence; rdtsc` and the LAPIC current count before
and after the PIT output flips, and scales both deltas by 100. Bounds
are asserted (TSC 100 MHz–10 GHz, LAPIC 1 MHz–10 GHz), a bounded spin
protects against a dead PIT, and the tick vector is allocated here. The
tick is the LAPIC timer in periodic mode at `lapic_hz / hz`.

**pit.c.** `arch/testhooks.h` only: PIT channel 0 in rate-generator mode
as an ISA IRQ 0 source for the `irq-route` self-test, and a stop that
leaves it in one-shot mode with count 0.

**trap.c dispatch tail.** For vectors ≥ 32, `x86_trap_dispatch`
increments `percpu->irq_depth` and `irq_count`, calls
`interrupt_dispatch`, then `arch_irqc_eoi(vector)`, decrements
`irq_depth`, and, if `irq_depth == 0 && preempt_count == 0 &&
(frame->rflags & RFLAGS_IF)`, records a quiescent state
(`quiesce_note_quiescent`, `docs/kernel/quiesce/design.md`: the
interrupted context holds no spinlock and is not an interrupt) and then,
if `need_resched`, calls `sched_preempt()`. The context switch happens inside the handler on the
interrupted thread's stack; the `iretq` completes when the thread is
switched back in. Exceptions (vectors < 32) do not count as interrupt
nesting, so a page fault in a thread does not block a later preemption.

## Deliberately absent

- **MSI/MSI-X**: Phase 6 with PCI.
- **x2APIC mode**: detected by `cpu.c`, not enabled; xAPIC MMIO is used.
- **Vector state in the trap path**: the kernel is compiled with
  `-mgeneral-regs-only`, so an interrupt saves no x87/SSE/AVX state; the
  owning thread's registers stay live across it and are saved only at a
  thread switch (`fpu.c`).
- **Symbolised backtraces**: addresses are resolved offline against
  `out/<arch>-<build>/kernel/kernel.map`.
- **Speculative-execution mitigations** (IBRS, retpolines, PTI): none;
  the target is QEMU/TCG. To be revisited with real hardware.

## AArch64 mapping

| x86-64 | AArch64 equivalent |
|---|---|
| `entry.S` stack switch | same, `x0` carries the info pointer |
| IDT + 256 stubs | `VBAR_EL1` vector table, 16 entries, ESR_EL1 decoding into a vector space of the same size |
| PIC/APIC | GICv2/v3 (Phase 13) |
| `arch_irq_*` on RFLAGS.IF | `DAIF` I bit via `msr daifset/daifclr` |
| `arch_emulator_exit` on port 0xF4 | semihosting `SYS_EXIT` or PSCI `SYSTEM_OFF` |
| `-mcmodel=kernel` | `-mcmodel=small` with a higher-half `TTBR1` mapping |
| `hlt` / `pause` | `wfi` / `yield` |

The interface headers do not change; `arch/fpu.h` is implemented as
no-ops while FP/SIMD stays disabled at EL0 (`fpu.c`), and
`arch_test_paranoid_entry` reports nothing to test because TPIDR_EL1 is
never swapped and every exception from EL0 lands on SP_EL1.
