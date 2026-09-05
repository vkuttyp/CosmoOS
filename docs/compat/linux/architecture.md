# Linux compatibility: architecture

Phase 11 of the roadmap ("Linux ELF, Linux personality, syscall
translation, signals, futex, mmap, sockets, dynamic linker").
Constitution section 38 (a Linux process personality translates the
Linux syscall ABI onto the native subsystems; never intercept CPU
interrupts for it), section 39 (personalities define numbering,
conventions, errno, signals, filesystem and socket semantics, ELF
behaviour), section 40 (incremental: first statically linked ELF, basic
process creation, basic syscalls; then dynamic linking, signals,
threads, mmap, futex), section 63 (`compat/linux/`), invariant 7 (Linux
compatibility must not contaminate the native ABI).

This phase delivers ABI stage 1 of section 40 in full and the parts of
stage 2 a statically linked single-threaded program touches (`mmap`,
`brk`, the thread pointer, `futex`, the signal-table calls); the dynamic
linker, threads and signal delivery are the next stage and are recorded
as gaps, not attempted.

## Where it sits

```text
   Linux program (static ELF, e.g. built with musl-gcc -static)   tests/linux/*.c (freestanding, raw Linux ABI)
        │  SYSCALL: rax = Linux number, rdi rsi rdx r10 r8 r9
        ▼
   kernel/arch/x86_64/syscall_entry.S  ─►  kernel/syscall/syscall.c: pers->table[nr]
        │                                   (the same entry and dispatcher as the native personality)
        ▼
   compat/linux/syscalls.c        personality_linux: 87 translated Linux calls, each translating arguments,
   compat/linux/*.c               structures (stat, dirent64, utsname, timespec, iovec, sockaddr_in),
        │                         flags (O_*, MAP_*, AT_*, futex ops) and results (wait status, errno)
        ▼
   native kernel services         vfs_* / file_* (files), ksock_* (sockets), process_* (processes),
                                  vm_user_* (memory), futex_* (kernel/ipc), clock, random, tty
```

Selection happens once, at `spawn`: the ELF loader looks for the
`CosmoOS` ABI note (`PT_NOTE`, name `CosmoOS`, type 1) that every native
program carries from `crt0.S`; a static x86-64 executable without it runs
under the Linux personality (a process the kernel itself creates, such
as `init`, is always native). Nothing else about the process differs:
the same `struct process`, handle table, address space, scheduler, and
the same exit and kill paths.

## Purpose

Run programs built for Linux without rebuilding them, starting with the
statically linked ones a small libc produces, by translating at the
system-call boundary and nowhere else. Every translated call lands in
the same native subsystem the native call uses (section 38's two
columns), so nothing in the VFS, network stack or process code learns
what Linux is.

## Responsibilities

- **Personality selection** (`kernel/process/elf.c`, `process.c`): the
  loader records whether the image carries the CosmoOS note;
  `process_create_from_elf` picks `personality_native` or
  `personality_linux`. Native programs get the note from `crt0.S`
  (`.note.cosmo`, placed in a `PT_NOTE` segment by `user.ld`). `user.ld`
  also puts the ELF and program headers inside the text segment
  (`FILEHDR PHDRS`) so the loader can hand a Linux libc `AT_PHDR`.
- **Linux ELF and initial stack**: static `ET_EXEC` images load through
  the existing loader (W^X, page overlap and range rules apply
  unchanged). A Linux process's initial stack carries the auxiliary
  vector a Linux libc reads: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
  `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` (16 random bytes), `AT_UID`,
  `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_HWCAP`, `AT_CLKTCK`,
  `AT_NULL`.
- **Thread pointer**: `arch_prctl(ARCH_SET_FS/ARCH_GET_FS)` sets the
  thread's `tls_base`; the arch switch code loads `MSR_FS_BASE` for user
  threads so `%fs:` accesses (a libc's thread control block) work.
- **Memory**: `brk` (a per-process heap region above the image),
  anonymous `mmap`/`munmap`/`mprotect` (`MAP_FIXED`, `MAP_PRIVATE`,
  `MAP_ANONYMOUS`; file mappings are refused with `ENODEV`).
- **Futex** (`kernel/ipc/futex.c`, a native primitive the personality
  uses): `FUTEX_WAIT`/`FUTEX_WAKE` on user words with relative
  timeouts; enough for a libc's locks and `sleep` implementations.
- **Signals, stage 1**: `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`
  store their state per process so libcs initialise; `kill` translates
  to the native kill (termination); no handler is ever invoked.
- **Files, directories, processes, time, identity, sockets**: the
  translated set is listed in `api.md` (87 calls; 13 more are known and
  refused with `-ENOSYS`). Any other number below 512 lands on
  `lx_unknown`: `-ENOSYS`, counted in the process's `unknown_syscalls`,
  the first eight logged at DEBUG; a number at or above 512 is refused
  by the dispatcher's own bounds check.
- **`uname`** reports `Linux 6.0.0-cosmo x86_64` (a personality
  decision: libcs check the kernel name and version).
- **Architecture**: the numbers table is the x86-64 Linux ABI and is
  compiled only for `ARCH=x86_64`. On AArch64 (Phase 13) the personality
  is still selected for a binary without the CosmoOS note, but its table
  is empty (`count = 0`), so the first system call is `-ENOSYS`; the
  AArch64 Linux table (the generic `unistd` numbering) is a later phase.

## Non-responsibilities

- The dynamic linker (`PT_INTERP`), shared objects, `execve`, `fork`,
  `vfork`, `clone` and threads: stage 2 and beyond; each returns
  `-ENOSYS`.
- Signal delivery to user handlers, `sigreturn`, real-time signals,
  job control: recorded; `kill` terminates as it does natively.
- `/proc`, `/sys`, `/dev` nodes, `ioctl` beyond "not a terminal",
  `epoll`/`poll`/`select`, `sendmsg`/`recvmsg`, file-backed `mmap`,
  `mremap`, shared memory, `clone`-based anything, resource limits,
  namespaces, seccomp: later stages or never.
- Running a Linux distribution's userspace (stage 4).
- Anything in the native personality or subsystems on behalf of Linux:
  the translation layer adapts to them, never the reverse.

## Interfaces at a glance

| Interface | Where | Used by |
|---|---|---|
| `personality_linux` (table, count, name) | `compat/linux/syscalls.c`, `kernel/process.h` | `process_create_from_elf`, the dispatcher |
| The CosmoOS ELF note (`.note.cosmo`) | `libc/src/crt0.S`, `userland/user.ld`, `kernel/process/elf.c` | personality selection |
| Linux ABI definitions (numbers, structures, flags) | `compat/linux/linux_abi.h` | the translation only |
| `futex_wait`/`futex_wake` | `kernel/include/kernel/futex.h` | the Linux personality (native programs have no futex call yet) |
| `arch_prctl` → `thread->tls_base` → `MSR_FS_BASE` | `compat/linux/syscalls.c`, `kernel/arch/x86_64/context.c` | Linux libcs |
| `tests/linux/lxtest`, `lxhello`, `hello_musl` | `tests/linux/`, `/boot/tests/linux/` | `/etc/rc.test`, CI |

Tests (`testing.md`): `tests/linux/lxtest.c` is a freestanding program
using the raw Linux ABI (its own `_start`, Linux numbers and structure
layouts) that exercises every translated area and prints
`LINUXTEST: PASS`; `lxhello` prints one line; both are built with the
project toolchain and run from `/etc/rc.test` in self-test builds.
Where a `musl-gcc` exists (the CI runner installs `musl-tools`; on
macOS `MUSL_GCC=` may name a wrapper that compiles in an Alpine
container), a real statically linked musl program is built and run as
well; the harness requires its output when the build had musl. Host
tests cover the pure translation helpers (structure conversion, flag
mapping, wait status, dirent records); the kernel self-test `linux-elf`
covers the loader's note and program-header detection.
