# Build system: invariants

These hold for every configuration. Breaking one requires changing this
file in the same commit and saying why.

## B1. Every target compile carries an explicit `--target=` triple

No compile of kernel or loader code relies on the compiler's default
target. `KERNEL_CFLAGS` and `LOADER_CFLAGS` each begin with
`--target=$(..._TARGET)` from `build/arch/<ARCH>.mk`. Constitution
section 4: never assume host properties while building target code.

## B2. No host header reaches target code

`-nostdinc` removes every system include path; the only one added back is
clang's resource directory (`stdint.h`, `stddef.h`, `stdbool.h`,
`stdarg.h`), whose contents depend on the `--target=` triple, not the
host. A `#include <string.h>` in kernel code must resolve to
`kernel/include/kernel/string.h` via `<kernel/string.h>` and nothing else.

## B3. Generic kernel objects cannot include architecture headers

Only objects listed in `KERNEL_ARCH_SRCS` receive
`-I$(ROOT)/kernel/arch/$(ARCH)/include`. Generic code sees
`kernel/include/arch/*.h` (the interface) and never `x86/*.h` (the
implementation). This is the mechanical form of constitution Invariant 1.

## B4. Warnings are errors, everywhere, always

`-Werror` is part of `COMMON_WARNINGS` and is never removed per file or
per component. A warning that cannot be fixed is fixed by changing the
code, not the flags.

## B5. Output never lands in the source tree

All artifacts go under `$(OUT)` (or a sibling `out/...` tree for
`test-crash` and `reproducible`). `.gitignore` excludes `out/`. A rule
that writes next to its source is a bug.

## B6. Identical sources produce identical binaries

`make reproducible` must pass. Mechanisms that keep it true:
`-ffile-prefix-map`, `-Wdate-time`, `SOURCE_DATE_EPOCH` exported,
`/Brepro /timestamp:0` for the PE link, `--build-id=sha1` for the ELF,
`BUILD_ID` derived from the tree state. Introducing a timestamp, random
seed, or absolute path into an output breaks this invariant.

## B7. The kernel ELF is W^X at the segment level and carries PT_NOTE

`scripts/check-kernel-elf.sh` runs at every kernel link and rejects a
PT_LOAD with both `w` and `x` flags or a missing PT_NOTE. The linker script
must keep `.text`, `.rodata`, and `.data`/`.bss` in three separate
segments and keep `.note.cosmoboot`.

## B8. The kernel never uses floating point or vector registers

`-mgeneral-regs-only` is mandatory in `KERNEL_ARCH_CFLAGS` for every
architecture. Trap entry saves only general registers on that basis
(`kernel/arch/x86_64/isr.S`).

## B9. Frame pointers are never omitted

`-fno-omit-frame-pointer` is in `COMMON_CFLAGS`. `arch_backtrace()`
depends on it; a frameless function silently truncates every stack trace
through it.

## B10. Test results do not depend on the accelerator

`QEMU_ACCEL=tcg` is the default and what CI runs. A test that passes
only under KVM/HVF is not a passing test.

## B11. `test` and `test-crash` disagree on exactly one thing

Both run the same harness and image pipeline; `test-crash` differs only
in `CRASH_TEST=1`, a separate `OUT`, and `--expect-panic`. The harness
must therefore keep the boot markers shared and only the verdict markers
different, so a regression in the shared path is caught by both.

## B12. Adding a source means editing one list

Sources are enumerated explicitly in `KERNEL_GENERIC_SRCS`,
`KERNEL_ARCH_SRCS`, or `LOADER_SRCS`. There is no wildcard globbing, so a
stray file cannot join the build unnoticed and a deleted file fails
loudly.
