# Roadmap 1.2 A1: the clang evaluation, 2026-10-27

A1 (`docs/roadmap-1.2.md`): "Clang is evaluated in the same milestone and
recorded: a static musl `clang`+`lld` cross-built on the host runs under
the Linux personality without new kernel work if its memory and `mmap`
needs fit, and is adopted only if the evaluation says so." This is that
record. The tooling is `tools/clang-eval/` (`make clang-eval`, `make
test-clang-eval`; its README).

**Verdict: not adopted.**
- A static clang does not run on CosmoOS without new kernel work. Exec
  refuses any program over 16 MiB, and clang is 79-84 MB and lld 47-50 MB.
- With that limit raised in an experiment, clang and lld work on both
  architectures. But each run costs about 45-60 s under TCG before it
  compiles anything (the exec copy, inferred). A large compile needs up to
  132 MiB.
- tcc stays `cc`. Revisit after MEM-11 (exec mapping programs from the
  page cache), and only if a later milestone needs a compiler tcc cannot
  be.

## What was built

LLVM 19.1.7 (`llvm-project-19.1.7.src.tar.xz`, SHA-256 `82401fea…97501`,
the release the BusyBox port takes compiler-rt from), cross-built on the
development Mac:
- the host's tablegen;
- libc++, libc++abi and libunwind for `ARCH-linux-musl`, static;
- clang and lld for `ARCH-linux-musl`, `-static` against musl and that
  libc++, with one LLVM target each.

| | x86-64 | AArch64 |
|---|---|---|
| `clang` (stripped, static) | 84,479,424 bytes | 79,029,392 bytes |
| `lld` | 50,132,264 bytes | 46,771,016 bytes |
| package (`clang.tar.gz`) | 53 MB | 51 MB |
| build time (8-core Mac, after the shared tablegen) | about 15 min | 15 min |

Making the build and package right took five changes, each recorded in
`build.sh`:
- `<linux/futex.h>`: libc++'s `atomic.cpp` needs it and the musl sysroot
  has no Linux UAPI headers, so `build.sh` supplies the two constants.
- `__cxa_thread_atexit_impl` off: CMake's checks only compile, so they
  found glibc's hook on musl.
- The 4 MiB image base: lld's default 2 MiB is below the user window,
  the same rule the ports' `cc` wrapper follows.
- `clang.cfg` beside clang, so the programs clang links get that image
  base too.
- compiler-rt's crtbegin and crtend objects and `libunwind.a` in the
  per-target runtime directory, under the normalized triple.

## On the system

`make test-clang-eval`: this build installed and booted, with a 2 GiB data
pool for the package; the boot runs `tools/clang-eval/eval.sh`.

**The released kernel: clang does not start**, on either architecture.
- Exec reads the whole program file into kernel memory
  (`kernel/process/spawn.c`: `vm_kernel_alloc(size, VM_KALLOC_POPULATE)`)
  and refuses a file over `SPAWN_IMAGE_MAX`, 16 MiB, with `-ENOEXEC`.
- It logs nothing. The shell then runs the ELF as a script (`line 1:
  syntax error`).
- That is the roadmap's "new kernel work" condition, failed: MEM-11.

**An experiment, not committed: `SPAWN_IMAGE_MAX` raised to 128 MiB**, in
a separate build directory, 1 GiB guests, release, QEMU TCG on the Mac.
Times are wall seconds for the whole command. Peak is the largest
`VmRSS` of any one compiler process, sampled every 0.2 s: the kernel
keeps no peak (no `VmHWM`, no `ru_maxrss`).

| Stage | x86-64 | AArch64 |
|---|---|---|
| clang and lld start (`--version`, resource dir, builtins) | yes | yes |
| `hello.c` static, by clang | 46 s, 17 MiB | 58 s, 19 MiB |
| `hello.c` dynamic, by clang | 48 s, 19 MiB | 59 s, 19 MiB |
| `libgreet.so` (`-shared -fPIC`) | 47 s, 13 MiB | 58 s, 14 MiB |
| `dl-main` (`dlopen` of it); runs | 48 s, 15 MiB | 57 s, 17 MiB |
| `tcc.c` by tcc `-O2` | 1 s, 5.4 MiB | 2 s, 5.3 MiB |
| `tcc.c` by clang `-O0` | 56 s, 51 MiB | 69 s, 47 MiB |
| `tcc.c` by clang `-O2` | 125 s, 85 MiB | 172 s, 132 MiB |
| the clang-built tcc builds and runs `hello` | yes | yes |

Every program ran. Nothing in `mmap`, threads, `dlopen` or the Linux
personality failed. The cost is the exec:
- A `hello` takes 46-59 s, and almost none of that is compiling
  (inferred: `-O0` of all of tcc.c adds only about 10 s).
- Each clang run execs three programs, one at a time: the driver, its
  compile process and lld. That is about 220 MB copied into freshly
  zeroed kernel memory.
- At 256 MiB on x86-64, the static and dynamic programs passed (65-73 s
  each). The next clang run panicked the kernel: `TLB shootdown ...
  acknowledged by 2 of 3 CPUs; not by cpu 0`. CPU 0 was in `memset`
  with no tick for 1,001 ms, while exec's populate of the 84 MB image
  zeroed page after page under `kernel_space.lock` with interrupts off.
  That is MEM-3, already in the backlog. At the real 16 MiB cap the window
  is a fifth as long. The panic needs the experiment's larger images.

## Findings recorded

- **MEM-11** (new, usability, M): exec copies a program into kernel
  memory, refuses one over 16 MiB with a silent `-ENOEXEC`, and costs time
  and memory in proportion to its size. A large program needs PT_LOADs
  mapped from the page cache, as file `mmap` already is.
- **MEM-3** (evidence added): the populate's interrupts-off window, seen
  to panic a TLB shootdown with an 84 MB image.
- **BUILD-8** (new, nice-to-have, S): the BusyBox port's
  `libclang_rt.builtins.a` holds compiler-rt's `crtbegin.o` and
  `crtend.o`. A link with clang's own crtbegin pulls crtend in twice
  (duplicate `__CTOR_LIST_END__`). The ports' links never use crtbegin,
  so they are unaffected.

## What would change the verdict

MEM-11 removes the hard refusal and most of the fixed cost (inferred:
demand paging reads only the pages a run touches). Even then, under TCG a
clang compile of tcc.c at `-O2` took 2-3 minutes against tcc's 1-2 s.
clang is worth its size only for what tcc cannot build, or for code
quality: the clang-built tcc is about the same size as tcc's own. A3's
risk line ("the compiler may have to be clang") is where that would
show; this record is what it would start from.

## Run

Everything above was run on 2026-10-27, on the development Mac, release
builds of main at `4abc8d6a`:
- the stock-kernel runs, on both architectures;
- the 1 GiB experiments, on both;
- the 256 MiB experiment, on x86-64.

The experimental kernels lived only in `out/ARCH-release-bigexec`.
`kernel/process/spawn.c` was restored after each build (`git diff`
clean).
