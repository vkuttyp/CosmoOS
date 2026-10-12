# System Calls: Testing

The system-call layer is tested from both sides: kernel self-tests that
call the user-access helpers directly, and user programs that make real
system calls and check each result. Commands are as in
`docs/development.md` (use `gmake` on macOS).

## What runs where

| Level | What | Command |
|---|---|---|
| Target self-tests (debug builds, `CONFIG_SELFTEST`) | `uaccess`, `uaccess-guard`, `read-bounce`, `read-bench`, `process-user`, `process-efault`, `process-protnone`, `process-oom`, `signal-stop-restart`, `syscall-fuzz` (registered in `kernel/core/selftest.c`) | `make test` |
| Boot scripts (debug builds) | `/etc/rc.test` runs `/boot/tests/native/thrtest` (the filter step); `/etc/rc.linux` runs `/boot/tests/linux/lxtest` (unknown Linux numbers, `SA_RESTART`) | `make test` |
| Single CPU, two CPUs | the same suite | `QEMU_SMP=1 make test`, `make test-smp2` |
| AArch64 | the same suite through the `SVC` entry | `make ARCH=aarch64 test` |
| Access guard | the same suite on a CPU model with SMAP (x86-64) or PAN (`cortex-a76`); the harness requires `selftest: uaccess-guard: guard live` | `make test-guard`, `make ARCH=aarch64 test-guard` |
| Release | the boot only: `SELFTEST` is 0 for release builds, so none of the self-tests above is compiled | `make BUILD=release test` |
| Host (ASan/UBSan) | `test_cred` (`cred_has` and the privilege rules the handlers call) | `make host-test` |

There is no host test of the dispatcher, the filter or `uaccess.c`; they
are exercised only in the guest.

## Kernel self-tests

| Test | File | Steps | Proves |
|---|---|---|---|
| `uaccess` | `kernel/syscall/uaccesstest.c` | from a kernel thread (no process): every exception-table entry names kernel text and resolves to its fixup; `user_range_ok` on seven boundary cases (`USER_LO`, `USER_HI` with length 0 and 1, `USER_LO - 1`, a range straddling `USER_HI`, a kernel address, a `(size_t)-1` length); four copies at unmapped user addresses return `-EFAULT` with the `fixups` count up by exactly four; the destination is untouched or terminated; a zero-length copy and a kernel-address copy touch nothing; `strncpy_from_user(..., 0)` is `-EINVAL` | SC9, M32 |
| `uaccess-guard` | `kernel/syscall/uaccesstest.c` | a private space maps one page; with interrupts off the page is read once inside `arch_user_access_begin/end` (must succeed) and once outside (must fault where `arch_user_guard_present()`, must succeed where not); logs which | SC10 |
| `read-bounce` | `kernel-services/vfs/vfstest.c` | `syscall_bounce_get` sizing (stack chunk to 1 KiB, heap to 64 KiB); an injected heap refusal takes the stack chunk and bumps `syscall_bounce_fallback_count`; a ramfs file answers a 64 KiB object call with 64 KiB, its tail with 3072; a pipe holding 300 bytes answers 300 | SC12 |
| `read-bench` | `kernel-services/vfs/vfstest.c` | object-path read throughput of a ramfs file at 1 KiB and 64 KiB requests (figures in `architecture.md`, "The bounce") | the bounce's purpose; no assertion on speed |
| `process-user` | `kernel/process/proctest.c` | runs `init --selftest` (below) and requires exit status 0, within a 20 000 ms budget | everything in the user-mode suite |
| `process-efault` | `kernel/process/proctest.c` | `init --probe efault`: system calls given `PROT_NONE`, read-only and unmapped pointers return `-EFAULT`; exit 0 | SC9, P14, M32 |
| `process-protnone` | `kernel/process/proctest.c` | `init --probe none-touch`: a user-mode touch of a `PROT_NONE` page ends the process with the fault status; the kernel keeps running | the user/kernel split of P15 |
| `process-oom` | `kernel/process/proctest.c` | `init --probe oom-copy`: an injected allocation failure in the demand fault inside `read`'s copy is `-EFAULT`; a user-mode touch under the same failure is fatal | M32 |
| `signal-stop-restart` | `kernel/process/proctest.c` | `init --probe signal-stop-restart`: a child reading the console from the background is stopped, given the terminal and continued; the kernel types `x\n`; the child's `read` must return the line, not `EINTR` | SC8 (the stop restart) |
| `syscall-fuzz` | `kernel/process/proctest.c` | runs `init --syscall-fuzz 20000 20260905` and requires exit status 0 | SC1–SC3 under random input; below |

## The user-mode suite (`init --selftest`)

`userland/init/init.c` runs the sections of `g_sections` in order and
prints `USERTEST: section <name> <ms> ms` after each and `USERTEST:
PASS` or `USERTEST: FAIL` at the end; `tests/boot/run_boot_test.py`
checks the section list against the lines it sees. The sections are
`fs`, `mmap`, `fsctl`, `net`, `unix`, `fifo`, `devices`, `proc`, `fpu`,
`trap`, `priv`, `proc-fs`, `svc`, `syscalls`. Those that test the
system-call layer itself:

- **`syscalls`** (`syscalls_selftest`): `write` and `read` with valid,
  closed, wrong-right and negative handles (`EBADF`), and with kernel,
  `0x10`, top-of-window, unmapped and `(size_t)-1` ranges (`EFAULT`);
  `getpid`, `yield`, `sleep_ns` (at least 5 ms, under 200 ms, and
  `EINVAL` above an hour); `mmap`/`munmap`/`mprotect` argument rules;
  `log` from a `PROT_NONE` page and a kernel address (`EFAULT`) and with
  a 4096-byte length (`EINVAL`); `close(7)` (`EBADF`); and the unknown
  numbers `SYS_COUNT`, 999999 and -1 (`ENOSYS`).
- **`proc`** (`proc_selftest`): the filter. Each case is a child,
  `init --filter <kind>`, whose exit status is the result: `inside` (0),
  `outside` (159, `128 + SIGSYS`), `exit-unnamed` (0), `widen` (159),
  `inherit` (0: its own child dies of the inherited filter),
  `inherit-start` (0: a child of a `spawn`+`wait` filter reaches its own
  `main`), `linux-child` (0: a filtered native process cannot spawn
  `lxhello`, `EPERM`).
- **`priv`** (`priv_selftest`): `init --unpriv-test` drops to uid 1000
  and requires `EPERM` from, among others, `mount`, `umount`,
  `sethostname`, `klog` and a `kill` of a root process.

## Boot-script programs

- **`thrtest`** step 10 (`userland/tests/thrtest.c`): a child that
  denies only `thread_create` and calls it must die of signal 31; a
  child with an all-zero mask must still leave through `thread_exit`
  with its own status 7.
- **`lxtest`** (`tests/linux/lxtest.c`, `docs/compat/linux/testing.md`):
  Linux numbers 510 and 9999 are `-ENOSYS`; in the threads section a
  `read` interrupted by `tgkill` returns `-EINTR`, and with
  `SA_RESTART` it is restarted and completes with the data written
  afterwards.

## The syscall fuzzer (`init --syscall-fuzz N SEED`)

`syscall_fuzz` in `userland/init/init.c`, specified in
`docs/verification/design.md` §5. It drops to uid/gid 1000, closes
handle 0, maps two scratch pages, and makes `N` calls chosen from its
`allowed` table with arguments drawn from pointer, length, handle,
string and flag generators (NULL, a valid page, a straddling end, the
kernel half, the top of the window, unterminated strings, `-1`, very
large lengths). The table holds the calls 1–61 except those that
would block or kill the fuzzer itself, which the source names (`exit`,
`read`, `recvfrom`, `accept`, `connect`, `wait`, `kill`, `spawn`,
`vcpu_run`, `setrlimit`, and the `futex_wait`, `futex_wake`,
`thread_create` it never had), plus `mprotect`, `futex_requeue` and
`thread_kill`; a few are
constrained per call (`munmap`/`mprotect` only on their own page or an
invalid range, `thread_kill` with signal 0). The table also holds
`SYS_COUNT`, `SYS_COUNT + 1`, 1000 and -1, so the dispatcher's own
bounds check is fuzzed. It prints

```text
USERTEST: syscall-fuzz ok: N calls, E errors, S successes, C/SYS_COUNT system calls exercised, seed SEED
```

and the `syscall-fuzz` self-test requires exit status 0. A result in
`[-4095, -1]` is counted as an error and never treated as a failure;
the test fails only when the kernel panics or the process dies.

To run it by hand at the shell of a debug boot, as a user that may
change its ids (it drops to uid 1000 itself and exits 1 if it cannot):

```sh
/boot/init --syscall-fuzz 20000 1
```

## Gaps

Recorded against the invariants they leave partly unchecked:

- No unknown number under a filter (SC3), no `sigreturn`/`rt_sigreturn`
  under a mask that omits them (SC5), no install racing a spawn (SC6),
  no filtered `exec` across personalities and no all-zero fallback
  (SC7).
- No native-personality `SA_RESTART` test, and nothing checks that a
  `sigreturn` result is never restarted (SC8).
- `uaccess-guard` cannot fail on the default CI CPU models, which have
  no SMAP or PAN; only `make test-guard` proves the guard (SC10).
- The native table's fullness and an object's over-report are checked
  by review only (SC4, SC12).
- The fuzzer reaches none of the calls numbered 62 and above except
  `mprotect`, `futex_requeue` and `thread_kill` (no `aio_wait`,
  hostname, filter, signal, process-group, terminal, thread, link,
  `msync`, unix-socket, `mknod`, timer, `switch_root`, `exec` or
  `sysctl_set` call), and none of the Linux table. It is one process with one seed
  (`docs/verification/testing.md`).
