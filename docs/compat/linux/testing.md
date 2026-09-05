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

Built by `tests/linux/linux.mk` with the user compiler flags and
`userland/user.ld` but **without** `crt0.o` or `libc.a`, so they carry
no CosmoOS note; `lxabi.h` supplies `_start` (reads `argc`/`argv` from
the initial stack, calls `main`, `exit_group`s) and raw `syscall`
wrappers `sc0..sc6` with the Linux register convention. Numbers and
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
| identity, uname, time | `getpid` > 0, `gettid == getpid`, `getppid` > 0, uid/euid 0; `uname` gives `Linux`/`x86_64`; `clock_gettime(MONOTONIC)`, a 10 ms `nanosleep`, `clock_gettime(REALTIME)` advanced; clock 99 `-EINVAL` |
| thread pointer | `arch_prctl(ARCH_SET_FS, tcb)`, `%fs:0` reads the word, `ARCH_GET_FS` returns the base, `ARCH_SET_GS` `-EINVAL`; a sleep, then `%fs:0` again |
| brk, mmap | `brk(0)`, grow 8 KiB and write both pages, shrink back, a request below the start returns the old break; anonymous `mmap` of 8 KiB written, `mprotect` to read-only, `munmap`; a file mapping `-ENODEV`; RWX `-EINVAL`; `MAP_FIXED` at a free page, `MAP_FIXED` again on it replaces (reads 0), then `munmap`. Milestone 5: eight pages, `mprotect` of the middle two to read-only (page 0 still writable, the two readable with contents), to `PROT_NONE` (`write` from it `-EFAULT`), back to RW (the byte survived); `munmap` of one page, `mprotect` across the hole `-ENOMEM`, `munmap` of the whole range 0; `brk` grow, shrink to one page, regrow larger: kept page, zero page, new byte |
| files | `openat(AT_FDCWD, "/tmp/lxtest.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644)`, `write`, `writev` of two iovecs, `fstat` (`S_IFREG`, size 22), reopen `O_RDONLY`, `pread64` at 6, `lseek` 0, `readv` into two buffers, `read` at EOF returns 0, `close`, second `close` `-EBADF`; `newfstatat` size, `stat` of a missing path `-ENOENT`, `/tmp` is `S_IFDIR`, fd 0 is `S_IFCHR`, `ioctl(1, TCGETS)` `-ENOTTY`, `access`; `mkdirat`, `rename` into the directory, `getdents64` sees `.`, `..` and `moved` (`d_type` 8) with 8-aligned records of at least 24 bytes, a second `getdents64` returns 0; `unlinkat`, `unlinkat(AT_REMOVEDIR)`; `chdir("/tmp")`, `getcwd` returns 5 (NUL included) and `/tmp`; `chdir("/")`; `umask` 022 |
| pipes, dup, fcntl | `pipe2(O_CLOEXEC)`, write 3 bytes, read them back; `dup`, `dup3` to 40, `dup3(fd, fd)` `-EINVAL`; `F_GETFL` on the write end is `O_WRONLY`; `F_DUPFD` from 50 lands at or above 50; close every write end, `read(40)` returns 0 (EOF); `fstat(40)` is `S_IFIFO` |
| signals, wait, kill | `rt_sigaction(2)` stores a handler at `0x400000` and reads it back; `rt_sigaction(9)` `-EINVAL`; `rt_sigprocmask` block and read back; `wait4(-1)` `-ECHILD`; `kill(999999, 15)` `-ESRCH`; `kill(self, 0)` 0; `execve` and `fork` `-ENOSYS`; `sched_yield` 0 |
| futex | `FUTEX_WAIT|PRIVATE` with the wrong value `-EAGAIN`; `FUTEX_WAIT` with a 20 ms timeout `-ETIMEDOUT`; `FUTEX_WAKE` returns 0; operation 99 `-ENOSYS` |
| random, sockets | `getrandom(32)` returns 32; a UDP socket bound to 127.0.0.1 sends `ping` to itself and `recvfrom` receives it with the sender's address; `getsockname` into a 4-byte buffer reports 16 and leaves a canary intact; `fstat` on the socket is `S_IFSOCK`; `setsockopt(SOL_SOCKET, SO_REUSEADDR)` 0; `close`; `socket(AF_UNIX)` `-EAFNOSUPPORT` |
| unknown numbers | 510 (in the table, unimplemented) and 9999 (beyond it) both `-ENOSYS` |

`hello_musl` (`tests/linux/hello_musl.c`): `uname`, `malloc`,
`strcpy`, `printf`, `free`, and prints `hello from musl on Linux x86_64
(pid N)`. Under the hood musl's start-up performs `arch_prctl`,
`set_tid_address`, `rt_sigprocmask`, `brk`, `mmap`, `ioctl(TCGETS)`
(gets `-ENOTTY`: the output is fully buffered and flushed at exit),
`writev`, `exit_group`: the sequence a real Linux libc needs.

## `/etc/rc.test`, Linux section

After the package section: `/boot/tests/linux/lxhello || FAILS=1`,
`/boot/tests/linux/lxtest || FAILS=1`, then `hello_musl` only if the
file exists (`ls ... 2> /tmp/.musl.probe && /boot/tests/linux/hello_musl`).
A failing Linux program therefore also fails `SHTEST`.

The harness requires in self-test builds (`LINUXTEST_MARKERS`):

| Marker | From |
|---|---|
| `^hello from linux abi$` | `lxhello` |
| `^LINUXTEST: PASS$` | `lxtest` |
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

No two-thread futex test and no contention test (a single-threaded
process cannot exercise a real wake). No hostile-pointer sweep over the
Linux table comparable to `init --selftest`. No test of `brk`'s 1 GiB
cap, of `MAP_FIXED` over the heap, of TCP through the Linux socket
calls (only UDP loopback), of `wait4` on a real child (Linux processes
cannot spawn yet), or of two Linux processes alternating their `%fs`
bases. `hello_musl` is the only foreign-toolchain binary; a larger
program (a shell, `busybox`) is the natural next fixture once file
mappings and `fork`/`execve` exist.
