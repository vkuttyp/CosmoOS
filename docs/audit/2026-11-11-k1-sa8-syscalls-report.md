# K1, first PR: the native system calls and the Linux socket calls in CeeSafe, 2026-11-11

Roadmap 1.3's K1 finishes roadmap 2.0's SA8: `kernel/syscall/` and `compat/linux/` in CeeSafe, except the two call
tables and the personality records. This PR is the seven commits that were made on the working branch on 2026-11-10
and -11 and not pushed, rebased onto main. It changes no behaviour on purpose; the differences from the C are listed.

## Scope and baseline

Branch `k1-sa8-syscalls` from main at `ffbbce9d` (#511, the ceesafec 0.33.0 pin). The commits were written and first
checked with ceesafec 0.24.2; the matrix below ran with 0.33.0, which accepts them unchanged.

## Audit (main at `ffbbce9d`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| `native.c` is 2,148 lines: the mapping, read/write, socket, process, aio, pipe, sysctl, reboot and getrandom calls, the bounce both personalities use, the native personality's two functions, the table and the record | `kernel/syscall/native.c` | what this PR ports, but the table and the record |
| `syscalls.c` is 4,956 lines; its socket calls are one section of about 730 | `compat/linux/syscalls.c` | the first of the sections K1 ports |
| The call tables are arrays of C functions and the personality record a C struct holding one | `native.c`, `syscalls.c` | writable in CeeSafe since 0.33.0 (ADR 0091); not done here |
| `SYS_spawn` reads its request twice and trusts the second copy's flags | `native.c` `sys_spawn` | kept as it was; backlog SEC-14 |

## What changed

One commit per file, each moving functions out of the C and naming them in `kernel/syscall.h` or
`compat/linux/linux_internal.h`; `scripts/cee-gen.py` and `kernel/kernel.mk` gain the ports.

| Commit | CeeSafe file | Lines | What moved |
|---|---|---|---|
| `03a9cfb3` | `kernel/syscall/mmap.cee` | 409 | mmap, msync, munmap, mprotect, `mmap_file_check` |
| `882b33c2` | `kernel/syscall/io.cee` | 307 | the bounce, `syscall_obj_read/_write`, `syscall_handle_read/_write`, read, write, log, close |
| `c10d4c08` | `kernel/syscall/sockets.cee` | 924 | the thirteen native socket calls |
| `661d247a` | `kernel/syscall/procs.cee` | 955 | spawn, exec, wait, kill, aio, timer, pipe, dup, cwd, procinfo, klog, the filter, hostname, `syscall_handle_stat` |
| `0b8fe981` | `kernel/syscall/sysctl.cee` | 556 | sysctl, sysctl_set, reboot, getrandom |
| `0bee38e0` | `kernel/syscall/personality.cee` | +39 | `native_claims_elf`, `native_auxv` |
| `d3e4d51e` | `compat/linux/lxsock.cee` | 1,201 | the fifteen Linux socket calls, `sock_of`, `sock_of_err` |

`native.c` is 224 lines: the table, the always-allowed list and the record. `syscalls.c` is 4,223.

Records that C defines and CeeSafe reads at the door (an address, `struct cosmo_msg`, `struct cosmo_spawn`,
`struct lx_msghdr`, the statistics records) are CeeSafe structs or words of the same layout, and the C compiler
checks each size and offset (`@cvalue`).

**Differences from the C**, each in its commit's message:

- Locals the C left uninitialised (an address not yet filled, a message's handles) start zeroed.
- User addresses made from a caller's numbers (a message's handle and rights arrays, `argv` and `envp` entries,
  control lengths and vector addresses at the Linux door) are added with wrap-around, as C's unsigned arithmetic
  did, so a bad one is `copy_from_user`'s `-EFAULT` and not a panic. mmap's offset overflow check is written
  without wrapping.
- `getcwd` and `gethostname` answer `-ERANGE` rather than copy past their buffer should the copied name ever fill
  it; both sources are shorter than the buffer, so it cannot happen today.
- `kill` of `INT_MIN` names group `0x80000000` by defined arithmetic.
- `sys_log`'s line stops at the first NUL, as `%s` did; `sysctl` values are written by `TextWriter`, cut at 512
  bytes with the full length reported, as `ksnprintf` did.
- `KASSERT`s are panicking checks with the same conditions.

## Validation

Local, macOS host, the tree of `d3e4d51e` with ceesafec 0.33.0. No step of the matrix needed a rerun.

| Step | x86-64 | AArch64 |
|---|---|---|
| debug and release build, `make cee-check` | exit 0 | exit 0 |
| `make host-test`, `make fuzz` | pass | pass |
| `make analyze` | clean (14 baselined, 0 unexpected, 0 stale) | clean (14 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 185.9 s | PASS in 189.2 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 191.7 s | PASS in 239.5 s |
| debug `make test`, `QEMU_SMP=4` | PASS in 185.9 s | PASS in 219.4 s |
| `make test-smp2` | PASS in 181.4 s | PASS in 209.0 s |
| `make test-chaos` | PASS in 199.3 s | PASS in 222.2 s |
| `make test-harness-retry` | PASS in 195.4 s | PASS in 209.1 s |
| `make BUILD=release test` | PASS in 18.7 s | PASS in 21.4 s |
| `make test-busybox` | PASS in 716.3 s | PASS in 488.0 s |
| `make test-net-login` | PASS in 727.5 s | FAIL once, then PASS in 887.5 s (below) |

AArch64 `test-net-login` failed in its fourth and last boot: `process-user` took 20,396 ms against its 15-second
bound, where the same image had taken 8,700, 8,425 and 6,794 ms in the three boots before it. The whole test rerun
passed. The host's load was rising as the failing boot ended (38 a minute later), but the rerun passed under a
higher sampled load, so the load is a candidate and not an established cause; the facts are in
`docs/testing/flakes.md`. No budget or assertion was changed.

Two earlier sightings from the days these commits were made, both recorded in `flakes.md`: `net-firewall`'s "from
B" check failed once in an x86-64 `test-net-login` on macOS (cause not established), and boots failed timing bounds
at host load averages of 57 to 142.

## What this does not do

- K1's acceptance box stays open: `compat/linux/syscalls.c` has 4,223 lines of C left, in sections.
- The two call tables and the personality records stay in C. ceesafec 0.33.0 can write them; that is a later PR.
- SEC-14 (`SYS_spawn`'s double read) is kept as it was.
