# K1, third PR: the Linux personality's calls in CeeSafe, 2026-11-12

After K1's second PR (#513) the one C file left in `compat/` and `kernel/syscall/` was `compat/linux/syscalls.c`
(4,223 lines): the Linux personality's calls, the hooks its record names, the call table and the record. This PR
moves the calls and the hooks to CeeSafe and leaves the file as the table and the record.

## Scope and baseline

Branch `k1-lx-files` from main at `8cb69229` (#513). One commit per file moved, each building on both
architectures, then the documents. Two commits add entries to `docs/testing/flakes.md` for failures seen while the
stack was built (below).

## What changed

- **Sixteen CeeSafe files** (6,557 lines) hold what `syscalls.c` held:

  | File | Calls |
  |---|---|
  | `lxident.cee` | identity, limit, credential and session calls |
  | `lxtime.cee` | clock, sleep, nice and reboot calls, interval timers, futex |
  | `lxproc.cee` | fork, clone, execve and the affinity calls |
  | `lxsys.cee` | sysinfo, uname, sethostname, getrandom |
  | `lxpoll.cee` | poll, ppoll, select, pselect6 |
  | `lxmount.cee` | mount, umount2, statfs, fstatfs |
  | `lxbase.cee` | `lx_get_path`, `lx_at_base`, `lx_file_of`, `lx_file_of_manage`, which the other files start from |
  | `lxmem.cee` | brk, mmap, msync, munmap, mremap, mprotect, madvise, the SysV shm calls |
  | `lxattr.cee` | the chmod and chown families, chroot, utimensat |
  | `lxname.cee` | unlinkat, faccessat, the rename and link families, chdir, fchdir, getcwd, the dup family, pipe, pipe2, fcntl, sync, fsync, umask |
  | `lxtty.cee` | ioctl |
  | `lxio.cee` | read, write, pread64, pwrite64, readv, writev, open, creat, openat, close, lseek |
  | `lxstat.cee` | the stat family, statx, the readlink and symlink families |
  | `lxfd.cee` | eventfd, signalfd, timerfd, memfd_create, ftruncate, epoll |
  | `lxdir.cee` | getdents64, mkdir, mkdirat, rmdir, unlink, access, mknod, mknodat |
  | `lxhooks.cee` | `linux_process_init`, `_release`, `_fork`, `_exec`, `linux_auxv`, `linux_sigtramp_addr`, `linux_claims_elf`, prctl, and `lx_unknown` |

- **`compat/linux/linux_internal.h`** declares every call for the table, and now holds `struct linux_state` and
  `struct lx_shm_attach`, which `lxmem.cee` and `lxhooks.cee` share.
- **`compat/linux/syscalls.c`** (311 lines) is the table, the list of calls a filter may never deny, and
  `personality_linux`. The table is `static const`, with `lx_unknown` in every slot by a range initialiser and the
  calls over it; `g_table`, the copy made into it when the first Linux process started, its lock and
  `linux_table_get` are gone. In the release objects of both architectures `llvm-nm` shows `linux_table` and
  `linux_always_allowed` as `r` and `personality_linux` as `R`.
- **`scripts/cee-gen.py`, `kernel/kernel.mk`**: an entry and an object for each file.

## Differences from the C

- The table is read-only data filled at build time; the C filled a writable copy at run time under a lock.
- `linux_state.wx_logged` is a `uint32_t`; it was a `bool`. Both are swapped atomically; `lxmem.cee` does it
  through an `AtomicU32`.
- Stack buffers start zeroed (the 1 KiB path buffers among them); the C left them unset until written.
- `faccessat` passes `vfs_stat` a 256-byte buffer, checked at build time to hold a `struct cosmo_stat`.
- `linux_auxv` checks once, before writing, that the caller's buffer holds every word it may write; the C
  asserted before each pair.
- The fork hook panics if its pool of attach records is empty when it takes one. `m <= n` under the parent's lock
  rules that out; the check is there because `make analyze` could not relate the two counts and reported a null
  dereference in the generated C (commit `8b87a98f`, after the first matrix run below).
- Calls that only x86-64's table names (`lx_mkdir`, `lx_open` and the like) are exported on AArch64 too, unused.
- `epoll_wait` zeroes the four bytes of padding in AArch64's 16-byte event record before copying it out.
- `fcntl(F_GETFL)` adds `O_NONBLOCK` with `+=` where the C used `|=`; the access mode never holds that bit.

## Failures seen while the stack was built

Recorded in `docs/testing/flakes.md`:

- `process-user` over its 15 s bound in four boots of one local matrix (x86-64 `test-smp2` and `test-chaos`,
  AArch64 `QEMU_SMP=4 test` and `test-net-login`), 17.9 s to 23.2 s, while another session's fuzzers loaded the
  host; all four passed rerun with the one-minute load under 6 (6.3 s to 8.5 s).
- On #513's CI: AArch64 chaos `sched-load`, "placement chose a CPU that was already running a thread" (first
  sighting), and the firmware RNG stall in the upgrade test's `be-intent-repaired` boot (seventh sighting).

## Validation

Local, macOS host, ceesafec 0.37.0 (the published binary). The matrix ran twice. The first run, at `553b413f`,
passed every step but `make analyze`, which failed on both architectures with the one unexpected diagnostic
described above. The table is the second run, at `8b87a98f`, which has the fix; no step in it needed a rerun.
`make test-net-login` was not run again: its result is the first run's.

| Step | x86-64 | AArch64 |
|---|---|---|
| debug and release build, `make cee-check` | exit 0, no warning | exit 0, no warning |
| `make host-test`, `make fuzz` | pass | pass |
| `make analyze` | clean (14 baselined, 0 unexpected, 0 stale) | clean (14 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 195.3 s | PASS in 179.0 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 195.5 s | PASS in 189.2 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 186.0 s | PASS in 208.0 s |
| `make test-smp2` | PASS in 187.0 s | PASS in 200.0 s |
| `make test-chaos` | PASS in 204.7 s | PASS in 191.6 s |
| `make test-harness-retry` | PASS in 197.0 s | PASS in 210.4 s |
| `make BUILD=release test` | PASS in 19.1 s | PASS in 21.6 s |
| `make test-busybox` | PASS in 455.5 s | PASS in 464.9 s |
| `make test-net-login` (at `553b413f`) | PASS in 730.7 s | PASS in 708.0 s |

## What this does not do

- The Linux table and record stay in C: the table's `[0 ... LX_NR_MAX - 1]` range initialiser has no CeeSafe form
  yet. Roadmap 1.3's K1 and roadmap 2.0's SA8 are ticked with that exception written beside them.
- Documents that name `compat/linux/syscalls.c` for calls that now live in the `lx*.cee` files are swept only in
  `docs/compat/linux/`: backlog DOC-4.
