# Architecture Abstraction and x86-64 Implementation

## Purpose

The architecture layer is the only part of the kernel that knows what CPU
it runs on. It exposes a small set of C interfaces in
`kernel/include/arch/` that the generic kernel programs against, and
implements them under `kernel/arch/<arch>/`. Constitution Invariant 1
("generic kernel code must not depend on a specific CPU architecture") is
realised here, and Invariant 10 ("architecture-specific assembly must
remain isolated") is realised by keeping every assembly file under
`kernel/arch/`.

Two implementations exist: x86-64 (`kernel/arch/x86_64/`, described in
this directory) and, since Phase 13, AArch64 (`kernel/arch/aarch64/`,
described in `aarch64/`: architecture, design, api, invariants,
testing). The headers below are the whole contract between them and the
generic kernel.

## Responsibilities

- Receive control from the bootloader and reach C on a kernel-owned stack
  (`entry.S`).
- Bring up the CPU into a state where the generic kernel can run: descriptor
  tables, exception vectors, legacy interrupt controller quiescence, CPU
  identification, and protection features (`start.c`, `gdt.c`, `idt.c`,
  `pic.c`, `cpu.c`).
- Provide the early console device (`serial.c`).
- Save and restore register state on every trap and hand each one to the
  generic dispatcher with an opaque frame (`isr.S`, `trap.c`).
- Provide local interrupt enable/disable, halt, spin hints, backtraces, and
  emulator exit (`cpu.c`, `backtrace.c`, `shutdown.c`).
- Define the kernel image layout (`linker.ld`).
- Since Phase 12, the hardware virtualization backend behind
  `arch/hv.h` (`svm.c`: AMD-V enable, VMCB, intercepts, exit decoding,
  state; `svm_npt.c`: nested page tables; `svm_run.S`: the VMRUN
  sequence). The generic VM manager is `kernel-services/virtualization/`
  (`docs/kernel-services/virtualization/`); a VT-x backend would sit
  beside `svm.c` behind the same header.

## Non-responsibilities

- Deciding what an interrupt means. The vector-to-handler table lives in
  `kernel/interrupt/` (see `docs/kernel/interrupt/`).
- Memory management. The bootstrap page tables are built by the loader and
  merely inherited; the VMM in Phase 2 replaces them.
- Any policy: scheduling, security, devices. The layer is pure mechanism.

## The interface surface

Generic code may include only these headers:

| Header | Provides |
|---|---|
| `arch/cpu.h` | `arch_name`, `arch_cpu_brand_string`, `arch_cpu_id`, `arch_cpu_relax`, `arch_cpu_wait_for_interrupt`, `arch_cpu_halt_forever`, `arch_dma_barrier` (Phase 13) |
| `arch/irq.h` | `arch_irq_save/restore/enable/disable/enabled`, `arch_irq_state_t` |
| `arch/trap.h` | opaque `struct arch_trap_frame`, `enum arch_trap_kind`, `arch_trap_vector`, `arch_trap_vector_count`, `arch_trap_is_exception`, `arch_trap_name`, frame accessors, `arch_trap_frame_dump`, `arch_trap_unhandled`, `arch_debug_break` |
| `arch/console.h` | `arch_console_early_init` |
| `arch/shutdown.h` | `arch_emulator_exit`, exit code constants |
| `arch/backtrace.h` | `arch_backtrace` |

Everything else the x86-64 code needs lives in
`kernel/arch/x86_64/include/x86/` (`cpu.h`, `gdt.h`, `idt.h`, `io.h`,
`pic.h`, `serial.h`, `trapframe.h`). Those headers are private.

### Build enforcement

`kernel/kernel.mk` compiles generic objects with `-Ikernel/include
-Iboot/protocol` only. Architecture objects additionally receive
`-Ikernel/arch/$(ARCH)/include` through a target-specific `EXTRA_CFLAGS`.
A generic file that writes `#include <x86/cpu.h>` fails to compile. The
boundary is therefore checked on every build, not by review.

## Where the x86-64 implementation sits

```
bootloader (cosmoboot protocol)
        │  RDI = info, CR3 = bootstrap tables, IF = 0
        ▼
entry.S  _start ── switch to x86_boot_stack ── call x86_start
        ▼
start.c  serial → GDT/TSS → IDT → PIC mask → CPUID/CR0/CR4/EFER
        ▼
kernel_main()  (generic, kernel/core/main.c)
        │
        ├── klog/kprintf ──► console sink ──► serial.c
        ├── interrupt_init() ──► arch_trap_vector_count()
        ├── arch_irq_enable()
        └── selftest: arch_debug_break() ──► int3 ──► isr.S stub 3
                       ──► isr_common ──► x86_trap_dispatch (trap.c)
                       ──► interrupt_dispatch (generic) ──► handler
```

## Interfaces summary (see `api.md` for contracts)

The generic kernel calls into the arch layer; the arch layer calls back
into the generic kernel at exactly two points: `kernel_main()` from
`x86_start()`, and `interrupt_dispatch()` from `x86_trap_dispatch()`. The
arch layer also calls `console_register()`, `panic_frame()`, `kprintf()`
and the `klog` family, all of which are generic utilities with no
architecture dependency.

## Data structures

- `struct arch_trap_frame` (`x86/trapframe.h`): 22 × 8 bytes, lives on the
  interrupted stack for the duration of one trap. Owned by `isr_common`;
  handlers may read it and may modify register slots (this is how a future
  signal delivery or debugger will work), but must not retain the pointer
  after returning.
- GDT, TSS, double-fault stack, IDT (`gdt.c`, `idt.c`): static, kernel
  lifetime, written once at boot, read by hardware.
- `struct x86_cpu_info` (`cpu.c`): static, written once by `x86_cpu_init`,
  read-only afterwards.
- Boot stack: 64 KiB in `.bss.boot_stack`, kernel lifetime, used by CPU 0
  until threads exist.

## Concurrency model

Single CPU. Nothing here takes a lock. Every function that touches the
static tables is called once on the boot CPU before interrupts are
enabled. The interrupt enable/disable helpers act on the local CPU only.
The SMP work in Phase 3 will make the TSS, IST stacks, and
`arch_cpu_id()` per-CPU; the GDT and IDT can stay shared.

## Memory ownership

No dynamic allocation. Every object is static or on the boot stack. The
arch layer never frees anything and never retains pointers into
loader-provided memory beyond the `info` pointer passed through to
`kernel_main`.

## Error handling

- Hardware faults before the IDT is loaded triple-fault; that window is
  four function calls long and starts after the console is up.
- After `idt_init`, every vector reaches `interrupt_dispatch`; unhandled
  exceptions become a panic with a register dump (`arch_trap_unhandled`).
- The serial probe failing is not an error: output is dropped silently and
  the kernel continues.

## Performance considerations

None yet, deliberately. The trap path saves all fifteen general registers
on every entry; when a bottleneck is measured, a lighter path for
known-safe vectors can be added without changing the frame layout seen by
handlers.

## Security considerations

- W^X for the kernel image is enforced by the loader from the linker
  script's segment flags, and `scripts/check-kernel-elf.sh` refuses to link
  a segment that is both writable and executable.
- `x86_cpu_init` sets CR0.WP, EFER.NXE, CR4.SMEP, CR4.SMAP and CR4.UMIP
  when the CPU supports them, independent of what the loader did.
- The `#BP` gate is DPL 3 by design so that a future user-mode debugger can
  use `int3`; every other gate is DPL 0.
- `#DF` runs on its own 8 KiB IST stack so a kernel stack overflow produces
  a report instead of a triple fault.

## Testing strategy

See `testing.md`. The breakpoint self-test drives a real exception through
the IDT, stub, common path, and dispatcher; the crash test drives a real
page fault through the panic path.

## Future extensibility

- **AArch64** (done, Phase 13): implements every `arch/*.h` header under
  `kernel/arch/aarch64/`: exception vector table instead of IDT, GICv2
  plus GICv2m instead of PIC/APIC/IOAPIC/MSI, DAIF bits for `arch_irq_*`,
  EL1 as the kernel level, TTBR0/TTBR1 stage-1 tables, PSCI for
  secondary CPUs, semihosting for `arch_emulator_exit`. The generic
  kernel gained two interface functions (`arch_dma_barrier`,
  `arch_mmu_near_arena`) and the boot protocol a second table root
  (v4); see `aarch64/architecture.md`.
- **User mode (Phase 4)**: `isr_common` gains `SWAPGS` conditioned on the
  saved CS, the TSS gets `rsp0` set via `gdt_set_kernel_stack`, and the
  selector layout already suits `SYSRET`.
- **SMP (Phase 3)**: LAPIC/IOAPIC replace the masked PIC; `arch_cpu_id`
  reads the LAPIC ID; per-CPU TSS and IST stacks.
