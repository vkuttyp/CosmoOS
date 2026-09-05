# Processes and User Mode: Testing

## Kernel self-tests (`kernel/process/proctest.c`)

Run from thread 0 after the SMP tests (and, since Phase 7, last in the
table so `init --selftest` finds the cosmofs the filesystem tests leave
behind); each leaves the process count where it found it.

### `objects`

| Step | Proves |
|---|---|
| `kobject_init` → refcount 1; get → 2; put → 1, release not called; put → release called once | reference counting and single release |
| `handle_install` returns 0, refcount +1, `handle_table_count` 1 | install takes a reference |
| lookup with READ returns the object at refcount 3; put restores | lookup takes a reference |
| lookup with WRITE on a READ-only slot → NULL; empty slot, -1, 64 → NULL | rights and bounds (P17) |
| `handle_install_at(3)` ok, again `-EBUSY`, slot 99 `-EBADF`; close 3 ok, again `-EBADF` | explicit slots and double close |
| install until `-EMFILE` after 63 more; count 64 | table is full at 64 |
| `handle_table_destroy` → count 0, refcount back to 1; final put releases | destroy drops every reference |

### `elf`

Crafted 120-byte images built by `make_elf` (one `PT_LOAD`, memsz 4096):

| Image | Expected |
|---|---|
| RX segment at `0x400000`, entry `0x400010` | 0; one segment `0x400000`+4096, entry recorded |
| RWX segment | `-ENOEXEC`, "PT_LOAD is writable and executable (W^X)" |
| RW segment, entry inside it | `-ENOEXEC`, "entry point is not inside an executable segment" |
| RX segment at `0x1000` | `-ENOEXEC` (below the user window) |
| `p_filesz` 100000 | `-ENOEXEC` (file bytes outside the file) |
| first byte `X` | `-ENOEXEC` (bad magic) |
| `e_type` = `ET_DYN` | `-ENOEXEC` |
| size 10 | `-ENOEXEC` (shorter than the header) |

### `process-reject`

128 zero bytes passed to `process_create_from_elf` → `-ENOEXEC` and no
process object; the log shows `rejected: bad ELF magic`.

### `process-spawn` (Phase 9)

`path_normalize`: `/` + `usr/bin` → `/usr/bin`; `/usr/bin` + `..` →
`/usr`; `/usr/bin` + `../../..` → `/`; `/a` + `./b//c/./d` → `/a/b/c/d`;
`/a/b` + `/x/../y` → `/y`; `/` + `.` → `/`; a 4-byte output buffer →
`-ENAMETOOLONG`. Then two kills of the archive's `init` created by the
kernel: `init --block` (blocked in a console read) is killed with
`SIGTERM` after 50 ms and must exit with 143 within 2 s; `init --spin`
(a CPU-bound loop, killed with `SIGKILL`) must exit with 137, which
proves the return-to-user delivery point. Both are checked not to have
exited before the kill.

### `process-user`

Runs the archive's `init` as `init --selftest` and requires exit status 0
within 5 s, then waits for the process count to return to its baseline
(the object is released by the reaper). Skipped with a log line when
the loader found no module. The user program's checks
(`userland/init/init.c`, `selftest()`: `fs_selftest()` for the Phase 7
filesystem calls (`docs/kernel-services/vfs/testing.md`), `net_selftest()`
for the Phase 8 sockets (`docs/kernel-services/network/testing.md`),
`proc_selftest()` for Phase 9, then the Phase 4 checks below) must all
pass for status 0. Since Phase 9 init is built on libc, so most checks go
through the library (`docs/libc/testing.md`); the Phase 4 checks still
use the raw wrappers to test kernel error codes exactly.

`proc_selftest` (Phase 9):

- **pipes and dup**: `pipe` gives two distinct handles ≥ 3; `write` 3
  bytes, `fstat` on the read end is a FIFO of size 3, `read` returns
  them; `read` on the write end and `write` on the read end → `EBADF`;
  `dup` of the write end keeps it alive after the original closes;
  `dup2(d, 40)` → 40 and writable; `dup2(d, 64)` → `EINVAL`; after the
  last write end closes the pending two bytes are read and then 0 (EOF);
  a pipe whose read end is closed → `EPIPE` on write.
- **console**: `fstat(0)` is a character device; `isatty(0)`; `fstat(7)`
  → `EBADF`.
- **spawn and wait**: `echo spawned child` with the pipe's write end as
  the child's handle 1 → the read end yields `spawned child\n` then EOF
  (after the parent closed its copy); `waitpid(pid)` → status 0; a
  second `waitpid(pid)` → `ECHILD`. `sh -c "cd /tmp && pwd && exit 7"`
  prints `/tmp` and exits 7; the parent's cwd is still `/`.
- **kill**: `cat` with the pipe's read end as its handle 0 blocks;
  `waitpid(WNOHANG)` → 0; `kill(pid, SIGKILL)` → 0; `waitpid` → 137;
  `kill(999999)` → `ESRCH`; `kill(pid, 0)` → `EINVAL`.
- **hostile spawn**: a map naming parent handle 63 (free) → `EBADF`;
  two entries for child slot 0 → `EINVAL`; `/etc/rc` (mode 0644) and
  `/bin` (a directory) → `EACCES`; `/bin/nothere` → `ENOENT`;
  `spawnvp("nothere")` → `ENOENT`; an empty `argv` → `EINVAL`;
  `waitpid(-1)` with no children → `ECHILD`.
- **working directory**: `chdir("/tmp")`, `mkdir("cwdtest")` creates
  `/tmp/cwdtest`; `chdir("cwdtest/../cwdtest/.")` → `/tmp/cwdtest`;
  `chdir("..")` → `/tmp`; `chdir("/boot/init")` → `ENOTDIR`;
  `chdir("/nope")` → `ENOENT`; `getcwd` into 4 bytes → `ERANGE`;
  `rmdir("cwdtest")`, `chdir("/")`.
- **introspection**: `getppid() == 0`; `procinfo` lists its own pid with
  name `init` and one thread; `klog_read` returns more than 100 bytes
  containing a log line; `sysctl_get("kernel.name")` is `CosmoOS`
  (length 7), `hw.ncpu` ≥ 1, `sysctl.names` contains `kernel.version`,
  `no.such` → `ENOENT`, a 3-byte buffer gets a truncated value and the
  full length.
- **libc**: `malloc(100000)` written, `realloc` to 200000 keeps the
  bytes; a mixed `snprintf` format gives length 40 and the expected
  text; `strtol`/`strtoul`; `setenv`/`getenv`.

The Phase 4 checks (raw wrappers), unchanged except where noted:

- **write**: 19 bytes to handle 1 → 19; zero length → 0; handle 7
  (unopened), handle 0 (stdin, no WRITE right), handle -1 → `-EBADF`;
  buffer at `0xffffffff80000000` (kernel), `0x10` (below the window),
  `0x00007FFFFFFFF000` (top of the window), `0x0000600000000000`
  (unmapped) → `-EFAULT`; length `(size_t)-1` → `-EFAULT`.
- **read**: handle 1 (no READ right) → `-EBADF`; kernel-pointer buffer
  → `-EFAULT`; a zero-length read of handle 0 → 0 without blocking (a
  real read would wait for a typed line).
- **pid/yield/clock/sleep**: `getpid` > 0; `yield` → 0; a 5 ms sleep
  advances the clock by at least 5 ms and less than 200 ms; a sleep
  over one hour → `-EINVAL`.
- **mmap/munmap**: 3 pages anonymous RW → address > 0, first and last
  words read as 0 (demand-zero), written and read back; `write` of
  length 0 from it → 0; `munmap` → 0; second `munmap` → `-EINVAL`;
  length 0 and 4097 → `-EINVAL`; RWX → `-EINVAL`; non-anonymous →
  `-EINVAL`; `MAP_FIXED` at `0x10` → `-EINVAL`; `MAP_FIXED` at
  `0x0000200000000000` → that address, written; `MAP_FIXED` on the
  same page → `-EEXIST`; `munmap` of it → 0; `munmap(0x10)` → `-EINVAL`.
  Milestone 5: four RW pages, `munmap` of the middle two → 0 and both
  ends still hold their bytes; `munmap` of the gone page → `-EINVAL`;
  `munmap` of the whole range across the hole → `-EINVAL` and the ends
  unchanged (strict); the two ends unmapped one by one → 0; a
  `PROT_NONE` mapping succeeds and `log` from it is `-EFAULT`.
- **log/close/unknown**: `log` of 20 bytes → 0; kernel pointer →
  `-EFAULT`; length 4096 → `-EINVAL`; `close(7)` → `-EBADF`; numbers
  `SYS_COUNT`, 999999, and -1 → `-ENOSYS`.
- **stack**: a 64 KiB local array is written at both ends through
  lazily populated stack pages.
- **last**: `close(2)` → 0 then `write(2)` → `-EBADF` (after this no
  failure could be reported on handle 2).

### `process-fault`

Runs `init --crash`, which prints a line and writes to address 0;
requires exit status `COSMO_EXIT_FAULT` (139). The log shows
`fault: user write at 0x0000000000000000 (not present); terminating`.

### `process-efault`, `process-protnone`, `process-oom` (milestone 5)

`init --probe efault` maps a `PROT_NONE` page, a read-only page and
three RW pages with the middle one unmapped, then: `write` from the
`PROT_NONE` page and `log` of it are `-EFAULT`; `read` from a pipe into
the read-only page is `-EFAULT` (a protection fault on a present page
inside the kernel's copy); `read` into 16 bytes that straddle the hole
is `-EFAULT`; the same `read` into the surviving page returns the data;
`stat` into the `PROT_NONE` page and `stat` of a path whose bytes run
off the end of a mapped page without a NUL are `-EFAULT`; `write` from
the read-only page works. Exit 0 (a nonzero exit names the failing
step) and `vm_stats.fixups` rose (six on both architectures).
`init --probe none-touch` writes to a `PROT_NONE` page: status 139.
`process-oom` (fault-injection builds) arms `demand-copy` for one hit
and runs `init --probe oom-copy`, which reads from a pipe into a fresh
page: the kernel's copy takes the demand fault, the frame allocation is
made to fail, the read is `-EFAULT`, exit 0, one hit counted; then
`demand-page` for one hit and `init --probe oom-touch`, whose first
write to a fresh page is fatal (139). Both were kernel panics before.

### `process-rlimit` (milestone 6)

Runs `init --probe rlimit-root` (exit 0), `rlimit-unpriv` (exit 0) and
`mem-limit` (status 139); the probes are specified in
`docs/kernel/security/testing.md`.

## Harness markers (`tests/boot/run_boot_test.py`)

Always required: `init: CosmoOS userland, pid N`, `CosmoOS userland
ready`, `init: rc exited with status 0`, `interactive-ok` (typed by the
shell harness, `docs/userland/testing.md`), `init: shell exited with
status 0`, `[ INFO] init exited with status 0`, `[ INFO] boot complete`.
Required whenever any `SELFTEST:` line appears (debug builds):
`USERTEST: PASS` and `SHTEST: PASS`. Release builds disable self-tests
and run only the real `init`, so those two are not demanded there; the
interactive harness runs in every normal build.

## Measured results (2026-09-05, QEMU TCG, Apple Silicon host)

| Configuration | Result |
|---|---|
| debug, `-smp 4` | `SELFTEST: PASS (61 tests)`, `USERTEST: PASS`, `SHTEST: PASS`, the shell harness completes, init exits 0, about 10 s |
| debug, `-smp 1` | PASS |
| release, `-smp 4` | PASS (init exits 0 after the interactive session) |
| `make test-crash` | PASS (kernel-side fault report unchanged) |
| `make host-test` | 6 binaries pass |
| `make analyze` | clean |
| `make reproducible` | byte-identical |

Every user ELF (`out/x86_64-debug/userland/*.elf`, packed into the boot
archive as `init`, `bin/*`, `sbin/*`) has three `PT_LOAD` segments
(r-x, r--, rw-) and a non-executable `PT_GNU_STACK`.

Milestone 6 (2026-09-05): `SELFTEST: PASS (103 tests)` on both
architectures with `process-rlimit` at about 20 ms. Milestone 5: 100
tests on x86-64 with 4 and 1 CPUs and on AArch64; `process-efault` 8 ms / 18 ms,
`process-protnone` 9 / 27 ms, `process-oom` 16 / 27 ms (x86-64 /
AArch64).

## Gaps and planned tests

- `elf_validate` compiles on the host (`ELF_HOST_TEST`) but no host
  test drives it yet; a `tests/host/test_elf.c` with the crafted cases
  and a fuzz loop over random mutations of the init image is planned.
- No fuzzing of the syscall surface from user space; a user-side
  fuzzer for argument combinations is planned.
- No test kills a process blocked in a socket wait or in `sleep`; no
  test creates an orphan under the real init; no test exceeds
  `COSMO_ARG_MAX`.
- No concurrency tests for the handle table (single thread per
  process today).
- SMAP (`stac`/`clac`) is untested on `qemu64`; a run with
  `-cpu max` is planned in CI once TCG's SMAP emulation is confirmed.
- Timing bounds in the user test (5 ms sleep, 200 ms ceiling) are
  loose for TCG.
