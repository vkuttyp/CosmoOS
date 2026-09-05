# Architecture Layer: Testing

## What is tested today

All tests run under QEMU (`q35`, TCG, `qemu64,+nx`, 256 MiB) via
`make test` and `make test-crash`. There are no host-side unit tests for
this layer yet: everything here either executes privileged instructions
or depends on the descriptor tables, so it must run on the target.

### Boot path (`make test`)

`tests/boot/run_boot_test.py` requires, in the serial log, the loader
banner, `jumping to kernel entry`, `CosmoOS kernel`, `Architecture:
x86_64`, `Boot: UEFI`, and `[ INFO] boot complete`, and requires QEMU to
exit with 33 (`ARCH_EMULATOR_EXIT_SUCCESS` via `arch_emulator_exit`).
Reaching `boot complete` proves `entry.S`, `start.c`, `gdt_init`,
`idt_init`, `pic_init_masked`, `x86_cpu_init`, the serial sink, and
`arch_irq_enable` all worked. The exit code proves `shutdown.c`.

### Self-tests (`CONFIG_SELFTEST=1`, default in debug builds)

These self-tests in `kernel/core/selftest.c` target this layer:

- **trap-paranoid**: `arch_test_paranoid_entry` (x86-64 `trap.c`)
  registers a probe on vector 2, raises a software NMI from kernel
  context and one with the user's GS base live (`swapgs; int $2; swapgs`
  with interrupts off), and checks each ran on the NMI IST stack of this
  CPU, saw the right per-CPU block and `irq_depth 1`, and that the GS
  base afterwards is the kernel's again. AArch64 reports nothing to test.
- **fpu-switch**: `arch_test_fpu_switch` (x86-64 `fpu.c`) runs two
  state-owning threads pinned to the calling CPU that load distinct
  xmm0-15 patterns and, across 400 yields each, verify their registers
  came back untouched.
- **smp-ipi-storm** (`smptest.c`): cross calls to every other CPU for
  300 ms while a timer on this CPU wakes threads pinned to those CPUs
  every tick, so wake IPIs are sent from interrupt context during the
  ICR write sequence of the cross calls; every call must complete.
- **irq-state**: nested `arch_irq_save`/`arch_irq_restore` pairs leave IF
  exactly as found; `arch_irq_enabled` reports false inside the pair.
- **breakpoint-trap**: registers a handler on
  `arch_trap_vector(ARCH_TRAP_BREAKPOINT)`, calls `arch_debug_break()`,
  and checks that the handler ran exactly once, saw vector 3, read a
  `rip` inside `[__text_start, __text_end)` via `arch_trap_frame_pc`, that
  IF is still set afterwards (IRETQ restored RFLAGS), and that the
  dispatch counter advanced. This drives the IDT gate, the 16-byte stub,
  `isr_common`, `x86_trap_dispatch`, `interrupt_dispatch`, and the return
  path end to end.

### Panic path (`make test-crash`)

Builds with `CRASH_TEST=1` into `out/<arch>-<build>-crash/`. `kernel_main`
writes to `0xFFFF900000000000`, a canonical address with no PML4 entry,
producing `#PF` with error code 2 (not-present, write, kernel). The
harness (`--expect-panic`) requires `KERNEL PANIC: unhandled exception 14
(#PF page fault)`, a `trap 14` line, an `RIP=` register line,
`CR2=ffff900000000000 (not-present write kernel)`, `stack trace:` with a
frame `#0` inside kernel text, `halting.`, and QEMU exit 35. This proves
the error-code stub variant, `arch_trap_unhandled`, `arch_trap_frame_dump`
including CR2 decoding, `arch_backtrace` from a trap frame, and the
failure exit code.

### Static checks (every build)

- `scripts/check-kernel-elf.sh`: every `PT_LOAD` is W^X; a `PT_NOTE`
  exists.
- `_Static_assert` on `struct arch_trap_frame` (176 bytes), `struct tss`
  (104), `struct idt_entry` (16).
- `make analyze`: clang static analyzer over every C file, including the
  x86-64 sources.

## How to run

```sh
make test                       # debug build, self-tests on
make BUILD=release test         # self-tests off, same markers
make test-crash                 # panic path
make run                        # interactive serial console
cat out/x86_64-debug/boot-test.log
```

`V=1` shows the exact compiler and linker invocations.
`QEMU_EXTRA="-d int"` logs every interrupt QEMU delivers, useful when a
vector arrives that the dispatcher does not expect.

## Reading a failure

- **Triple fault** (QEMU resets or exits without a panic line): the fault
  happened before `idt_init`, or inside the trap path itself. Run with
  `QEMU_EXTRA="-d int,cpu_reset -no-reboot"` and look at the last
  exception and its `RIP`.
- **`KERNEL PANIC: unhandled exception N`**: the register dump gives
  `RIP`; resolve it in `out/<arch>-<build>/kernel/kernel.map`.
- **Missing loader banner**: firmware did not find
  `\EFI\BOOT\BOOTX64.EFI` or the image is not FAT; see `docs/boot/`.
- **Doubled characters in the loader lines**: firmware mirrors ConOut to
  serial; the loader writes only ConOut until `ExitBootServices` for this
  reason. If it recurs, check `boot/uefi/console.c`.

## Gaps and planned tests

- Double-fault on IST1: a deliberate kernel stack overflow test.
- `#GP` on a non-canonical address and `#UD` on `ud2`, to cover the
  no-error-code and error-code paths of every exception class.
- Spurious IRQ 7/15 injection to exercise `pic_is_spurious`.
- Per-feature assertions that CR0/CR4/EFER contain the expected bits after
  `x86_cpu_init` (currently visible only in the debug log).
- Host-side unit tests for the pure C parts (`arch_trap_vector` mapping,
  CPUID family/model decode) once a host test harness exists.
- Fuzzing has no target here yet; the first fuzz target in the arch layer
  will be the trap-frame dump formatter fed with random frames.
