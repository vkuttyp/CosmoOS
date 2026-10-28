# A5, second PR: nice values, their rules in Nova, 2026-10-28

Roadmap 1.2 A5 (OpenSSH and git) absorbs LX-13: no nice values and no
`setpriority`/`nice`, with `/proc/<pid>/stat` reporting priority 20 and
nice 0 for every process. On 2026-10-26 the owner chose A5's kernel lines
in Nova and nice values as a weighted share. This PR is the interface:
the value, its rules, the calls and where it shows. The third PR makes
the scheduler weigh it.

## Scope and baseline

Branch `a5-nice` from main at `1ccfd9eb` (#475).

## Audit (main at `1ccfd9eb`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| No nice value anywhere; `stat` prints the literal `20 0` | `procfs.c` `render_pid_stat` | the gap |
| `getpriority`/`setpriority` absent from both syscall tables (`-ENOSYS`) | `compat/linux/nr_*.h`, `syscalls.c` | added (x86-64 140/141, AArch64 141/140) |
| Linux `RLIMIT_NICE` reads as infinity and accepts anything | `lx_rlimit_map` | mapped onto a new `COSMO_RLIMIT_NICE`, default 0 |
| Raising a limit needs `PRIV_PROC`; `cred_has` needs euid 0 too | `process_setrlimit`, `cred.h` | `PRIV_PROC` is the `CAP_SYS_NICE` here |
| A group walk that drops the table lock between members exists | `process_group_next` | generalised to a user walk too |
| `kill` hides other domains as `-ESRCH` | `native.c` `kill_one` | the nice calls do the same |
| `ps` is BusyBox's, reading `stat` field 19; `nice` and `renice` were off | `busybox.config`, `procps/ps.c` | enabled |
| The native `cosmo_procinfo` has no spare field | `uapi/cosmo/syscall.h` | not extended: the native ABI gets no nice call |

## What changed

- **The rules in Nova** (`e2932e46`; `kernel/process/nice.nova`, 75
  lines, generated `nice.c`; header `kernel/include/kernel/nice.h`):
  the clamp; who may set a value (a target whose real or effective uid
  is the caller's effective uid, or any with `PRIV_PROC`: else `-EPERM`);
  how low (below 20 - the *target's* `RLIMIT_NICE` only with `PRIV_PROC`:
  else `-EACCES`); and how a group's or a user's outcome is reported
  (Linux's `set_one_prio` and `getpriority`). It runs under the target's
  spin lock, so nothing in it can fail: the one generated check, in
  `20 - clamp(n)`, cannot be reached. `unsafe` is only in the export that
  turns the header's pointers into references.
- **The process side, C.** `struct process.nice` under `lock`, copied by
  fork and spawn with the credentials, kept by exec.
  `process_nice_set` checks and stores under the target's lock;
  `process_user_next` walks a uid as `process_group_next` walks a group.
- **Linux ABI.** `getpriority`, `setpriority` (`PRIO_PROCESS`, `_PGRP`,
  `_USER`, `who` 0 the caller's), `RLIMIT_NICE`. `/proc/<pid>/stat`
  reports priority 20 + nice and nice; `/proc/<pid>/limits` names
  `nice`.
- **BusyBox** (`ab057202`): `nice` and `renice`.

Not done: the scheduler does not read the value (A5's third PR); the
value is per process, not per thread; the native ABI has no call.

## Tests

Written first (`1f5a61d6`) and run against the unchanged kernel
(2026-10-28, x86-64 debug): lxtest's new block failed 26 checks
(`getpriority` `-ENOSYS`, `RLIMIT_NICE` infinity, `stat` 20/0, the
unprivileged child's first failing check). The shell script and the
Linux tests after lxtest then stopped, as when any lxtest check fails.

| Test | Where | Checks |
|---|---|---|
| lxtest's nice block (guest, both arches) | `tests/linux/lxtest.c` `test_nice`, marker `lxtest: nice ok` | the 20 - nice encoding; `stat` fields 18 and 19; clamping and the 32-bit argument; `-EINVAL`, `-ESRCH`; `RLIMIT_NICE` 0 by default; fork inherits; a group's and a user's set and get; an unprivileged child: raising free, lowering to the limit and no further (`EACCES`), root's process readable and `EPERM`, the limit unraisable |
| ash.sh (guest, both arches, static and dynamic BusyBox) | `tests/busybox/ash.sh` | `nice`; `nice -n 7 nice` (exec keeps it); `ps -o nice`; `renice -n 2`, `renice -12`; `stat`'s `8 -12` |
| `test_nice` (host) | `tests/host/test_nice.c` | the permission rule against Linux's over all 950,400 inputs (caller and target uids, privilege, ten limits, every current and wanted value); named cases; both results' folding |

No fuzz target: the inputs are few enough to try exhaustively.

## Validation (2026-10-28, macOS host, this branch at `2063373f`)

| Check | x86_64 | aarch64 |
|---|---|---|
| debug `make test` (lockdep) | PASS (178.8 s): `lxtest: nice ok`, `LINUXTEST: PASS`, `SHTEST: PASS`; no lockdep report beyond the self-test's expected ones | PASS (187.9 s), the same |
| `make test-busybox` | PASS (440 s); the five nice checks pass in both shells | PASS (430 s), the same |
| `BUILD=release make test` | PASS (19.4 s) | PASS (21.6 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test` (`test_nice` included) | PASS | (host) |
| `make nova-check` with novac 0.1.1 | match (5 files) | |

The debug boots warn that lockdep's class table peaks at 500 of 512; main
at `1ccfd9eb` prints the same warning, and this PR adds no lock class.

## Backlog

LX-13 stays open until the third PR: the value is kept and shown but not
yet weighed.
