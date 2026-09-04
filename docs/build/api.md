# Build system: API

The build system's public interface is make targets, make variables, and
the scripts they invoke. Per constitution section 52 each entry states
purpose, inputs, outputs, and failure modes; ownership/lifetime,
concurrency, blocking, and interrupt-context columns do not apply to
host-side tooling and are omitted. ABI stability: targets and variables
listed here are stable; renaming one requires updating `docs/` and
`.github/workflows/ci.yml` in the same change.

## Targets

### `make all` (default)

Purpose: build `kernel.elf` and `BOOTX64.EFI`.
Inputs: `ARCH`, `BUILD`, `OUT`, `LLVM_PREFIX`, `SELFTEST`, `CRASH_TEST`.
Outputs: `$(OUT)/kernel/kernel.elf`, `$(OUT)/kernel/kernel.map`,
`$(OUT)/boot/BOOTX64.EFI`.
Failure: any warning (they are errors), link error, or
`check-kernel-elf.sh` rejection.

### `make kernel`, `make boot`

Purpose: build one component. Same inputs and failures as `all`.

### `make image`

Purpose: assemble the boot medium.
Inputs: the two binaries; `mformat`, `mmd`, `mcopy` on the PATH.
Outputs: `$(OUT)/cosmoos.img`, a 64 MiB FAT32 image containing
`/EFI/BOOT/BOOTX64.EFI` and `/cosmo/kernel.elf`.
Failure: mtools missing (`check-tools` reports it) or a write error;
the image is built as `cosmoos.img.tmp` and renamed only on success.

### `make run`

Purpose: boot interactively with serial on the terminal.
Inputs: `QEMU_MEM`, `QEMU_ACCEL`, `QEMU_EXTRA`, `OVMF_CODE`.
Outputs: none on disk. Exit status is QEMU's (33 after a clean kernel
shutdown).
Failure: no firmware found (`find-firmware.sh` prints where it looked).

### `make test`

Purpose: automated boot verification.
Inputs: as `run`.
Outputs: `$(OUT)/boot-test.log`; exit 0 on PASS, 1 on FAIL with reasons
and the full log printed.
Failure criteria: see `testing.md`.

### `make test-crash`

Purpose: verify the panic path and the harness's failure detection.
Inputs: as `test`; recursively builds with `CRASH_TEST=1` into
`$(OUT)-crash`.
Outputs: `$(OUT)-crash/cosmoos.img`, `$(OUT)-crash/boot-test-crash.log`;
exit 0 when the kernel panicked exactly as expected (page fault at
`0xFFFF900000000000`, full register dump, stack trace, exit value 0x11).
Failure: the kernel did not fault, the report was incomplete, or the
exit status was wrong.

### `make analyze`

Purpose: run `clang --analyze` on every kernel and loader source with the
same flags as compilation.
Outputs: `*.analyzed` stamp files next to the objects; prints
`static analysis: clean`.
Failure: any analyzer diagnostic (they are reported as text and the rule
fails).

### `make reproducible`

Purpose: prove determinism.
Inputs: `ARCH`, `BUILD`.
Outputs: `out/repro-a/`, `out/repro-b/`; prints `same`/`DIFFERS` per
artifact and `reproducible: yes|NO`.
Failure: any byte difference in `kernel.elf` or `BOOTX64.EFI`.

### `make check-tools`

Purpose: report the state of prerequisites.
Outputs: one line per tool, firmware path, and a compile probe for each
target triple. Exit 1 if anything is missing.

### `make clean`

Purpose: delete `$(OUT)`. Does not touch other output trees
(`out/repro-*`, `$(OUT)-crash`); delete `out/` by hand for a full reset.

### `make help`

Purpose: print the target table and the effective `ARCH`, `BUILD`,
`OUT`, host, and tool names.

## Variables

| Variable | Type | Default | Notes |
|---|---|---|---|
| `ARCH` | string | `x86_64` | must have `build/arch/$(ARCH).mk` and `kernel/arch/$(ARCH)/arch.mk` |
| `BUILD` | `debug`\|`release` | `debug` | validated; anything else is `$(error)` |
| `OUT` | path | `$(ROOT)/out/$(ARCH)-$(BUILD)` | absolute or relative to the make invocation directory |
| `V` | `0`\|`1` | `0` | `1` echoes full commands |
| `SELFTEST` | `0`\|`1` | `1` for debug, `0` for release | becomes `CONFIG_SELFTEST` |
| `CRASH_TEST` | `0`\|`1` | `0` | becomes `CONFIG_CRASH_TEST`; only `test-crash` should set it |
| `LLVM_PREFIX` | path with trailing `/` | empty | prefix for every LLVM tool name |
| `PYTHON` | command | `python3` | interpreter for the test harness |
| `QEMU_MEM` | QEMU size | `256M` | guest RAM |
| `QEMU_ACCEL` | `tcg`\|`kvm`\|`hvf` | `tcg` | `tcg` is what CI uses; results must not depend on it |
| `QEMU_EXTRA` | string | empty | appended to the QEMU command line; e.g. `-d int,cpu_reset` |
| `OVMF_CODE` | path | auto-detected | firmware image; bypasses `find-firmware.sh` |
| `SOURCE_DATE_EPOCH` | integer | `0` | exported; the compiler uses it for any embedded time |

Derived, read-only: `BUILD_ID` (`git describe --always --dirty`, or
`unknown` outside git), `HOST_OS`, `HOST_ARCH`, `CLANG_RESOURCE_INC`.

## Scripts

All scripts are POSIX `sh` except the Python harness, use `set -eu`, and
are callable outside make.

| Script | Arguments | Purpose | Exit status |
|---|---|---|---|
| `scripts/mkimage.sh` | `OUT.img BOOTX64.EFI kernel.elf` | build the FAT32 image with mtools | non-zero on any mtools error |
| `scripts/find-firmware.sh` | `[x86_64]` | print an OVMF/EDK2 code image path, honouring `OVMF_CODE` | 1 if none found |
| `scripts/qemu-run.sh` | `IMAGE` | `exec` QEMU with the standard machine description | QEMU's status |
| `scripts/check-tools.sh` | `CC LD LDLINK OBJCOPY PYTHON` | prerequisite report | 1 if anything missing |
| `scripts/check-kernel-elf.sh` | `OBJDUMP kernel.elf` | W^X and PT_NOTE checks on program headers | 1 on violation |
| `scripts/check-reproducible.sh` | `[ARCH] [BUILD]` | two builds, byte comparison | 1 on difference |
| `scripts/setup-dev-linux.sh` | none | apt install of prerequisites (Debian/Ubuntu) | apt's status |
| `scripts/setup-dev-macos.sh` | none | brew install of prerequisites | brew's status |
| `tests/boot/run_boot_test.py` | `--image --log [--timeout] [--expect-selftest auto\|yes\|no] [--expect-panic]` | boot under QEMU and judge the outcome | 0 PASS, 1 FAIL |

## Component makefile contract

A component fragment must:

1. List sources relative to `$(ROOT)` in a variable.
2. Convert them with `$(call objs_of,$(SRCS))`.
3. Register rules with `$(eval $(call compile_rules,$(OBJS),<CFLAGS var>))`
   and, for assembly, `assemble_rules`.
4. Define its link rule using `$(call log,...)` and `$(Q)`.
5. `-include $(OBJS:.o=.d)`.

`kernel/kernel.mk` and `boot/uefi/boot.mk` are the two existing examples.
