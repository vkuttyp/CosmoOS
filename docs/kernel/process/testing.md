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
the loader found no module.

**One `SELFTEST` line, eleven sections (ten when this was written; the file-regions unit added `mmap`), and each reports its own time.**
`selftest()` drives a table of sections rather than calling them in a
row, timing each and printing

```text
USERTEST: section svc 1386 ms
USERTEST: sections 10, total 3610 ms
```

so a slow section is named instead of the whole suite (invariant
**F13**, `docs/audit/next-subsystem-usertest-sections.md`). The table
is the point: the driver is the only caller, so a section cannot be
added without a line. The `usertest: ... ok` prose lines the sections
print are unchanged and are **not** boundaries -- two of them are
followed by further checks, and `trap_selftest` is an empty function on
aarch64 that prints none at all while still reporting `0 ms`. The user program's checks
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
- **a wake inside a system call preempts** (the wake-preempt unit): the
  read of sysctl `debug.preempt_probe` creates a priority-16 kernel
  thread on the caller's CPU, wakes it, and reports what it saw of the
  caller's next statement; `saw=0` on a debug kernel, `ENOENT` on a
  release one (`docs/kernel/scheduler/testing.md`).
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
- **the working directory's name** (P32, the cwd-name unit): six
  `chdir`s through `/tmp/ncnl` (an absolute link to `/tmp/ncn/deep`) and
  `/tmp/ncnr` (a relative one) -- the link, `..`, `/tmp`, the relative
  link, `/tmp/ncnl/..`, `deep/../../ncnl` -- each followed by `getcwd`
  against the expected path and `stat(".")` against `stat(getcwd())`;
  a child spawned with `cwd` `tmp/ncnr` runs `init --probe
  cwd-is:/tmp/ncn/deep`, which checks its own name the same way;
  `lstat("/proc/self")` is a link and `readlink` answers the pid;
  `chdir("/proc/self")` publishes `/proc/<pid>`, a child inheriting it
  and a child spawned with `cwd` `/proc/self` both get the parent's
  `/proc/<pid>` (checked by `cwd-is:`), and `chdir("..")` from there is
  `/proc`.
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
`docs/kernel/security/testing.md`. `process-nproc` (there too) has two
kernel threads create sixteen `init --probe hold` children of one uid
under a limit of four while a sampler watches the count.

### `process-user` additions (milestone 10)

The user-mode trap tests now expect each exception's own signal:
`ud` 132 (`SIGILL`), `gp` 139 (`SIGSEGV`), `de` 136 (`SIGFPE`), `db` 133
(`SIGTRAP`); the native `kill` goes through the signal core (a
default-ignore signal such as `SIGCHLD` no longer terminates). The
Linux-side coverage of threads, signals, frames and the return guards is
in `docs/compat/linux/testing.md` (`lxtest`, `lxsig`, `lxdyn`).

### `process-reaped`

Creates a process from the boot archive, checks `process_lookup` finds
it, waits for it to exit -- it has no parent, so exiting reaps it --
and requires `process_lookup` to return NULL while the test is still
holding the creation reference, so the object is provably still in the
table. The user-mode form of this check (`kill(pid, 0)` right after
`waitpid`, in `init --selftest`) samples the same window but races the
reaper; this one does not.

### The native signal ABI, sessions and the terminal

Seven self-tests, each a user program the kernel runs and whose exit
status is the number of the check it failed (the kernel side logs that
number: the program that knows the detail is gone by then).

- **`signal-native`** -- `init --probe signal`. Installs a handler,
  loads a pattern into all sixteen vector registers, `raise`s the signal
  and finds the pattern intact afterwards; the handler tramples the
  vector registers and four scratch general registers on purpose. Then
  the siginfo (`SI_USER`, the sender's pid), the mask (blocked inside
  its own handler, unblocked after), `SA_NODEFER`, `SA_RESETHAND` (the
  action reads back as `SIG_DFL`), and `sigaction(SIGKILL, ...)` being
  `-EINVAL`.
- **`signal-async`** -- `init --probe signal-async`. The same at the
  other delivery point: a child (`signal-poke`) sleeps 20 ms and signals
  its parent, which is spinning in hand-written assembly holding
  sentinels in four registers no calling convention preserves. The loop
  reports how many times it went round, so a signal that arrived before
  the loop started fails the probe instead of passing it having proved
  nothing. It is bounded, so a signal that never arrives fails rather
  than hangs. The vector pattern is loaded *after* the spawn, because
  the library's string handling uses vector registers and would
  otherwise have overwritten it -- which is how this test first failed.
- **`signal-mask`** -- blocked, pending, delivered at the unblock; the
  mask coming back *out of the frame* (a second signal is blocked before
  the handler runs, blocked inside it, and blocked again after it
  returns -- a frame carrying no mask would leave it unblocked, and
  nothing else in these tests notices that); `SIGKILL` and `SIGSTOP`
  refusing to be blocked; `SIG_IGN` discarding.
- **`signal-fault`** -- a `SIGSEGV` handler is told the faulting
  address, maps a page there, and the store that faulted is run again
  and lands.
- **`signal-group`** -- four sleepers, three in one group and one
  outside; `kill(-pgid, SIGTERM)` ends exactly the three and the
  outsider runs to completion. Then an empty group as a target
  (`-ESRCH`), a nonexistent pid to `setpgid` (`-ESRCH`), an empty group
  to join (`-EPERM`), a session leader trying to change group
  (`-EPERM`), and a child moved into a group of its own.
- **`signal-setsid`** -- run in a child, because the process the kernel
  starts for a probe already leads its own group: the child sees its
  parent's group, `setsid` succeeds once, changes both ids, and is
  `-EPERM` the second time. Inheritance is checked *from the child*
  (`getpgid(0) == getpgid(getppid())`) rather than by the parent looking
  at the child, which races the child's own `setsid` -- and lost, on one
  architecture's CI runner and not the other's.
- **`tty-intr`** -- two phases, driven from both ends. A user process claims the
  console and waits; the test polls `tty_foreground_pgrp` until it is
  the child's group, runs a second process from another session which
  must be refused both `tcsetpgrp` (`-EPERM`) and `tcgetpgrp`
  (`-ENOTTY`), types `abc` and then `^C`, and requires the child to exit
  130 with no line committed (the partial line is thrown away) and the
  terminal released once its session leader is gone. The second phase
  writes `^\` and `^C` as one batch at a process that catches the
  interrupt and leaves the quit fatal: it must die of the quit, which a
  line discipline that kept only the last signal of a batch would not
  manage.

### Job control

- **`signal-stop`** -- a child stops itself, the parent sees
  `WIFSTOPPED` with the right signal and *not* a second time without a
  second stop, continues it, sees `WIFCONTINUED`, and collects the exit
  status the child only reaches after being resumed.
- **`signal-stop-kill`** -- a stopped process is killed and dies.
- **`signal-stop-mask`** -- `SIGSTOP` cannot be caught, ignored or
  blocked.
- **`signal-stop-restart`** -- driven from both ends, because the stop
  has to land *inside* the call under test: a reader in a background
  group is stopped by its own `read` (`SIGTTIN`), the probe then hands
  it the terminal and continues it, and the kernel side types a line the
  restarted read must return. Two earlier versions aimed a stop at a
  sleeping child from the parent and both passed with the restart
  deliberately broken -- the first because the stop landed between the
  handshake and the sleep, the second because it measured elapsed time,
  which includes however long the process sat parked and so cannot tell
  a failed call from a restarted one.
- **`signal-stop-late`** -- a stop and a continue sent back to back
  before the child has run at all, six times: it must end up running,
  and no stop may be reported after the continue. Bounded, because the
  failure it covers is a hang.
- **`tty-stop`** -- `^Z` at the terminal in the shape a shell uses: the
  probe leads the session and holds the terminal, the *job* is a child
  in a group of its own (which is also what makes that group
  non-orphaned), and the kernel types the keystroke. The job must stop
  rather than die, and then continue and be killable.
- **`tty-ttin`** -- all three ways the rule can go, in one probe. A
  child in its own group with a live parent is stopped with `SIGTTIN`;
  a real orphan -- a grandchild whose parent exits, reporting through a
  pipe because it cannot be waited for -- gets `-EIO` instead, and so
  does a reader that blocks `SIGTTIN`, because no stop can follow for
  it either. Removing
  the orphan rule wedges the boot rather than failing it, which is
  precisely the failure the rule exists to prevent.

These three console-driving tests run **before `hid-arm`**: they type
control characters at the console, and `^C` throws away whatever line is
under edit, so running them after the keyboard harness has started
typing eats its line.

The interactive boot test (`tests/boot/shelltest.py`) covers the same
path end to end: it runs `sleep 5`, waits half a second, sends a bare
`0x03` byte, and requires the echoed `^C` and the next prompt **within
three seconds** of the keystroke. It then runs `sleep 30`, types `^Z`,
and requires `[1]+  Stopped`, `jobs` to list it, `fg` to bring it back,
and a `^C` to end it -- the same pid throughout, which is what says it
was stopped and continued rather than restarted -- followed by
`sleep 1 &` and a `jobs` that shows it running in the background. The bound is the test: `sleep 5`
reaches a prompt on its own eventually, so without it a run in which the
`^C` did nothing still ends with every pattern matched -- which is what
happened to the first version of this check, and what reintroducing the
bug found. `sleep` exits 130 in the log.

### `elf` (milestone 10)

An `ET_DYN` image with a segment at 0 validates with `is_dyn`, relative
addresses and no interpreter; `elf_rebase` to `USER_PIE_BASE` moves the
segment, the entry and `hi`; an `ET_DYN` segment beyond the window's span
is refused.

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

Milestone 6 (2026-09-05): `SELFTEST: PASS (105 tests)` on both
architectures with `process-rlimit` at about 20 ms and `process-nproc`
(two concurrent spawners against one limit) at about 100 ms. Milestone 5: 100
tests on x86-64 with 4 and 1 CPUs and on AArch64; `process-efault` 8 ms / 18 ms,
`process-protnone` 9 / 27 ms, `process-oom` 16 / 27 ms (x86-64 /
AArch64).

Milestone 10 (2026-09-06): `SELFTEST: PASS (124 tests)` on both
architectures; `process-user` about 800 ms; the Linux programs add the
thread, signal and PIE coverage on both machines.

2026-09-19, the first per-section measurement: `process-user` is
3711 ms (x86-64) and 3938 ms (aarch64), of which the sections account
for 3610 / 3827 ms and the rest is the spawn and teardown. The order is
the same on both, and it is not the order the check counts suggest --
`svc` 1386 / 1489 ms from **34** checks and nine sleeps waiting on
service state, `proc` 912 / 939 ms from 235, `fpu` 664 / 665 ms from 4
(it spawns two partners), then `fsctl`, `fs`, `trap`, `priv`, `net`,
`proc-fs`, `syscalls`, and since the file-regions unit `mmap` (264 / 300 ms then; 1186 / 1468 ms since the shared-futex unit added its two-process waits, three deliberate timeouts and the futex bench). Time here is spawning and waiting, not
checking.

## Gaps and planned tests

- `elf_validate` compiles on the host (`ELF_HOST_TEST`) but no host
  test drives it yet; a `tests/host/test_elf.c` with the crafted cases
  and a fuzz loop over random mutations of the init image is planned.
- No fuzzing of the syscall surface from user space; a user-side
  fuzzer for argument combinations is planned.
- No test kills a process blocked in a socket wait or in `sleep`; no
  test creates an orphan under the real init; no test exceeds
  `COSMO_ARG_MAX`.
- No concurrency tests for the handle table from several threads of one
  process. Native threads make this reachable from a CosmoOS program for
  the first time (`thrtest`), and it is still not tested: the handle table
  under two threads sharing it is worth a unit of its own.
- SMAP (`stac`/`clac`) is untested on `qemu64`; a run with
  `-cpu max` is planned in CI once TCG's SMAP emulation is confirmed.
- Timing bounds in the user test (5 ms sleep, 200 ms ceiling) are
  loose for TCG.

## Native threads (`thrtest`, audit unit "native threads and a futex")

A kernel self-test cannot create a **user** thread, so this unit's proof is
a native userland program: `tests/native/thrtest` in the boot archive
(`SELFTEST` builds only), run from `/etc/rc.test` -- **before** its hypervisor section, which asks
for guests of 16 MiB and, where a Linux `Image` exists, 256 MiB: a test
about threads should not be hostage to what the frame allocator looks like
afterwards, and CI refused this test's second thread stack with `-ENOMEM`
when it ran last. Its own threads ask for 16 KB stacks rather than the
64 KB default, which is what they need -- gated by
`THREADTEST: PASS` in `run_boot_test.py`'s own `THREAD_MARKERS` group --
its own group and not the hypervisor's, which are gated on a backend,
because native threads run on every build. Each step prints its number
before running: two of the bug-proofs below kill the process outright, and
"no output" would say a step failed without saying which.

(1) A thread runs and is joined: its id is neither zero nor the pid, the
work it did is visible afterwards, and `join` returns what the function
returned. (2) **The entry conditions the ABI promises**, captured by a
*naked* stub that records the stack pointer and tail-jumps to the C body,
because reading `rsp` inside the C function measures the frame and not the
entry -- an earlier version did that and passed on AArch64 by luck while
x86-64 caught it. `rsp % 16 == 8` on x86-64, `sp % 16 == 0` on AArch64, the
argument in the first argument register, and a 16-byte-aligned vector store
that faults if the alignment is wrong rather than merely unusual. (3) Two
threads make progress against each other under a bound: the assertion is
*progress*, so a single-CPU run passes too (preemption suffices) and
parallelism makes it fast rather than making it pass. (4) The futex answers
`-EAGAIN` for a word that does not hold the expected value, `-ETIMEDOUT`
for a timeout, zero woken when nobody waits, `-EINVAL` for an unaligned
word and `-EFAULT` for one outside the caller's space. (5) **`clear_tid` is a join**, in two parts. First
deterministically: a child that waits to be told to stop cannot have
exited, so the word must still hold its tid. Asserting that against a
child which returns at once is simply wrong -- on a machine with more
parallelism the child finishes, the kernel zeroes the word, and the parent
reads 0 -- and CI proved it after five local runs had not. Then a hundred
times: the word holds the child's tid the instant
`thread_create` returns -- written by the kernel before the child could
run, so it cannot be a stale value the caller wrote -- and is zero after
the join, with every tenth iteration pausing so the child finishes *first*.
There the word may legitimately read the tid *or* zero depending on who
won, so what the loop proves is the **join**: a stale tid, written after
the child had already zeroed it, is what hangs it. One attempt is not
enough either way: the property is a race over a few microseconds, and the
bug-proof for the ordering passed against a single attempt.
(6) Exit semantics: a worker's `thread_exit` leaves the process running and
its joiner returns. (7) The **signal mask is per-thread**: a worker blocks
`SIGUSR1`, the main thread does not, and the signal sent to the process is
handled by the thread that does not block it. (8) Every argument check --
a request outside the caller's space, an unmapped `stack_top`, an unaligned
one, an unknown flag, an unaligned `clear_tid` -- using a page this test
maps and frees, so the address is unmapped *by construction* rather than by
assumption (0x400000 is the program's own load address, which an earlier
version discovered by creating a thread whose stack was its own text).
It also checks that **a refused start leaves a joinable handle**: a caller
keeps its handles in one array, checks each start and then joins, so a
start that fails must not leave the handle as the caller's stack found it.
The handle is poisoned with `0xa5` and the start is refused for certain by
asking for half the address space; the join must answer `-EINVAL` rather
than wait on the poison. (9) A mutex under two threads loses no update, and `trylock` fails on a
held one. (10) **The filter, observed from outside**: a denied call does not
return an error, it kills the process with `SIGSYS` (status 159), so the
filtered process cannot report on itself -- one child calls a denied
`thread_create` and must die that way, and another, under a filter that
denies *everything*, must still exit cleanly with its own status through
`thread_exit`. (11) **The allocator and stdio under three threads at once** -- whose
creates **retry**, because a thread's stack is a mapping and a mapping can
be refused on a machine under pressure: CI's aarch64 runner has refused
one twice where this machine never has, and the step's subject is the
allocator and stdio under concurrency rather than the proposition that a
create always succeeds. Every refusal is printed with its errno. Retrying
costs the step the very overlap it exists to measure, though -- a sleep
between sequential retries lets an earlier worker finish before the last
one exists -- so the workers wait on a **barrier**: each announces
itself and then sleeps on the release word with a futex, and the main
thread, having finished its creates, waits for all of them to arrive
before storing the release and waking them.

Three properties make that barrier trustworthy, and each was a separate
mistake first. The release is **unconditional**, so no worker is left
parked however the creates went. Both waits are **bounded**, so neither
side can hang the boot -- but a bound that expires silently is the flaw
the barrier exists to remove, since a worker that gave up waiting would
allocate alone while every assertion still held. So both bounds are
**observable**: the main thread asserts that every started worker
arrived, and a worker whose wait expires says so and counts itself, which
the step asserts never happened. And both bounds are **durations derived
from the retry budget** -- `HEAP_BARRIER_NS`, computed from the same
constants the retry loop uses -- rather than yield counts, because a
worker begins waiting the moment it is created while main may still spend
twenty attempts and twenty milliseconds apiece on the workers after it. A
count would have expired inside a successful retry sequence and reported
the first worker late during exactly the refusal the retry exists to
tolerate.

The join afterwards joins exactly the slots that started -- counting
them instead would join a refused slot and one thread twice, leaving a
real thread running. Each thread
allocates, fills its block with its own byte, verifies every byte of it,
reallocates, frees, and prints as it goes. This is the step that would
otherwise find out the hard way what an unlocked free list does -- a lost
or shared block shows up as a wrong byte rather than only as a crash --
and the whole lines from three threads in the log are stdio's side of it.
It also calls `fflush(NULL)` and `fflush(stdout)`, because nothing did
until that locking was found to deadlock the first form against its own
lock.
(12) **`SYS_set_tls`'s contract**: unaligned is `-EINVAL`, below or at the
top of the user range is `-EFAULT`, and a thread can set its pointer, set
it again, and set it to zero. The successful calls run on a *worker*, never
on the main thread, because from this unit onward the main thread's pointer
is libc's own block and a test that took it away would break `errno` for
everything after it. That the call *took effect* is not asserted here:
there is no architecture-independent way to read a thread pointer back --
x86-64 cannot read the FS base without `rdfsbase`, which is the whole
reason libc's block points at itself -- so the effect is what steps 13 to
16 assert, through `errno` itself. A straddling base is not tested because
the alignment makes it unreachable: an earlier version asserted it anyway
and failed by *succeeding*, setting the main thread's pointer.
(13) **Two threads, two `errno`s** -- the point of the unit. One thread
provokes `EBADF` two thousand times, another provokes `ERANGE` through
`strtoll` (a different code from a different call, so the two are not
merely racing inside one syscall path), and the main thread provokes a
third while they run; each reads its own value back every iteration. One
shared `errno` loses this within a few iterations. The counts are counted
rather than `CHECK`ed, because two thousand failing assertions would bury
the log.
(14) **The first thread's `errno` survives a thread's**: main sets
`ERANGE`, a thread sets `EBADF` and exits, and main's is still `ERANGE`.
(15) **A thread libc did not make, installing its own block**: a raw
`SYS_thread_create` with `tls = 0`, whose entry calls `cosmo_tcb_install`
-- refused for a buffer shorter than the prefix and for a misaligned one,
both while the thread still has no block, which is why that function must
not itself touch `errno` -- and only then uses `errno` and the tid cache.
The *un*installed case is deliberately **not** tested: the contract is that
it faults, and a test asserting a fault would be asserting the absence of a
fallback this design does not have.
(16) **The cache and the layout.** `cosmo_thread_id()` agrees with
`SYS_thread_self` and with `getpid` for the first thread, and a created
thread reports the tid its creator was given -- filled on the first call
that asks, so this step is also what proves the lazy fill answers the same
thing an eager one would have. Then the layout, as a
layout: a thread finds its own block through `&errno` -- no new interface
needed, since `errno` is a field of it -- and checks that the block is
*above* a local, which is on the stack by definition. That assertion exists
because the obvious proof does not work: a block placed inside the stack
corrupts `err` silently on AArch64, where the `self` word is unused, and
every assertion that sets `errno` and reads it straight back still passes.
Finally the block is shown to be *freed* with the stack: a write from its
address after the join is `-EFAULT`. The report proposed counting the
address space across a thousand cycles for this; asserting the thing itself
is both shorter and stronger, and a count would have needed a limit to
count against, since `getrlimit` reports the limit and not the usage.
Proved by reintroducing, each failure named by the step that caught it and
the source restored byte-identical every time:

(17) **`__thread` works** (the audit unit "`__thread`, and the TLS image a
program brings with it"). Four threads' worth of per-thread storage: a
`.tdata` variable read back as its initialiser in *every* thread, which is
what distinguishes a copied template from one shared image; a `.tbss`
array zero in a new thread even after an earlier one filled it; an
`_Alignas(64)` variable actually aligned; and `strerror` of an unknown
code from each thread, which is invariant L8's last line. Each thread then
writes its own values and checks they survive while the others write
theirs, which a shared image loses immediately.
**These assertions are what prove the offset formula.** The linker
resolved those addresses relative to the thread pointer, and reading back
an initialiser libc placed is the only way to know that libc and the
linker agree -- reading the ABI documents establishes nothing. Proved by
not copying `.tdata` (every thread reads 0) and by ignoring the template's
alignment (the image shifts 48 bytes and *every* variable is wrong, not
just the over-aligned one, which is broader than predicted).
(18) **A signal is seen, and the mutex comes back**: one waiter, one
signaller, and the waiter returns with its predicate true *and* holding
the mutex again -- which `cosmo_mutex_trylock` returning `-EBUSY` is what
proves, since the predicate was made true before the wait returned and so
cannot fail for a missing re-acquisition
(docs/audit/next-subsystem-condvar.md).

(19) **A signal delivered inside the sleep window is not lost.** The
window between `cosmo_cond_wait`'s read of its sequence number and its
`futex_wait` is a few instructions wide, and no arrangement of threads
reaches it: unlocking the mutex makes a blocked signaller *runnable*, not
*running*. libc's one-shot probe (`__cosmo_cond_probe`) is armed
immediately before the wait and signals from inside the window on the
waiting thread itself. Proved by moving the sequence read after the
unlock -- the step hangs -- and, separately, by running that same bug with
a signaller thread instead of the probe, which **passes**: the evidence
that the seam is what carries this test.

(20) **A broadcast reaches every waiter**, four of four. Its second half
offers one ticket to four waiters and checks exactly one is taken --
*work taken*, not threads woken, because spurious wakeups are permitted
and the number of threads a signal makes runnable is not observable here
without a race.

(21) **Timeouts**: one wait that expires returning `-ETIMEDOUT` after at
least the interval it asked for, bounded by the clock rather than a loop
count, with the mutex retaken on that path too; and one that is signalled
before its deadline returning 0.

(22) **A correct caller survives spurious wakeups**: a waiter whose
predicate never becomes true, broadcast at twenty times, returns from
every wait and goes back round its `while` every time. This is what makes
the loop contract a tested property rather than a comment in a header.

(23)–(30) **The native thread door** (`docs/libc/testing.md`, "The native
thread door", carries the table and the numbers): the herd measured with
eight waiters and one requeueing broadcast; every waiter returning with
the mutex held at 1, at 2, and not held; another holder unlocking inside
the broadcast at either phase of libc's `__cosmo_cond_bcast_probe`; a
concurrent broadcaster answered `-EAGAIN`; a requeued timed wait
expiring on the mutex word; a never-waited condition; the recorded
mutex's page unmapped before a broadcast (a child, so the bug-proof is a
status); and `cosmo_thread_kill` aiming at a running sibling, a sibling
with the signal blocked, this thread by its pid, another process's
thread (`-ESRCH`, untouched), a joined thread (`-ESRCH`, eventually) and
bad signals. They precede the bound step for the reason it gives.

(31) The bound holds: creating threads until `-EAGAIN` stops
at `PROCESS_MAX_THREADS`, every one joins afterwards, and **three** more
creates succeed -- three rather than one, because a join that returned
before the kernel stopped counting its thread left the next create refused
`-EAGAIN`, and a single retry's worth of luck hides that. It is
deliberately the **last** step: it exhausts a resource on purpose, and the
memory those threads held returns as the kernel reaps them rather than the
instant their joins return, so anything run after it is running on a
machine still recovering. An earlier revision put the heap step after this
one and watched `malloc` and `thread_create` be refused for want of
memory, which is this step working rather than a bug -- and which cost two
runs to diagnose only because the step counted three different causes as
one number. Each cause now prints itself.

(32) **A program can find its own program headers**, which is how it finds
its own `PT_TLS`: the auxiliary vector's `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`
are present, `PHENT` is the only size the loader accepts, and -- the part
that matters -- some `PT_LOAD` in those headers covers the address of
`main`, so the vector describes *this* program rather than pointing
somewhere plausible. `PAGESZ` and an unknown tag are checked too, because
a reader landing one word off the vector produces addresses that pass a
null check. It creates no threads, which is why it may follow the bound
step; the `PT_TLS` count it prints is not asserted, so the step that gave
this program a `__thread` variable could be seen to change it from 0 to 1.
Proved twice: with the three tags not passed, and with the reader landing
on envp's terminator instead of past it.

1. The tid written **after** `process_thread_start` -- with the window
   widened by a deliberate sleep, because the real one is about a hundred
   nanoseconds and a hundred contested attempts never lost it: step 1's
   join then waits for ever on a word the child had already zeroed.
2. The x86-64 return address not pushed → step 2 dies of a **#GP** in its
   aligned store, the entry having seen `rsp % 16 == 0`.
3. `SYS_futex_wait` passing zero instead of the value it was given → step
   4's `-ETIMEDOUT` becomes `-EAGAIN`.
4. `SYS_thread_create` not arming `clear_child_tid` → step 1's join hangs.
5. `SYS_thread_exit` calling `process_exit` → the process dies during step
   1, when the first worker finishes.
6. `SYS_thread_exit` removed from `native_always_allowed` → step 10's
   deny-everything child dies with `SIGSYS` instead of exiting 7.
7. The stack probe made x86-only again → on AArch64 step 8's create with an
   unmapped stack **succeeds**, and the thread it made dies at address 0
   and takes the process with it.
8. `SYS_thread_self` answering the scheduler's `tid` → three checks fail at
   once, the id being asserted in steps 1, 6 and 7.
9. `PROCESS_MAX_THREADS` not checked → step **12** runs past 300 threads
   and both its bound assertions fail.
10. The futex timeout left unbounded → step 4's `-EINVAL` for a duration
    that would wrap the deadline becomes an immediate `-ETIMEDOUT`.
11. The allocator's lock removed → step **11** aborts (`SIGABRT`, status
    134): three threads in one free list trip the allocator's own
    corruption check. That is the hazard a review said documentation could
    not excuse, and it is right -- the lock is the fix, and this is the
    proof it is load-bearing.

A twelfth reintroduction is not needed for the one deadlock this locking
caused, because the fix is what the test now asserts: `fflush(NULL)` used
to take the stdio lock in the public `fflush` and take it again in
`__stdio_flush_all`, so a thread deadlocked against itself. Nothing called
`fflush(NULL)` until a review found it; step 11 calls it now, and the
null-stream branch runs the unlocked core.

stdio's lock has no proof of its own here. Racing a `FILE`'s buffer
pointers garbles output rather than failing an assertion, and this test
cannot read its own stdout; what the log does show is whole lines from
three threads, where an unlocked stream would interleave inside them. The
lock is argued from the code (everything funnels through `fputc`, and
`vfprintf` holds it across a whole format so the sink writes through the
unlocked core) and from the allocator's proof, which exercises the same
mutex.

**One fix cannot be proved here, and the reason is a property of this
kernel.** The mutex used to hand the lock over as "held, no waiters"
(`cas(state, 0, 1)` in the retry loop), which strands a sleeper whenever
three or more threads contend: the winner leaves 1, the unlock sees 1 and
wakes nobody, and a thread already asleep on 2 is never called again. That
is the variant Drepper's *Futexes Are Tricky* gives as flawed, and the fix
is his correct one -- always exchange 2 in when taking the lock through the
slow path. Reintroducing it does **not** fail step 9 (the mutex step), because
`futex_wait` in this kernel returns 0 when a wake raced its enqueue (a
spurious wake the futex contract permits, and this one takes), and the
retry loop absorbs it: the stranded thread is rescued by the next
contender's wake. The flaw is real and the rescue is not something a
correct mutex may rely on -- the contract permits spurious wakes, it does
not promise them -- so the fix stands on the argument and the step stands
as the regression guard for the lock's *mutual exclusion*, which it does
prove (no lost update under three contenders).

Two other proofs were first written against the **shared** machinery --
the futex's compare, and the zero-and-wake in `process_thread_exit` -- and
both hung the boot *before* `thrtest` ran, because the Linux personality's
own joins depend on exactly the same code. That is evidence the contract
belongs where it now lives rather than in a personality, but it proves
nothing about this test, so each was rewritten to perturb the native
wrapper alone.

The native thread door's proofs (steps 23–30), each run on x86-64 with
the mutation reverted after: the wake-all broadcast put back (step 23's
count returns to seven or eight); a waiter relocking through the fast
path, and the requeue waking none (each a bounded join reporting a hang
in step 23); the kernel requeue without its compare (step 26's `-EAGAIN`
never comes); `SYS_thread_kill` delivering to the process instead of the
thread (step 30's blocked sibling sees the handler run elsewhere); and
the same-address requeue moving waiters again (the boot stops at step
23's sleeper count with interrupts off — the kernel bug this unit found).
One proof **passed** and changed the design: the report's third rule,
the broadcaster reading the mutex word after the requeue, removed —
every step still passed, because the woken waiter heads the chain
under rule 1 whatever the word says. The rule is gone, and with it the
only load through the recorded pointer and the lifetime contract it
needed.

## cwdtest: the working directory from more than one thread

`userland/tests/cwdtest.c` (`docs/audit/next-subsystem-cwd-ref.md`), four
steps, run from `rc.test` on both architectures.

(1) A walk survives the directory moving under it: one thread alternating
between two directories, another opening a relative path. Every refusal
must be `-ENOENT` and both answers must actually occur, or the threads did
not overlap and the step tested one directory in peace.

(2) `chdir` racing `chdir`, with a reader of the name. The moves go
**down** (`chdir("s")`), not `../X`, and the two bases are at different
depths sharing nothing after `/tmp/cwdr/` — because `..` discards the last
component, which is the only part where sibling paths differ, so a torn
`cwd_path` cannot change the answer. An earlier version used `../a` and
`../b` and could not have failed.

Two threads issuing relative moves share one current directory, so a
`chdir("..")` applies to wherever the other thread left it and the pair
can walk the process up out of the subtree -- `/tmp` and `/` included.
The observer therefore checks membership of *every path the writers can
reach*, not of the two they aim at; an earlier version failed on a
correct kernel for not knowing that. A torn base is still caught, because
the two names share only `/tmp/cwdr/` and any mixture of them is in no
legitimate set.

(3) The name and the directory agree. Its writers move **down**
(`chdir("s")`), not between siblings: sibling moves cannot express the
defect at all, because normalising `../NAME_A` from either sibling and
looking it up from either sibling both produce `DIR_A`, so a path and a
vnode taken from different directories still agree. A down-move keeps the
base, so the pair can name different leaves, and a marker file in each
leaf tells them apart. The writers are **quiesced** first,
on a condition variable, because the obvious version — `getcwd`, then open
the file belonging to it — fails on a correct kernel when a writer chdirs
between the two calls. Quiescing is sound here because the defect's damage
is persistent: a process left holding one directory's path and another's
vnode stays that way until its next `chdir`.

(4) The directory is removed while a walk is inside it, which is what it
actually takes to free the vnode: `ramfs` pins every child from its
parent's entry, so steps 1 to 3 free nothing (measured: zero). Three
threads — mover, remover, walker — reach 228 frees and ~900–1250 walks
inside the victim.

**Steps 1–4 are a regression test, not a proof**, and the difference is
recorded rather than blurred. The cwd-ref unit recorded that every
reverted-fix run passed, "including one with every freed vnode poisoned
with `0xAA` before `kfree`", and concluded the walk is never inside the
few instructions where the free lands. **The second half was wrong.**
With the poison in `vnode_release` -- the one place every vnode free
passes through -- step 4 caught the reverted fix in one x86-64 boot of
five (`#GP` in `kobject_get`, `RAX=5a5a5a5a5a5a5a5a`), and in none of
three AArch64 boots (`docs/audit/next-subsystem-cwd-hold.md`, "Measured").
A rate is what a regression test gives; whatever the earlier `memset`
poisoned, it was not what the walk read.

**`cwdtest --held <pass>` is the proof**, driven by the kernel test
`cwd-hold-native` with the held-walk seam armed for the process name
(`docs/kernel-services/vfs/testing.md`, "The held walk"): two threads,
one pass of three (`capture`, `outlive`, `swapfirst`), A's relative
`open("f")` held with its pointer in hand until B's `chdir` has published
and put, the order enforced by the seam. Its twin at the Linux door is
`tests/linux/lxcwd.c` under `cwd-hold-linux`, with the first two passes.
