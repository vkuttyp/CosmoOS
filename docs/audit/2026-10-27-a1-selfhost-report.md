# Roadmap 1.2 A1, second PR: `make test-selfhost`, 2026-10-27

A1's acceptance (`docs/roadmap-1.2.md`): on the installed system, `cc`
compiles and runs a static, a dynamic and a `dlopen` program; tcc's own
`tests/tests2` pass, exclusions listed; tcc rebuilds itself and the
rebuilt compiler builds the same `hello` byte for byte. The first PR
(#468) put TinyCC on the image with a smoke test in the debug boot. This
one adds the acceptance test, and fixes the four defects in the port
that the test found. The clang evaluation, the rest of A1, comes next.

## Scope and baseline

Branch `a1-selfhost` from main at `60442953` (#468).

## The test

`tests/boot/selfhost_test.py` installs the build on a disk and boots it.
The harness serves `tests/tcc` and the source the installed tcc was built
from (`$(TCC_DIR)/src`, the pinned tarball with `ports/tcc`'s patches,
packed as `tinycc.tar.gz`) over HTTP. `tests/tcc/selfhost.sh` then runs
three stages on the system: `programs`, `tests2` and `rebuild`
(`docs/userland/testing.md`, "test-selfhost"). The system has no `make`
until A2, so `selfhost.sh` spells out tests2's Makefile rules for this
configuration: per-test flags, arguments, `NORUN`, and the two-file and
shared-library tests. The exclusions (`tests/tcc/tests2-skip.txt`,
`tests2-skip-<arch>.txt`) are only what that Makefile skips itself:
- another architecture's tests, and i386's and riscv64's;
- those needing the bounds checker or backtraces, which the port does not
  build;
- `34_array_assignment`.

Nothing is excluded for CosmoOS's sake.

## What it found, and the fixes (`ports/tcc`)

The test was run against the port as #468 left it, on x86-64 and then on
AArch64 (2026-10-27).

1. **Every `-run` test failed: `runmain.o` not found.** tcc's `-run`
   starts a program through `runmain.o`, which the port built (lib's
   `all`) but did not install. It is now in `/usr/lib/tcc`, and
   `check-reproducible.sh` compares it.
2. **Then `/usr/lib/libc.so: unrecognized file type`.** The installed
   tcc was a static program, so `-run` could not `dlopen` the libraries a
   program names (inferred from tcc's memory-output path). tcc is now
   linked against the system's `libc.so` through the BusyBox port's
   `cc-dyn`: a PIE with `/lib/ld-musl-<arch>.so.1` as its interpreter.
3. **Then `mprotect failed`.** tcc's default (`CONFIG_RUNMEM_RO` 0,
   `tccrun.c`) maps the code it made writable and executable at once,
   which the kernel refuses (`vm_user_protect`: no page is both). tcc
   already has a setting for systems with that rule, on by default only on
   macOS: `-DCONFIG_RUNMEM_RO=1` maps code read and execute and data read
   and write. The port builds with it, and so does the rebuild stage. The
   kernel's rule is unchanged.
4. **AArch64 only: `124_atomic_counter` failed to compile.**
   `incompatible redefinition of 'wchar_t'`: tcc defines `wchar_t` as
   `int` on every Linux target, but AArch64 Linux makes it `unsigned int`
   (AAPCS64), and musl's `bits/alltypes.h` defines it so beside tcc's
   `stddef.h`. Any program including both broke. The new
   `wchar-arm64.patch` gives `__WCHAR_TYPE__`, wide character literals and
   wide string literals that type on AArch64 Linux.

Found while reading the build: passing `--extra-cflags` replaces
configure's `-Wall -O2`, so #468's tcc was built without optimisation. It
is now passed explicitly. CI's cache key for the pinned source archives
now includes `ports/tcc/tcc.mk`; without it, CI fetched the tcc tarball
on every run.

## Run (2026-10-27, development Mac, QEMU TCG)

| Check | x86-64 | AArch64 |
|---|---|---|
| `make test-selfhost` (release) | PASS 124.7 s: programs; tests2 125 passed, 16 excluded; stage 1 = stage 2 (407,708 bytes); hello 3,471 bytes from both | PASS 145.6 s: programs; tests2 126 passed, 15 excluded; stage 1 = stage 2 (496,959 bytes); hello 3,463 bytes from both |
| `make test` (debug; `cc-hello` static and dynamic) | PASS 191.4 s | PASS 203.2 s |
| `BUILD=release make test` | PASS 19.2 s | PASS 21.6 s |
| `BUILD=release make reproducible` | yes (with `tcc/runmain.o`) | yes |

`make host-test`: PASS; `make check-docs`: PASS. No kernel source changed,
so the static analyzer was not rerun.

## Not done here

The clang evaluation (A1's last part, recorded in the roadmap). `make` on
the target is A2.
