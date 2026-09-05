# Linux compatibility: testing

Constitution section 57 (every subsystem tested where it runs) and
section 61 (an ABI is tested with fixtures: layouts and numbers, good
and bad). The personality is tested at three levels: the pure
conversions on the host under the sanitizers, freestanding programs
that speak the raw Linux ABI on the target, and a real statically
linked musl program built by a Linux toolchain.

| Level | What | How to run |
|---|---|---|
| Host, conversions | `tests/host/test_linux.c` over `compat/linux/convert.c` under ASan/UBSan | `make host-test` |
| Kernel self-test | `linux-elf` (`kernel/process/proctest.c`): the loader's note and program-header detection | `make test` (self-test builds) |
| Target, raw ABI | `/boot/tests/linux/lxhello`, `/boot/tests/linux/lxtest` from `/etc/rc.test` | `make test` |
| Target, real libc | `/boot/tests/linux/hello_musl` from `/etc/rc.test`, built when `musl-gcc` exists or `MUSL_GCC` is set | `make test` (CI always; locally see below) |
| Serial-log markers | `LINUXTEST_MARKERS`, `MUSL_MARKER` in `tests/boot/run_boot_test.py` | `make test` |

## `test_linux` (host)

Layouts: `sizeof(struct lx_stat) == 144`, `lx_utsname` 390,
`lx_sockaddr_in` 16, `lx_sockaddr_in6` 28, `lx_sigaction` 32. Open
flags: `O_RDONLY`; `O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC` (the last
dropped); `O_RDWR|O_APPEND|O_EXCL|O_DIRECTORY|O_NONBLOCK`; access mode 3
and an unknown bit refused. Stat: a directory with inode, mode 0755,
nlink, uid, gid, 4096 bytes (`st_blocks` 8, `st_blksize` 4096), times
split into seconds and nanoseconds; each `COSMO_DT_*` type to its
`S_IF*` bits. Wait status: exit 0, 7, 255; killed by 9 and 15; the
fault status 139 → `SIGSEGV`. Sockaddr in: an `AF_INET` address with
port 8080 (network order) and 127.0.0.1; too short (8 and 1 bytes)
`-EINVAL`; an `AF_INET6` address with `::1`; `AF_UNIX` `-EAFNOSUPPORT`.
Sockaddr out: a full 16-byte write with a canary after it; a 4-byte
buffer receives only 4 bytes but the call reports 16; an `AF_INET6`
address reports 28. Dirents: two native records (`hello`, regular,
`reclen` 24; `.`, directory, `reclen` 16) laid out with the name at byte
12 convert to two `linux_dirent64` records of 32 and 24 bytes with
`d_type` 8 and 4, NUL-terminated names and `d_reclen` set; a 40-byte
output buffer takes only the first; a record whose `reclen` is below
the header converts to nothing; `lx_dirent_type` for every native type
and an unknown one. Prot: `READ|WRITE`; an unknown bit refused.

## Kernel self-test `linux-elf`

`elf_validate` on the boot archive's `init`: succeeds, `cosmo_note` is
true, `phdr_vaddr` non-zero, `phnum > 0`, `phent == 56`. On
`tests/linux/lxhello`: succeeds, `cosmo_note` false, `phdr_vaddr ==
0x400040` (the program header table right after the 64-byte ELF header
in the text segment `user.ld` now builds with `FILEHDR PHDRS`). One of
the 62 self-tests.

## The target programs (`tests/linux/`)

Built by `tests/linux/linux.mk` for both architectures (milestone 10)
with the user compiler flags and `userland/user.ld` but **without**
`crt0.o` or `libc.a`, so they carry no CosmoOS note; `lxabi.h` supplies
`_start` (reads `argc`/`argv` from the initial stack, calls `main`,
`exit_group`s), raw `syscall`/`svc` wrappers `sc0..sc6` with the Linux
register convention, and `lx_clone` (a thread with `fn(arg)` on its own
stack, `exit` at the end). Numbers and
layouts come from `compat/linux/linux_abi.h`, the same header the
kernel side compiles, so a mismatch between the two sides is impossible
by construction (a wrong layout would be wrong on both sides and the
real-libc test would catch it). The archive carries them under
`tests/linux/` (`ramfs` gives `tests/` entries mode 0755) and the shell
runs them as `/boot/tests/linux/<name>`.

`lxhello`: `write(1, "hello from linux abi\n")`, exit 0.

`lxtest`: prints `LINUXTEST: PASS` or one `LINUXTEST: FAIL <check>
(<value>)` line per failed check and `LINUXTEST: FAIL (n checks)`. In
order:

| Area | Checks |
|---|---|
| identity, uname, time | `getpid` > 0, `gettid == getpid`, `getppid` > 0, uid/euid 0; `uname` gives `Linux`/`LX_MACHINE`; `clock_gettime(MONOTONIC)` advances across a 5 ms `nanosleep`; clock 99 `-EINVAL`; milestone 10: `CLOCK_REALTIME` is between 2020 and 2096, `time()` and `gettimeofday()` agree with it within 2 s, `REALTIME_COARSE` is not behind it, `BOOTTIME` is small |
| thread pointer | x86-64: `arch_prctl(ARCH_SET_FS, tcb)`, `%fs:0` reads the word, `ARCH_GET_FS` returns the base, `ARCH_SET_GS` `-EINVAL`; AArch64: `msr tpidr_el0`; both: a sleep, then the pointer again |
| brk, mmap | `brk(0)`, grow 8 KiB and write both pages, shrink back, a request below the start returns the old break; anonymous `mmap` of 8 KiB written, `mprotect` to read-only, `munmap`; a file mapping `-ENODEV`; RWX `-EINVAL`; `MAP_FIXED` at a free page, `MAP_FIXED` again on it replaces (reads 0), then `munmap`. Milestone 5: eight pages, `mprotect` of the middle two to read-only (page 0 still writable, the two readable with contents), to `PROT_NONE` (`write` from it `-EFAULT`), back to RW (the byte survived); `munmap` of one page, `mprotect` across the hole `-ENOMEM`, `munmap` of the whole range 0; `brk` grow, shrink to one page, regrow larger: kept page, zero page, new byte |
| files | `openat(AT_FDCWD, "/tmp/lxtest.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644)`, `write`, `writev` of two iovecs, `fstat` (`S_IFREG`, size 22), reopen `O_RDONLY`, `pread64` at 6, `lseek` 0, `readv` into two buffers, `read` at EOF returns 0, `close`, second `close` `-EBADF`; `newfstatat` size, `stat` of a missing path `-ENOENT`, `/tmp` is `S_IFDIR`, fd 0 is `S_IFCHR`, `ioctl(1, TCGETS)` `-ENOTTY`, `access`; `mkdirat`, `rename` into the directory, `getdents64` sees `.`, `..` and `moved` (`d_type` 8) with 8-aligned records of at least 24 bytes, a second `getdents64` returns 0; `unlinkat`, `unlinkat(AT_REMOVEDIR)`; `chdir("/tmp")`, `getcwd` returns 5 (NUL included) and `/tmp`; `chdir("/")`; `umask` 022 |
| pipes, dup, fcntl | `pipe2(O_CLOEXEC)`, write 3 bytes, read them back; `dup`, `dup3` to 40, `dup3(fd, fd)` `-EINVAL`; `F_GETFL` on the write end is `O_WRONLY`; `F_DUPFD` from 50 lands at or above 50; `pipe2(O_NONBLOCK)`: an empty read is `-EAGAIN`, `F_GETFL` shows `O_NONBLOCK`, `F_SETFL(0)` clears it, `F_SETFL(O_NONBLOCK)` on the write end and 4096-byte writes fill exactly 16384 bytes before `-EAGAIN`; close every write end, `read(40)` returns 0 (EOF); `fstat(40)` is `S_IFIFO` |
| rlimits | `getrlimit(NOFILE)` 64/64, `prlimit64(0, AS)` 2 GiB, `RLIMIT_STACK` infinity; `setrlimit(NOFILE, 8)` then opens until `-EMFILE`; back to 64 (root); `cur > max` `-EINVAL`; another pid `-EPERM`; `setrlimit(STACK)` accepted and ignored |
| file mmap (milestone 10) | a 6000-byte file mapped `PROT_READ` over 8 KiB matches and its tail is zero; a `PROT_READ\|PROT_WRITE` mapping at offset one page matches and a write to it leaves the file unchanged (`pread64`); `MAP_SHARED\|PROT_WRITE` `-EOPNOTSUPP`; read-only `MAP_SHARED` works; an unaligned offset `-EINVAL`; a bad fd `-EBADF` |
| signals, wait, kill (milestone 10) | `rt_sigaction` stores and reads back, refuses `SIGKILL`, signals 0 and 64 and a wrong `sigsetsize`; `rt_sigprocmask` block and read back; `wait4(-1)` `-ECHILD`; `kill(999999, 15)` `-ESRCH`; `kill(self, 0)` 0; signal 65 `-EINVAL`; `SIGCHLD` to self is dropped; `execve` (and `fork`) `-ENOSYS`; then the handler tests: `kill(self, SIGUSR1)` runs the handler once with `SI_USER` and the sender's pid, the signal blocked inside, the mask restored after, `uc_stack` `SS_DISABLE`, the FXSAVE image present with the default MXCSR (x86-64) or the `esr_context` present (AArch64); `tgkill` gives `SI_TKILL`, an unknown tid `-ESRCH`; a blocked `SIGUSR1` stays pending (`rt_sigpending`) and is delivered by the unblock; `rt_sigsuspend` with a mask that admits it returns `-EINTR` after the handler and the old mask is back; `SA_RESETHAND` leaves `SIG_DFL`; `sigaltstack` 16 KiB accepted, 100 bytes `-ENOMEM`, an `SA_ONSTACK` handler runs on it (`sp` inside, `sigaltstack` from inside reports `SS_ONSTACK`), the query afterwards reports 0, `SS_DISABLE` round-trips; a store to `0x7000` with a `SIGSEGV` handler reports `SEGV_MAPERR` and the address and the handler steps over the instruction; the trampoline page reads back as its two instructions and a store to it is `SEGV_ACCERR`; `xmm0` survives a handler that zeroes it (x86-64) |
| threads (milestone 10) | `clone` with the pthread flag set: the tid is at least `0x10000`, `PARENT_SETTID` wrote it, the child's `gettid` matches, its TLS is the `CLONE_SETTLS` value, the parent's TLS is untouched, the join through `CHILD_CLEARTID` completes, `tgkill(tid, 0)` afterwards is `-ESRCH`; `CLONE_VM\|CLONE_THREAD` alone `-EINVAL`, `PARENT_SETTID` to NULL `-EFAULT`, a fork-like clone `-ENOSYS`, `CLONE_PTRACE` `-EINVAL`; a reader thread blocked in `read` gets `SIGUSR1` by `tgkill`: `-EINTR` and the handler ran in that thread; with `SA_RESTART` the read survives the signal and completes with data written later; two waiters on futex A (`WAIT_BITSET`, absolute `CLOCK_REALTIME` deadline) are moved by `CMP_REQUEUE` (2; a wrong value `-EAGAIN`), a wake on A finds nobody, `WAKE_BITSET` on B releases both; a past absolute deadline `-ETIMEDOUT`; a real bitset `-ENOSYS`; `sched_getaffinity` returns 8 with CPU 0 set, 4 bytes `-EINVAL`, an unknown pid `-ESRCH`; `sched_setaffinity` 0 / `-EINVAL` |
| poll, ppoll (milestone 10) | a pipe: the read end not ready, the write end `POLLOUT`; fd 77 `POLLNVAL` counted at once; a negative fd ignored; a 20 ms timeout with nothing ready returns 0 after at least 15 ms; a thread writing after 30 ms wakes a `poll(-1)` with `POLLIN`; `ppoll` with a zero timespec returns 0; 2000 entries `-EINVAL`; `ppoll` with a mask admitting a pending `SIGUSR1` runs the handler and returns `-EINTR` with the old mask back; the writer closed: `POLLHUP\|POLLIN` |
| futex | `FUTEX_WAIT|PRIVATE` with the wrong value `-EAGAIN`; `FUTEX_WAIT` with a 20 ms timeout `-ETIMEDOUT`; `FUTEX_WAKE` returns 0; operation 99 `-ENOSYS` |
| random, sockets | `getrandom(32)` returns 32; a UDP socket bound to 127.0.0.1 sends `ping` to itself and `recvfrom` receives it with the sender's address; `getsockname` into a 4-byte buffer reports 16 and leaves a canary intact; `fstat` on the socket is `S_IFSOCK`; `setsockopt(SOL_SOCKET, SO_REUSEADDR)` 0; `close`; `socket(AF_UNIX)` `-EAFNOSUPPORT` |
| non-blocking sockets | `socket(SOCK_DGRAM\|SOCK_NONBLOCK)` bound to 127.0.0.1: `recvfrom` `-EAGAIN`, `F_GETFL` has `O_NONBLOCK`; a listener made non-blocking with `F_SETFL`: `accept4` `-EAGAIN`; a `SOCK_STREAM\|SOCK_NONBLOCK` client's `connect` is 0 or `-EINPROGRESS`, `accept4(SOCK_NONBLOCK)` polled until it returns a socket, a second `connect` `-EISCONN`, the accepted end's `recvfrom` `-EAGAIN` until `hey` arrives |
| unknown numbers | 510 (in the table, unimplemented) and 9999 (beyond it) both `-ENOSYS` |

`lxsig` (`tests/linux/lxsig.c`, milestone 10): a program that dies the
way its argument says; `rc.linux` runs every mode and echoes the status
the shell collected, and the harness requires each line:

| Mode | What | Marker |
|---|---|---|
| `term` | `kill(getpid(), SIGTERM)`, the default action | `lxsig term: 143` |
| `segv` | a store through a null pointer, no handler | `lxsig segv: 139` |
| `ill` | `ud2` / `udf #0` | `lxsig ill: 132` |
| `badret` | `rt_sigreturn` to `rip`/`pc` = `0x8000000000000000` (the SYSRET/IRETQ guard, `docs/kernel/process/invariants.md` P-S1) | `lxsig badret: 139` |
| `badstack` | `rt_sigreturn` to a non-canonical `rsp`/`sp`, then a push (`#SS` / a data abort) | `lxsig badstack: 139` |
| `group` | a second thread calls `exit_group(7)` while the main thread spins | `lxsig group: 7` |
| `lastthread` | the main thread `exit`s; the other thread `exit_group(5)`s | `lxsig lastthread: 5` |

`lxinterp` and `lxdyn` (milestone 10): `lxdyn` is an `ET_DYN` executable
(linked `-pie -z norelro`, no `user.ld`) whose `PT_INTERP` names
`/boot/tests/linux/lxinterp`, itself an `ET_DYN` image without an
interpreter. The kernel loads both, starts `lxinterp` at
`USER_INTERP_BASE` or above with the original stack; `lxinterp` checks
the auxiliary vector (`AT_BASE` is its own `__ehdr_start`, `AT_PHDR`/
`AT_PHNUM`/`AT_ENTRY` describe `lxdyn` at `USER_PIE_BASE`, `AT_EXECFN` is
the path, `AT_PLATFORM` the machine, `AT_RANDOM` and `AT_PAGESZ` present),
applies `lxdyn`'s `R_X86_64_RELATIVE`/`R_AARCH64_RELATIVE` relocations
from `PT_DYNAMIC` (at least one must exist), prints `lxinterp: ok` and
jumps to `AT_ENTRY`; `lxdyn` checks that it sits at `USER_PIE_BASE`, that
a relocated pointer table and function pointer are right, that `argv`
arrived, makes a system call and prints `lxdyn: ok`.

`hello_musl` (`tests/linux/hello_musl.c`): `uname`, `malloc`,
`strcpy`, `printf`, `free`, and prints `hello from musl on Linux x86_64
(pid N)`. Under the hood musl's start-up performs `arch_prctl`,
`set_tid_address`, `rt_sigprocmask`, `brk`, `mmap`, `ioctl(TCGETS)`
(gets `-ENOTTY`: the output is fully buffered and flushed at exit),
`writev`, `exit_group`: the sequence a real Linux libc needs.

## `/etc/rc.test`, Linux section

After the package section, `rc.test` runs `/etc/rc.linux` when
`/boot/tests/linux/lxhello` exists (both architectures since milestone
10): `lxhello || exit 1`, `lxtest || exit 1`, `lxdyn || exit 1`, the
seven `lxsig` modes each followed by `echo "lxsig <mode>: $?"`, then
`hello_musl` only if the file exists. A failing Linux program therefore
also fails `SHTEST`.

The harness requires in self-test builds (`LINUXTEST_MARKERS`):

| Marker | From |
|---|---|
| `^hello from linux abi$` | `lxhello` |
| `^LINUXTEST: PASS$` | `lxtest` |
| `^lxinterp: ok$`, `^lxdyn: ok$` | the PIE pair |
| `^lxsig <mode>: <status>$` (seven lines) | `lxsig` through `rc.linux` |
| `^hello from musl on Linux x86_64 \(pid \d+\)$` (`MUSL_MARKER`) | `hello_musl`; required only when the environment has `HAVE_MUSL=1`, which `make test` sets from `tests/linux/linux.mk` |

Release builds run no `rc.test`, so the Linux programs run only in
self-test builds (the release image still carries them).

## Building `hello_musl`

`linux.mk` sets `MUSL_GCC ?= $(shell command -v musl-gcc)` and
`HAVE_MUSL := $(if $(MUSL_GCC),1,0)`; with a compiler the rule is
`$(MUSL_GCC) -static -Os -o hello_musl hello_musl.c`. The CI runner
installs `musl-tools` (`.github/workflows/ci.yml`), so CI always builds
and requires it. On macOS there is no `musl-gcc`; a wrapper that
compiles inside an x86-64 Alpine container serves, since Alpine's `gcc`
is a musl toolchain:

```sh
# musl-gcc-docker.sh: musl-gcc -static -Os -o OUT SRC, run in alpine (gcc + musl-dev)
docker build --platform linux/amd64 -t cosmoos-musl-builder - <<'EOF'
FROM alpine:3.20
RUN apk add --no-cache gcc musl-dev
EOF
# ... parse -o OUT and SRC, then:
docker run --rm --platform linux/amd64 -v "$srcdir":/src:ro -v "$outdir":/out cosmoos-musl-builder \
  sh -c "gcc -static -Os -o /out/$(basename "$out") /src/$(basename "$src")"
```

```sh
make MUSL_GCC=/path/to/musl-gcc-docker.sh test
```

`HAVE_MUSL` then becomes 1 and the harness requires the musl line. The
binary is about 200 KiB, static, with a `PT_GNU_STACK` and four
`PT_LOAD` segments; the kernel's loader accepts it as it is.

## Results as of milestone 10 (2026-09-06)

| Configuration | Result |
|---|---|
| x86-64 debug, `-smp 4` and `-smp 1` | `SELFTEST: PASS (124 tests)`, `LINUXTEST: PASS`, `lxinterp: ok`, `lxdyn: ok`, the seven `lxsig` lines, `SHTEST: PASS` |
| AArch64 debug | the same set (the Linux section now runs there) |
| release, both | PASS (Linux programs present, not run) |

## Results as of Phase 11

| Configuration | Result |
|---|---|
| debug, `-smp 4` | PASS: `SELFTEST: PASS (62 tests)` including `linux-elf`, `hello from linux abi`, `LINUXTEST: PASS`, `SHTEST: PASS` |
| debug with `MUSL_GCC` (docker wrapper) | PASS including `hello from musl on Linux x86_64 (pid N)` |
| debug, `QEMU_SMP=1` | PASS |
| release | PASS (Linux programs present, not run) |
| `make test-crash`, `MODULE_SIG_ENFORCE=0` | PASS |
| `make host-test` | `linux ok` |
| `make analyze`, `make reproducible` | clean, `reproducible: yes` |

## Gaps

No futex contention test beyond two waiters and one requeue. No
hostile-pointer sweep over the Linux table comparable to `init
--selftest`. No test of `brk`'s 1 GiB cap, of `MAP_FIXED` over the heap,
of a blocking TCP transfer through the Linux socket calls (the
non-blocking handshake and one datagram are covered), of `wait4` on a
real child (Linux processes cannot spawn yet), of two Linux processes
alternating their thread pointers, of a handler interrupted by a second
signal, of `SIGKILL` against a thread blocked in `futex_wait`, or of
`AVX` state across a handler. `hello_musl` is the only foreign-toolchain
binary and runs only where `musl-gcc` exists (CI, x86-64); a larger
program (a shell, `busybox`) is the natural next fixture once
`fork`/`execve` exist. `lxinterp` applies `RELATIVE` relocations only;
no real `ld.so` has been run.
