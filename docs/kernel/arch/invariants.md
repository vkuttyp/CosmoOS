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
it found. That test is sound only while user mode cannot choose a GS
base: `x86_cpu_enable_features` clears and asserts `CR4.FSGSBASE` off
and the Linux personality refuses `ARCH_SET_GS`; enabling either requires
a different paranoid rule first. `x86_trap_paranoid` neither preempts
nor delivers a kill, and a paranoid handler must not fault or single-step
(the IST stacks are not re-entrant).
**Checked by** `idt_init`, `gdt_init`, and the `trap-paranoid` self-test
(a software NMI from kernel context and one with the user's GS base
live); a stack-overflow crash test is future work.

## I-ARCH-8: `arch_irq_save`/`arch_irq_restore` compose

Restoring a token from an outer save after an inner pair must leave IF as
it was at the outer save. **Checked by** the `irq-state` self-test.

## I-ARCH-9: Protection features are asserted by the kernel, not assumed from the loader

`x86_cpu_init` sets WP, NXE (if NX), PGE, SMEP, SMAP, UMIP as supported
regardless of prior state; AArch64 sets SPAN and PAN where the core has
PAN, and `SCTLR_EL1.WXN` on every CPU once the kernel's own tables are
active (the loader clears it, because its tables map RAM writable and
executable). **Checked by** the `hardening:` line every boot prints from
`CR4` / `SCTLR_EL1` as they are (`[ INFO] hardening: x86-64: nx smep
smap umip`, `[ INFO] hardening: aarch64: pan wxn`; a `[ WARN]
hardening: absent: …` names what is off), which the guard boot (`make
test-guard`, a CPU model that has every feature) requires whole and
whose `absent` form it forbids. The default CPU models lack SMEP/SMAP/
UMIP and PAN, so their boots carry the `WARN`: the guard is proved
where it exists and its absence handled where it does not
(`docs/kernel/arch/testing.md`, "The guard boot").

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

`-mgeneral-regs-only` is on for the kernel target and the loader and
modules, and **not** on the libc or user programs since the FP/SIMD
unit; the trap path saves no vector state. Since the compiler flag no
longer covers everything that links against the kernel, the rule is
checked on the built image (`scripts/check-fpregs.sh`, in `make
analyze`): no vector register outside the state save and restore, the
guest swap and the self-test hooks. A thread executes x87/SSE/AVX/NEON
instructions only if `thread->fpu` is set
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
therefore never call `ipi_send` or anything that does. The lockup
sample's answer on the NMI path (`lockup_answer`, called by
`x86_trap_paranoid`) records into its own CPU's buffer and sends
nothing; the sample *request* (`arch_ipi_send_nmi`) is sent from the
tick or a thread, never from an NMI. **Checked by** `smp-ipi-storm` and
review of the (registered) NMI handlers and of `lockup_answer`.

## I-ARCH-14: `arch_emulator_exit` may return

Callers must halt after calling it. **Checked by** `kernel_shutdown` and
`panic_common`, which both fall through to `arch_cpu_halt_forever`.

## I-ARCH-16: An asynchronous hardware error lets the machine continue only when the hardware corrected it, and is never blamed on a process

An SError (AArch64) and a machine check (x86-64) are the same class: the
CPU reporting a fault it could not attribute synchronously. Both are
dispatched as `ARCH_TRAP_ASYNC_ERROR`, and `arch_async_error_class`
answers **corrected** only for a syndrome that positively says so:

- **AArch64** — `ID_AA64PFR0_EL1.RAS` non-zero, `ESR_EL1.IDS` clear, and
  `AET = CE`. Without FEAT_RAS there is no `AET` to have read, so every
  SError is uncontained; `IDS` means the syndrome is implementation
  defined, so the same; every reserved `AET` encoding falls to the
  `default` arm.
- **x86-64** — **at least one bank with `VAL`**, every valid bank
  `UC == 0`, no bank with `PCC` or `OVER`, and `MCG_STATUS.RIPV`, across
  all `MCG_CAP.Count` banks. The first clause is not redundant: without
  it "every valid bank is clean" is true of *no banks*, and a machine
  check carrying no record would read as corrected.

Everything else panics, at either exception level, naming the class and
printing the syndrome — where before this rule every SError reached
`aarch64_trap_entry`'s `default` arm, was relabelled
`ARCH_TRAP_GENERAL_PROTECTION`, and stopped the machine even when the
hardware had already fixed the fault.

**No process is killed.** An asynchronous abort's frame names the context
that was *interrupted when the error was delivered*, not the one that
caused it, so nothing here is entitled to choose a victim: a deferred or
imprecise error lands on whoever is running. Attribution needs the RAS
error records (`ERR<n>_ADDR`), which this rule does not read, and the
unit that reads them is the one that may kill.

The corrected path **counts and does not print**: it runs in whatever
context the error interrupted, which may hold a run-queue lock.

**Checked by** `trap-async-class` — ten SError encodings and nine
machine-check bank combinations, one row per rule, including the reserved
`AET`s, a CPU without FEAT_RAS and the empty bank set that a vacuous rule
calls corrected — and `trap-async-inject`, which delivers a real
corrected SError through `HCR_EL2.VSE` and checks execution continued.

**Gap, and it is not small.** This kernel runs EL1 with `PSTATE.A` set
from its first instruction (`entry.S`: `msr daifset, #0xF`; the only
unmask anywhere is `daifclr, #2`, which is IRQ). So an asynchronous abort
is **not taken while the kernel runs** — it stays pending until something
unmasks it. EL0 does not have that property: user mode is entered with
`SPSR = 0`, DAIF clear, so an SError there is taken immediately, and that
is the path this rule governs today. Whether EL1 should unmask `A` is a
separate decision with its own risk — the kernel would then take an abort
at any instruction — and is not taken here. `trap-async-inject` opens the
mask deliberately and briefly to deliver its one error, which is why it
can observe anything at all.
