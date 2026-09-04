# Build system: testing

The build system tests two things: that it can produce correct artifacts
on a fresh host, and that the artifacts behave. Everything below runs
locally and in CI with the same commands.

## Prerequisite check

```sh
make check-tools
```

`scripts/check-tools.sh` verifies, in one pass, the LLVM tools
(`clang`, `ld.lld`, `lld-link`, `llvm-objcopy`), the image tools
(`mformat`, `mcopy`), `qemu-system-x86_64`, `python3`, a UEFI firmware
image (`find-firmware.sh`), and that the compiler can actually produce an
object for `x86_64-unknown-none-elf` and `x86_64-unknown-windows`. Every
missing item is listed; the script exits 1 if any is missing.

## Link-time checks

`scripts/check-kernel-elf.sh` is part of the `kernel.elf` rule. It parses
`llvm-objdump -p` output and fails the link if any PT_LOAD is both
writable and executable or if there is no PT_NOTE. It is the guard for
`docs/build/invariants.md` B7 and catches linker-script edits before a
QEMU run would reveal them as a mysterious loader rejection.

## Static analysis

```sh
make analyze
```

Runs `clang --analyze -Xanalyzer -analyzer-output=text` on every kernel
and loader source with exactly the flags used for compilation (same
target, same defines, same include paths). Reports are printed and fail
the rule. Stamp files (`*.analyzed`) make it incremental. Current result:
clean.

## Reproducibility

```sh
make reproducible            # or scripts/check-reproducible.sh x86_64 debug
```

Builds the tree twice into `out/repro-a` and `out/repro-b` and compares
`kernel/kernel.elf` and `boot/BOOTX64.EFI` with `cmp`. Current result:
identical. If this fails, the diff usually points at a new absolute path
or timestamp; `llvm-objdump -s` on both files locates the differing
section.

## Boot test

```sh
make test                    # debug
make BUILD=release test      # release
```

`tests/boot/run_boot_test.py` boots `$(OUT)/cosmoos.img` through
`scripts/qemu-run.sh` with serial captured to `$(OUT)/boot-test.log`,
waits up to 90 s, and judges the run on two independent signals that must
agree:

1. QEMU's exit status decodes (`(status - 1) >> 1`) to `0x10`
   (`ARCH_EMULATOR_EXIT_SUCCESS`).
2. The log contains every required marker and none of the forbidden ones.

Required markers (regular expressions, anchored per line):

```
^cosmoboot-uefi v\d+
^jumping to kernel entry
^CosmoOS kernel 
^Architecture: x86_64
^Boot: UEFI
^\[ INFO\] boot complete
```

Forbidden: `KERNEL PANIC`, `BUG:`, `SELFTEST: FAIL`, `cosmoboot: FATAL`.
When any `SELFTEST:` line appears (debug builds), a `SELFTEST: PASS` line
is also required; `--expect-selftest yes|no` overrides the auto rule.

On failure the harness prints each unmet criterion and the whole serial
log, so a CI failure is diagnosable from the job output alone.

Measured: PASS in about 2 s for both build types under TCG.

## Crash test

```sh
make test-crash
```

Builds a kernel with `CRASH_TEST=1` into `$(OUT)-crash` and runs the
harness with `--expect-panic`. The kernel writes to the canonical but
unmapped address `0xFFFF900000000000` after the self-tests
(`kernel/core/main.c`), which must produce:

```
^\[ INFO\] crash test: writing to an unmapped address
^KERNEL PANIC: unhandled exception 14 \(#PF page fault\)
^trap 14 
^RIP=[0-9a-f]{16} CS=
^CR2=ffff900000000000 \(not-present write kernel\)
^stack trace:
^  #0 +0xffffffff8[0-9a-f]{7}
^halting\.
```

and QEMU exit value `0x11`. Forbidden in this mode: `boot complete`
(the fault must happen), `crash test: write did not fault`, a recursive
panic, and any loader fatal. This is the test that proves the failure
side of the exit-code contract and the completeness of the panic report,
so a regression in `panic.c`, `trap.c`, or `backtrace.c` cannot hide
behind a passing `make test`.

## Continuous integration

`.github/workflows/ci.yml` runs on push and pull request on
`ubuntu-24.04`:

1. `apt-get install clang lld llvm make mtools qemu-system-x86 ovmf python3`
2. `make check-tools`
3. `make BUILD=debug test`
4. `make BUILD=release test`
5. `make test-crash`
6. `make analyze`
7. `make reproducible`

Serial logs are uploaded as artifacts on failure. CI runs QEMU under TCG
only. The runner is x86-64; ARM64 hosted runners are not enabled for this
private repository, so the ARM64 primary host is exercised manually until
that changes.

## What is not tested yet

- Host-side unit tests of `printf.c` and `string.c` (they run only as
  in-kernel self-tests).
- Booting on real hardware or under Parallels (QEMU is the defined test
  platform; hardware is a later milestone).
- Fuzzing of the ELF loader's input parsing. `elf.c` is small and fully
  bounds-checked, but a host-side fuzz harness is the right next step for
  it once `tools/` exists.
