# Shared libraries (roadmap 1.1 P4) — 2026-10-19

The project owner's task of 2026-10-19, PRs 3-5: musl's dynamic linker
as `PT_INTERP`, a dynamically linked BusyBox, and `pkg`'s record of the
libraries a package provides and requires. **Acceptance:** a dynamically
linked musl BusyBox and a small dynamically linked program using
`dlopen` pass the M3 scripted test on both architectures. Base: main
`f7107c2c` (v1.0.6). Branch `p4-dynlink`, one commit per PR.

## What was there

The loader already honoured `PT_INTERP`: `image_build`
(`kernel/process/process.c`) validated the interpreter, loaded an
`ET_DYN` one at the first free range at or above the space's random mmap
base and passed `AT_BASE`. Only a stand-in had run through it
(`tests/linux/lxinterp.c`, `RELATIVE` relocations only). musl was built
`--disable-shared`, its builtins after it and not position-independent;
the Linux vector had 40 words of room (`PERSONALITY_AUXV_WORDS`), with
36 used; `AT_SECURE` and `AT_HWCAP` were 0; `stat`'s `st_dev` was 0; the
boot archive put `lib/` under `/boot`.

## The exec path, as built (PR 3)

1. `spawn`/`execve` validates the executable and, when it names
   `PT_INTERP`, opens that path through the caller's lookup with
   `VFS_MAY_EXEC`. A missing interpreter is that lookup's `ENOENT`
   before anything of the caller changes (`lxnointerp`).
2. The executable (a PIE) lands at a random page bias in
   `[USER_PIE_BASE, +2^40)`; the interpreter at the first free range at
   or above the space's random mmap base, so both move per exec (M5's
   ASLR).
3. The process starts at the interpreter's entry with the vector below.
   musl's linker, `/lib/ld-musl-<arch>.so.1` (a link to `/lib/libc.so`,
   which is libc and linker in one), relocates itself, then maps each
   `DT_NEEDED` library itself: an anonymous reservation, `MAP_PRIVATE |
   MAP_FIXED` file mappings over it, `mprotect` of `PT_GNU_RELRO` to
   read-only after relocation, the thread pointer through
   `arch_prctl(ARCH_SET_FS)` or `TPIDR_EL0`. All of it ran before on the
   Linux door; what was verified rather than assumed is in the tests
   below.
4. W^X (L12) refuses any `PROT_WRITE|PROT_EXEC` mapping or protection.
   A library with text relocations needs exactly that, so its load fails
   with `EINVAL`, and the kernel logs `linux: pid N: mprotect with
   PROT_WRITE|PROT_EXEC refused (W^X): a library with text relocations
   cannot be loaded here; build it -fPIC` (`lx_wx_refused`). No shipped
   binary has text relocations (`llvm-objdump -p` of `libc.so` and
   `busybox.dyn` on either architecture: no `TEXTREL`, a `RELRO` segment,
   no `rwx` load segment), so W^X and the shipped set do not conflict.
5. musl tells two loaded libraries apart by `st_dev` and `st_ino`; an
   inode number is unique within one filesystem only, so `stat` now
   reports the mount's id as the device (`cosmo_stat.dev`, Linux
   `makedev(0, id)`).

The stand-in (`lxinterp`, `lxdyn`) is removed. Native CosmoOS programs
stay static in 1.1: the interpreter path is the personality's common
code, but no native program names one (inventory section 19).

## The auxiliary vector

`linux_auxv` (`compat/linux/syscalls.c`), room raised to 64 words, 32
pairs; a `KASSERT` checks each pair fits with the null one after it:

| Entry | Value |
|---|---|
| `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` | the executable's program headers (biased), 56, their number |
| `AT_PAGESZ` | 4096 |
| `AT_BASE` | the interpreter's bias, 0 without one |
| `AT_ENTRY` | the executable's entry (biased) |
| `AT_RANDOM` | 16 random bytes on the stack |
| `AT_EXECFN` | the path passed to `execve` |
| `AT_PLATFORM` | `x86_64` or `aarch64` |
| `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID` | the credentials' real and effective ids |
| `AT_SECURE` | 1 when the effective uid or gid is not the real one (setuid bits are ignored here) |
| `AT_HWCAP` | x86-64 `CPUID.1:EDX`; AArch64 the `HWCAP_` bits decoded from the boot CPU's ID registers as Linux's `arm64_elf_hwcaps`, for what EL0 may use unaided |
| `AT_HWCAP2` | x86-64 0 (`CR4.FSGSBASE` is off); AArch64 the `HWCAP2_` bits likewise |
| `AT_CLKTCK` | 100 |

No `AT_SYSINFO_EHDR` (no vDSO). The native vector is unchanged.

## The dynamic BusyBox (PR 4)

The same source and `busybox.config` less `CONFIG_STATIC`, built through
`cc-dyn` (`-fPIC`, `-pie`, `Scrt1.o`, `--dynamic-linker`): `/bin/busybox.dyn`,
with a link per applet under `/usr/bin`. `/bin/busybox` stays static and
the shell; `/bin` comes first in every `PATH` the system sets.

## pkg (PR 5)

`pkgbuild.py` writes `provides:` (each `DT_SONAME`) and `requires:` (each
`DT_NEEDED` the package does not provide itself) into a package's
manifest. `pkg install` refuses a package whose required library neither
the base system (`/etc/pkg/provides`: `libc.so`), the package itself nor
an installed package provides; `pkg remove` refuses to remove a library
another installed package requires and nothing else provides
(`--force` overrides, as for files). No solver. `INDEX` carries neither
key, so v1.0.6's `pkg` reads the same index. The repository gains
`busybox-dyn` and the fixtures `libdemo`, `demo-c` and `demo-d`.

## Tests

| Check | Where | x86-64 | AArch64 |
|---|---|---|---|
| auxv of a static program, `AT_SECURE` 1 for a setresuid child, missing interpreter `ENOENT` with the caller intact | `lxtest` (debug boot) | `lxtest: auxv ok` | `lxtest: auxv ok` |
| the vector against `dladdr`/`dl_iterate_phdr`, layout, RELRO of the executable (and of libc.so on x86-64), W^X, TLS | `dynhello` (debug boot, test-busybox) | `dynhello: ok` | `dynhello: ok` |
| `dlopen` through `RUNPATH`, `dlsym`, a missing library, a text relocation refused, `LD_LIBRARY_PATH` used (7) and ignored when `AT_SECURE` (42), library ASLR | `dltest` (debug boot, test-busybox) | `dltest: ok` | `dltest: ok` |
| M3's `ash.sh` and the BusyBox testsuite under `/bin/busybox.dyn`, `/usr/bin` first | `make test-busybox` | PASS | PASS |
| a required library missing, a library still required, `busybox-dyn` installed and run | `rc.test` (debug boot) | its six lines | its six lines |
| manifest round trip, `elf_libs` of a built library and program | `make host-test` | PASS | PASS |

ASLR, from the debug boots (`dltest` runs `dynhello addr` and `dltest addr` twice each and requires every pair to differ):

| | x86-64 | AArch64 |
|---|---|---|
| libc.so | `0x10df18a67000`, `0x10f0e5db6000` | `0x1091f9840000`, `0x109af7710000` |
| executable | `0x55659d4a9000`, `0x55c6864cc000` | `0x561388376000`, `0x557973f2b000` |
| libdltest.so | `0x107b7facf000`, `0x10d4eb110000` | `0x102a62019000`, `0x10170fea9000` |

On AArch64 musl leaves the linker's own RELRO (libc.so's) unprotected:
`kernel_mapped_dso(&ldso)` records it while `PAGE_SIZE` (`libc.page_size`,
from `AT_PAGESZ`) is still 0 (`ldso/dynlink.c`), so the range rounds to
nothing. That path does not depend on the kernel, so musl does the same
under Linux; it is upstream behaviour, recorded in the inventory. `dynhello` checks libc.so's RELRO on x86-64
only, and the executable's on both (an 8 KiB relocated table makes its
RELRO at least a page).

## Validation
