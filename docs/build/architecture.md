# Build system: architecture

## Purpose

Turn the source tree into a kernel ELF, a UEFI loader, and a bootable disk
image from any host, deterministically, and drive the tests that prove
those artifacts work. The build system is the only place where host and
target meet, so it is also where their separation is enforced.

## Where it sits

```
developer / CI
     │  make <target> [ARCH=..] [BUILD=..]
     ▼
Makefile ─── build/config.mk      knobs: ARCH, BUILD, OUT, V, SELFTEST, CRASH_TEST, QEMU_*
         ├── build/toolchain.mk   tool names, per-target CFLAGS/LDFLAGS
         │      └── build/arch/x86_64.mk   triples and code-model flags
         ├── build/rules.mk       compile/assemble/analyze pattern rules
         ├── kernel/kernel.mk     kernel objects, link, ELF check
         │      └── kernel/arch/x86_64/arch.mk   arch sources, linker script
         └── boot/uefi/boot.mk    loader objects, PE link
     │
     ▼
out/<ARCH>-<BUILD>/
   kernel/kernel.elf, kernel.map, kernel/**/*.o, *.d, *.analyzed
   boot/BOOTX64.EFI, boot/uefi/*.o
   cosmoos.img, boot-test.log
```

Scripts under `scripts/` do the non-make work: image assembly, firmware
discovery, QEMU invocation, post-link checks, reproducibility comparison,
and host setup. `tests/boot/run_boot_test.py` is the harness `make test`
runs.

## Responsibilities

- Select one LLVM toolchain (`clang`, `ld.lld`, `lld-link`, `llvm-objcopy`,
  `llvm-objdump`) and pass explicit `--target=` triples to every compile.
- Give each component the flags its execution environment needs (kernel
  code model, loader ABI) and the warnings the constitution requires,
  treated as errors.
- Mirror the source tree under `$(OUT)` so the source tree stays clean and
  several configurations can coexist (`out/x86_64-debug`,
  `out/x86_64-release`, `out/x86_64-debug-crash`, `out/repro-a`).
- Enforce Invariant 1 mechanically: generic kernel objects never receive
  the architecture-private include directory.
- Inject build identity (`COSMO_BUILD_ID`, `COSMO_BUILD_TYPE`) and
  configuration (`CONFIG_DEBUG`, `CONFIG_SELFTEST`, `CONFIG_CRASH_TEST`) as
  preprocessor definitions so the binary carries its provenance.
- Produce byte-identical output for identical input (`make reproducible`).
- Drive verification: `check-tools`, link-time ELF check, `test`,
  `test-crash`, `analyze`, `reproducible`, and the CI workflow that runs
  them.

## Non-responsibilities

- Installing tools. `scripts/setup-dev-*.sh` do that; `check-tools` only
  reports.
- Building host-side tools, userland, libc, or packages. Those components
  do not exist yet; when they do they get their own component makefiles
  with their own (host or target) toolchain variables, not a change to the
  kernel's.
- Configuration management beyond a handful of variables. There is no
  Kconfig; `CONFIG_*` macros come from `build/config.mk` and are few by
  design until a real need appears.
- Choosing what runs under QEMU. `scripts/qemu-run.sh` owns the machine
  description.

## Interfaces

The build system's public interface is the set of make targets and
variables documented in `api.md`, plus three files a component must
provide to join the build:

| Provided by a component | Consumed by |
|---|---|
| a `*.mk` fragment listing sources relative to `$(ROOT)` | `Makefile` via `include` |
| a call to `$(call compile_rules,<objs>,<CFLAGS var>)` | `build/rules.mk` |
| for architectures: `build/arch/<ARCH>.mk` and `kernel/arch/<ARCH>/arch.mk` | `build/toolchain.mk`, `kernel/kernel.mk` |

Everything else (output directories, dependency files, quiet output,
analyzer stamps) is handled by the shared rules.

## Why LLVM only

- `clang` is a native cross compiler: one binary targets both triples,
  so no per-target GCC/binutils build on the ARM64 VM.
- `ld.lld` links the kernel ELF and `lld-link` links the loader's
  PE/COFF image; no gnu-efi, no ELF-to-PE conversion, no relocation
  trampoline in the loader.
- Both are deterministic by default, which makes reproducible builds a
  matter of not leaking paths and dates rather than fighting the linker.
- The same toolchain is available from apt, Homebrew, and the Swift
  distribution, so the primary and secondary hosts build identically.

The cost is a hard dependency on clang-specific flags
(`-mgeneral-regs-only`, `--target=`, `-print-resource-dir`,
`-ffile-prefix-map`) which is accepted and recorded in
`kernel/include/kernel/compiler.h`.
