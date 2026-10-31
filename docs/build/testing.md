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

## Nova sources

```sh
make nova-check
```

`scripts/nova-gen.py` compiles every Nova source with the pinned novac
into `out/nova` and checks the sources the kernel must refuse
(`tests/nova/negative/*.nova`: exactly the errors their `// error[N…]`
lines name). CI's "Nova sources" job runs it; locally it needs the pinned
novac, as every build does (`docs/build/design.md`, "Nova sources"). That
the generated C is what a source tarball carries, and builds the same
bytes with no novac, is `make reproducible`'s (below).

## The Nova share

```sh
python3 scripts/nova-share.py                      # the table for HEAD
python3 scripts/nova-share.py --gate origin/main   # and no tree with more C to convert than main
```

`scripts/nova-share.py` (roadmap 2.0 SA1) counts the lines of ring 0
(`kernel/`, `kernel-services/`, `drivers/`, `compat/`, `boot/`) and of
CosmoOS's own userland (`libc/`, `userland/`, `pkg/`) per tree and per
subsystem: Nova sources; the floor (assembly, and the C files its `FLOOR`
names: Nova's runtime shim so far); and C to convert, every other `.c`
and `.h`, of which a port's `*_floor.c` is shown on its own. C generated
from a `.nova` source is not counted again. It reads a git tree, so the
numbers belong to a commit. CI's `nova-share` job runs it on every pull
request and push with `--gate`, which fails when any tree has more C to
convert than the commit the change started from: new code is Nova
(`docs/roadmap-2.0.md`, "The Nova rule", point 1). A file that has to stay
C is added to `FLOOR` in the same change, where its review asks why.

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

Runs `clang --analyze -Xanalyzer -analyzer-output=plist-multi-file` on every
enumerated kernel, loader, module and package source with its compilation
flags. Every invocation performs full analysis. Structured `*.analyzed`
reports retain diagnostics in headers as well as the main source file.
`scripts/check-analysis.py` rejects missing or malformed reports, any
diagnostic exceeding `tools/analysis/<arch>.json`'s reviewed baseline, and
any baseline entry not observed in the current run.
Each baseline key includes checker, translation unit, file, function and
message, with a reason and occurrence count; source line numbers are not
keys. The architecture baseline contains separate reviewed sets keyed by
the exact compiler version, and an unknown version fails. The CI run uploads
`$(OUT)/analysis-inventory.json`, including the compiler identity and the
complete current list.
Success reports the baselined count and zero unexpected diagnostics.

## Reproducibility

```sh
make reproducible            # or scripts/check-reproducible.sh x86_64 debug
```

Builds the tree twice into `out/repro-a` and `out/repro-b` and compares
every output of the release set with `cmp` (`scripts/check-reproducible.sh`
lists them). The second build is of a `make dist` tree
(`out/repro-src`, the generated C beside each `.nova` source), with a
`novac` that fails if run: so a source tarball builds with no novac, and
its C is the pinned novac's. Current result: identical. If this fails, the diff usually points at a new absolute path
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

1. `apt-get install clang lld llvm libclang-rt-dev make mtools musl-tools python3 qemu-system-x86 ovmf qemu-system-arm qemu-efi-aarch64`
2. `make check-tools`
3. `make BUILD=debug test`
4. `make BUILD=release test`
5. `make test-crash`
6. `make analyze`
7. `make reproducible`

Serial logs are uploaded as artifacts on failure. CI runs QEMU under TCG
only (invariant B10). The runners are x86-64; the AArch64 targets run
under TCG on them, and the ARM64 primary host is exercised manually. (The
repository is public since 2026-10-26, so GitHub's ARM64 hosted runners
are available to it; none is used yet.)

### Which jobs a pull request runs

A run's first job, `gate` (`scripts/ci-gate.py`), decides what the rest
run. Two acceptance tests take most of a run's time under TCG: the
large-file test (`make test-large`, about 65 minutes on x86-64 and 78 on
AArch64 in run 40398038325) and the encrypted root's `encrypt` part
(`make test-encrypt`, about 17 and 41). Every other job ends within about
28 minutes. So:

- **A push to main, and a run by hand** (`workflow_dispatch`), run every
  job.
- **A pull request** runs every job except those two, unless it changes a
  path they exercise (`HEAVY_PATHS` in the script: the file system, block,
  storage and memory code, booting, installing, upgrading and keys, the
  boot harness and tests, the build and CI) or carries the **`full-ci`**
  label. Adding the label starts a full run; another label starts nothing.
  The skipped tests show as skipped (the large-file job) or as an
  `encrypt` part whose build and test steps were skipped.
- **A pull request's newer push cancels its older run**, which can only
  report on a commit nobody will merge. Other runs never cancel each other.

A regression the gate lets through, from a path it does not list, is
caught by the full run on main after the merge. Label the pull request
`full-ci` when a change could reach the file system or the upgrade path
indirectly.

## What is not tested yet

- Host-side unit tests of `printf.c` and `string.c` (they run only as
  in-kernel self-tests).
- Booting on real hardware or under Parallels (QEMU is the defined test
  platform; hardware is a later milestone).
- Fuzzing of the ELF loader's input parsing. `elf.c` is small and fully
  bounds-checked, but a host-side fuzz harness is the right next step for
  it once `tools/` exists.
