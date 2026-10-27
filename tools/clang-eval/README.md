# tools/clang-eval

Roadmap 1.2 A1's clang evaluation (`docs/roadmap-1.2.md`, A1): "a static
musl `clang`+`lld` cross-built on the host runs under the Linux
personality without new kernel work if its memory and `mmap` needs fit,
and is adopted only if the evaluation says so." This directory is the
evaluation, not a port: nothing here goes into the image or runs in CI.
The result is recorded in `docs/audit/2026-10-27-a1-clang-eval-report.md`.

| Source | Version | SHA-256 |
|---|---|---|
| LLVM | `llvm-project-19.1.7.src.tar.xz` (the release the BusyBox port takes compiler-rt from) | `82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501` (fetched from the GitHub release 2026-10-27; GitHub publishes no digest for it) |

## Build (`make clang-eval`, `build.sh`)

For `ARCH`, into `out/clang-eval/ARCH`, in three steps:

1. The host's `llvm-tblgen` and `clang-tblgen`, shared by both
   architectures (`out/clang-eval/native`).
2. libc++, libc++abi and libunwind for `ARCH-linux-musl`, static, built
   by the host's clang through wrappers like the BusyBox port's `cc`
   (the musl sysroot, the compiler-rt builtins).
3. clang and lld for `ARCH-linux-musl`, linked `-static` against musl and
   that libc++, with one LLVM target (the architecture's own) and the
   target's defaults: lld, compiler-rt, libc++, libunwind.

The wrappers link at 4 MiB (`--image-base=0x400000`, as the ports'
`cc` does: lld's default 2 MiB is below the kernel's user window), give
libc++ the one Linux header it needs (`linux/futex.h`'s two constants),
and tell libc++abi that musl has no `__cxa_thread_atexit_impl`.

The package, `clang.tar.gz`:
- `bin/clang`, `bin/lld` (and `ld.lld`);
- `bin/clang.cfg`, which makes clang link its programs at 4 MiB too;
- `lib/clang/19/include`;
- in the per-target runtime directory, `lib/clang/19/lib/ARCH-unknown-linux-musl`:
  - the BusyBox port's builtins, less the `crtbegin.o` and `crtend.o` that
    archive carries (backlog BUILD-8);
  - compiler-rt's `clang_rt.crtbegin.o` and `clang_rt.crtend.o`;
  - `libunwind.a`.

## On the system (`make test-clang-eval`, `clang_eval_test.py`, `eval.sh`)

This build installed, booted with a 2 GiB data disk (`/mnt/d`; the root
is 256 MiB), the package and tcc's source fetched from the harness, then:

- `unpack`: versions, the resource directory and the builtins clang finds.
- `programs`: a static, a dynamic and a `dlopen` program built by clang;
  each runs.
- `bigger`: `tcc.c`, one translation unit of the whole compiler, built by
  tcc, by clang `-O0` and by clang `-O2`; the clang-built tcc builds and
  runs `hello`.

Each compile is timed, and the resident memory of every compiler process
(clang's driver, its compile process, lld, tcc) sampled from
`/proc/*/status` every 0.2 s: the kernel keeps no peak (`VmHWM`,
`ru_maxrss`). `QEMU_MEM` sets the guest's memory (256M by default).

On the released kernel clang does not start: exec refuses a program
over 16 MiB (backlog MEM-11). The report's measurements come from an
uncommitted kernel with that limit raised.
