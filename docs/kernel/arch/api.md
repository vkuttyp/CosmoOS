# Architecture Interface: API

These are the functions every architecture implements and the only
architecture functions generic code may call. Headers live in
`kernel/include/arch/`. All are **internal kernel ABI**: they may change
between kernel versions without notice; nothing in userland or in kernel
modules (Phase 5) may depend on them until a module ABI freezes them.

Unless stated otherwise every function: never allocates, never sleeps,
never takes a lock, never performs I/O beyond the named device, and is
safe in interrupt and panic context.

---

## `arch/cpu.h`

### `const char *arch_name(void)`
- **Purpose**: architecture string for banners (`"x86_64"`).
- **Outputs**: immortal string; caller does not own it.

### `void arch_cpu_brand_string(char *buf, size_t len)`
- **Purpose**: human-readable CPU description.
- **Inputs**: `buf` caller-owned, `len` its size (≥ 1).
- **Outputs**: NUL-terminated, truncated to `len - 1`. Before
  `x86_cpu_init` returns `"(unidentified)"`.

### `unsigned arch_cpu_id(void)`
- **Purpose**: logical index of the calling CPU. Always 0 until SMP.

### `void arch_cpu_relax(void)`
- **Purpose**: spin-wait hint (`pause`).

### `void arch_cpu_wait_for_interrupt(void)`
- **Purpose**: sleep until the next interrupt.
- **Blocking**: blocks until an interrupt arrives. If called with
  interrupts disabled it enables them atomically with the halt and
  disables them again afterwards; the caller's IF state is preserved.
- **Interrupt context**: must not be called from a handler.

### `void arch_cpu_halt_forever(void) __noreturn`
- **Purpose**: `cli; hlt` loop. Used by panic and shutdown.

---

## `arch/irq.h`

### `arch_irq_state_t arch_irq_save(void)`
- **Purpose**: disable local interrupts, return the prior state.
- **Outputs**: opaque token (x86: RFLAGS). Pair with `arch_irq_restore`.
- **Concurrency**: local CPU only. Nestable.

### `void arch_irq_restore(arch_irq_state_t state)`
- **Purpose**: re-enable interrupts iff they were enabled at the matching
  `arch_irq_save`.
- **Failure modes**: passing a token from a different save pair is a bug
  and is not detected.

### `void arch_irq_enable(void)` / `void arch_irq_disable(void)`
- **Purpose**: unconditional `sti` / `cli`. Not nestable; for boot code
  that knows the current state.

### `bool arch_irq_enabled(void)`
- **Purpose**: read the local interrupt-enable flag.

---

## `arch/trap.h`

`struct arch_trap_frame` is opaque here. Its storage is the interrupted
stack and its lifetime is the handler invocation; never retain the
pointer.

### `unsigned arch_trap_vector_count(void)`
- **Outputs**: number of dispatchable vectors (x86-64: 256).
- **ABI**: generic code sizes nothing on this; `interrupt_init` checks it
  against `INTERRUPT_MAX_VECTORS`.

### `int arch_trap_vector(enum arch_trap_kind kind)`
- **Purpose**: symbolic → numeric vector mapping.
- **Inputs**: one of `ARCH_TRAP_BREAKPOINT`, `ARCH_TRAP_DEBUG`,
  `ARCH_TRAP_DIVIDE_ERROR`, `ARCH_TRAP_INVALID_OPCODE`,
  `ARCH_TRAP_GENERAL_PROTECTION`, `ARCH_TRAP_PAGE_FAULT`.
- **Outputs**: vector, or `-1` if the architecture has no such trap.

### `bool arch_trap_is_exception(unsigned vector)`
- **Outputs**: true for CPU-generated exceptions (x86-64: vector < 32).

### `const char *arch_trap_name(unsigned vector)`
- **Outputs**: immortal string, never NULL (`"#PF page fault"`,
  `"legacy IRQ"`, `"interrupt"`).

### `uintptr_t arch_trap_frame_pc/sp/fp(const struct arch_trap_frame *)`
- **Outputs**: interrupted program counter, stack pointer, frame pointer.

### `void arch_trap_frame_dump(const struct arch_trap_frame *frame)`
- **Purpose**: log full register state through `kprintf`.
- **Concurrency**: output may interleave with other CPUs' output once SMP
  exists; the console has no lock yet.

### `void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame)`
- **Purpose**: policy for a vector with no registered handler. Called only
  by `interrupt_dispatch`.
- **Failure modes**: for exceptions, does not return (panics). For other
  vectors, logs and returns.

### `void arch_debug_break(void)`
- **Purpose**: raise the breakpoint trap synchronously (`int3`).
- **Failure modes**: if no handler is registered on
  `arch_trap_vector(ARCH_TRAP_BREAKPOINT)`, the result is a panic.

---

## `arch/console.h`

### `void arch_console_early_init(void)`
- **Purpose**: probe the first output device and register it with
  `console_register`.
- **Lifetime**: first call in the kernel; depends on nothing. Safe to call
  once only (registers a static sink).
- **Failure modes**: none reported; a missing device means output is
  dropped.

---

## `arch/shutdown.h`

### `void arch_emulator_exit(unsigned code)`
- **Purpose**: ask a hosting emulator to terminate with `code`.
- **Inputs**: `ARCH_EMULATOR_EXIT_SUCCESS` (0x10) or
  `ARCH_EMULATOR_EXIT_FAILURE` (0x11). These values are a contract with
  `tests/boot/run_boot_test.py` (QEMU exit 33 / 35).
- **Blocking**: returns; the exit may complete asynchronously. Callers must
  halt afterwards.
- **Failure modes**: no-op on hardware.

---

## `arch/backtrace.h`

### `size_t arch_backtrace(uintptr_t *pcs, size_t max, const struct arch_trap_frame *from)`
- **Purpose**: collect return addresses, innermost first.
- **Inputs**: `pcs` caller-owned array of `max` entries; `from` NULL for
  the caller's own stack, or a trap frame whose `pc` becomes entry 0.
- **Outputs**: count written (≤ `max`). 0 when `max == 0`.
- **Failure modes**: a corrupted frame chain yields a short trace, never a
  fault; validation is described in `design.md`.

---

## `arch/fwcfg.h` (Phase 8)

### `int arch_fwcfg_read(const char *name, void *buf, size_t len)`
- Purpose: copy a firmware configuration item (QEMU fw_cfg on x86-64:
  I/O ports 0x510/0x511, the `FW_CFG_FILE_DIR` directory) into `buf`.
- Inputs: the item name (for example `opt/cosmo/nettest`), a buffer and
  its size.
- Outputs: the item's full size (which may exceed `len`; only `len`
  bytes were copied), `-ENOENT` when the item is absent, `-ENODEV`
  when no such device exists (the normal state on real hardware).
- Concurrency: a spinlock serialises the selector/data port sequence;
  any context, but every call walks the directory, so it is for boot
  configuration. Generic code uses `fwcfg_get_string` from
  `kernel/fwcfg.h` (`kernel/core/fwcfg.c`), which prefixes
  `opt/cosmo/` and NUL-terminates. See
  `docs/kernel-services/network/api.md`.

## Private x86-64 headers (not part of the interface)

`kernel/arch/x86_64/include/x86/`: `cpu.h` (`x86_start`, `x86_cpu_init`,
`x86_cpu_info`, CR/MSR/CPUID inlines, bit constants), `gdt.h`
(selectors, `gdt_init`, `gdt_set_kernel_stack`), `idt.h` (vector
constants, `idt_init`, `idt_load`, `x86_isr_stubs`), `io.h` (port I/O),
`pic.h` (`pic_init_masked`, `pic_eoi`, `pic_is_spurious`), `serial.h`
(`serial_init`, `serial_present`, `serial_putc`, `serial_write`; since
Phase 9 `serial.c` also implements `arch_console_input_init` from
`arch/console.h`: it requests ISA IRQ 4 through `irq_legacy_to_gsi` and
`irq_request`, enables the UART's receive interrupt, unmasks the line
with `irq_enable`, and its handler feeds every received byte to the
console tty with `tty_input`, `docs/kernel/tty/`),
`trapframe.h` (`struct arch_trap_frame` layout, `x86_trap_dispatch`).
These may change freely; only x86-64 code includes them.
