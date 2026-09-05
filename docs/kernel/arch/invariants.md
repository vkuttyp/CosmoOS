# Architecture Layer: Invariants

Violating any of these requires revising this document and the code
together. Each entry names how it is checked today.

## I-ARCH-1: Generic code includes only `kernel/include/arch/*.h`

No file outside `kernel/arch/` may include a header from
`kernel/arch/<arch>/include/`. **Checked by the build**: the private
include directory is added only for architecture objects
(`kernel/kernel.mk`, `EXTRA_CFLAGS`).

## I-ARCH-2: Every architecture implements every function in the six interface headers

A missing implementation is a link error, not a runtime surprise.
**Checked by the linker** (no weak defaults exist).

## I-ARCH-3: Assembly lives only under `kernel/arch/`

Inline assembly in C is permitted only in files under
`kernel/arch/<arch>/` and their private headers. Generic code has zero
`__asm__` statements. **Checked by review**; a grep of `__asm__` outside
`kernel/arch/` must return nothing.

## I-ARCH-4: `struct arch_trap_frame` matches the `isr_common` push order

`x86/trapframe.h` and `isr.S` describe the same 22-word layout.
**Checked by** `_Static_assert(sizeof == 22 * 8)` and by the
`breakpoint-trap` self-test reading `rip` from a live frame and
confirming it lies in kernel text.

## I-ARCH-5: The kernel image is W^X and page-separated

Three `PT_LOAD` segments with flags `R X`, `R`, `RW`, each starting on a
4 KiB boundary. **Checked by** `scripts/check-kernel-elf.sh` after every
link, and independently by the loader (`boot/uefi/elf.c` refuses `W+X`
segments and page-level overlaps).

## I-ARCH-6: All 256 IDT vectors are populated before interrupts are enabled

There is no "unhandled at CPU level" vector; the meaning of an unexpected
one is decided by `arch_trap_unhandled`. **Checked by** `idt_init`
looping over `IDT_VECTORS` and by `start.c` ordering `idt_init` before
`kernel_main` (which is where `arch_irq_enable` is first called).

## I-ARCH-7: `#DF`, NMI, `#MC` and `#DB` run on their own IST stacks and take the paranoid entry

The gates for vectors 8, 2, 18 and 1 reference `IST_DOUBLE_FAULT`,
`IST_NMI`, `IST_MACHINE_CHECK` and `IST_DEBUG`; each CPU's TSS holds
four 8 KiB dedicated stacks. Their stubs jump to `isr_paranoid`, which
decides SWAPGS from `MSR_GS_BASE` (a kernel address means GS is already
the kernel's), never from the saved CS, and restores exactly the GS state
it found. `x86_trap_paranoid` neither preempts nor delivers a kill.
**Checked by** `idt_init`, `gdt_init`, and the `trap-paranoid` self-test
(a software NMI from kernel context and one with the user's GS base
live); a stack-overflow crash test is future work.

## I-ARCH-8: `arch_irq_save`/`arch_irq_restore` compose

Restoring a token from an outer save after an inner pair must leave IF as
it was at the outer save. **Checked by** the `irq-state` self-test.

## I-ARCH-9: Protection features are asserted by the kernel, not assumed from the loader

`x86_cpu_init` sets WP, NXE (if NX), PGE, SMEP, SMAP, UMIP as supported
regardless of prior state. **Checked by** the `[DEBUG] x86: cr0=… cr4=…
efer=…` line in every boot log; automated assertion is future work.

## I-ARCH-10: Frame-pointer walks never fault

`-fno-omit-frame-pointer` is in `COMMON_CFLAGS` for every target and
`arch_backtrace` validates each frame (non-null, 16-aligned, within the
kernel image, strictly increasing) before dereferencing it. **Checked by**
the crash test, which prints a trace from a page-fault frame.

## I-ARCH-11: The legacy PIC is remapped and masked before IF is ever set

Vectors 32-47 are reserved for it so a spurious IRQ can never alias an
exception. **Checked by** `start.c` ordering.

## I-ARCH-12: Selector layout is `0x08/0x10/0x18/0x20/0x28`

Changing it breaks the future `SYSRET` STAR assumption and every
`GDT_*` user. **Checked by** the constants in `x86/gdt.h`; any change must
update `design.md`.

## I-ARCH-13: Kernel code uses no FPU/SIMD registers; every thread that does owns its state

`-mgeneral-regs-only` is on for the kernel target (and the loader,
modules and native libc); the trap path saves no vector state. A thread
executes x87/SSE/AVX instructions only if `thread->fpu` is set
(`arch_fpu_alloc`: every user thread before its first instruction; test
threads explicitly), and `arch_thread_switch_prepare` saves the outgoing
owner's registers and loads the incoming owner's on every switch between
owners (eager). A guest runs from its own area with the owner saved
around it and its own XCR0 installed. CR0/CR4/XCR0 are asserted
identically on every CPU by `x86_fpu_init_cpu`. **Checked by** the
compiler, `fpu-switch`, `hv-guest-fpu`, `init --selftest` (`--fpu-partner`).

## I-ARCH-15: The ICR write pair is atomic against local interrupts, and NMI handlers send no IPIs

`icr_write_pair` writes ICR_HI and ICR_LO with interrupts disabled;
nothing else writes the ICR. An NMI handler cannot be masked and must
therefore never call `ipi_send` or anything that does. **Checked by**
`smp-ipi-storm` and review of the (registered) NMI handlers.

## I-ARCH-14: `arch_emulator_exit` may return

Callers must halt after calling it. **Checked by** `kernel_shutdown` and
`panic_common`, which both fall through to `arch_cpu_halt_forever`.
