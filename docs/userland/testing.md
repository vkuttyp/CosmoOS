# Userland: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| System calls through libc | `init --selftest` (kernel self-test `process-user`) | `make test` (self-test builds) |
| Shell and utilities, scripted | `/etc/rc`, run by init, runs `/etc/rc.test`; the script prints `SHTEST: PASS` or `SHTEST: FAIL n` | `make test` (self-test builds; the script is packed only with `SELFTEST=1`) |
| Shell, interactive | `tests/boot/shelltest.py` types commands at the `cosmo$ ` prompt through QEMU's serial stdin and checks the log | `make test`, every build |
| Kill paths | Kernel self-test `process-spawn` kills `init --block` and `init --spin` | `make test` |
| Serial-log markers | `init: CosmoOS userland, pid N`, `CosmoOS userland ready`, `init: rc exited with status 0`, `interactive-ok`, `init: shell exited with status 0`, `[ INFO] init exited with status 0`; self-test builds add `USERTEST: PASS`, `SHTEST: PASS` | `tests/boot/run_boot_test.py` |

## `init --selftest`

Listed in `docs/kernel/process/testing.md` (`process-user`). In short:
the Phase 7 filesystem checks with the libc directory stream and stdio
on a file; the Phase 8 socket checks through the libc names and
`inet_pton`/`inet_ntop`; the Phase 9 checks (`proc_selftest`): pipes and
`dup`/`dup2`, the console as a character device, `spawn` of `echo` into
a pipe and its status, `sh -c "cd /tmp && pwd && exit 7"` (the child's
cwd and status, the parent's cwd unchanged), a `cat` blocked on a pipe
killed with `SIGKILL` (status 137, `WNOHANG` returned 0 before),
`kill` of a bad pid (`ESRCH`) and signal 0 (`EINVAL`), hostile spawn
requests (closed parent handle `EBADF`, duplicate child slot `EINVAL`,
a non-executable file and a directory `EACCES`, a missing file `ENOENT`,
an empty `argv` `EINVAL`), `waitpid` with no children (`ECHILD`),
`chdir`/`getcwd` with `..` and `.` normalisation, `ENOTDIR`, `ENOENT`,
`ERANGE`, `getppid() == 0`, its own `procinfo` record, `klog_read`,
`sysctl_get` (`kernel.name`, `hw.ncpu`, `sysctl.names`, `ENOENT`,
truncation), `malloc`/`realloc`, `snprintf`, `strtol`, `setenv`/`getenv`.

## `/etc/rc.test`

Straight-line shell (no control flow exists); `FAILS` is set to 1 by
`||` on a command that must succeed or by `&&` on a command that must
fail, and the last line runs `sh -c "exit $FAILS"` to print the verdict.
What it covers, in order: `mkdir -p` of a nested path; `>` and `>>`;
one-, two- and three-stage pipelines through `cat`; `cat` of the
results (the log shows `hello` and `hello` then `world`); `cp` into a
directory and `cp -r`; `ls` of the copy; `mv`; `rm` of a moved-away
file fails; `rm -r`; `ls` of a removed directory fails; `false && ...`
and `true || ...`; `false; echo "status $?"` (prints `status 1`);
`2>` capture of `cat`'s error and its display; `sh -c "exit 3"` (prints
`exit 3`); a missing command (prints `notfound 127`); variables
(`X=42; echo "var $X ${X}1 $$"`); `export` visible in a child `sh -c`;
`cd` with `pwd` into `/tmp/shtest`, `dir`, `..`; `ls -l | cat > file`
then `cat` of it; `echo -n`; `ps`; `sysctl kernel.name`; `kill 99999`
fails; the package section (`pkg update`, `install fortune`, `badsig`
and `badsum` refused, `hello=1.0` then `upgrade`, removal in dependency
order; `docs/pkg/testing.md`); `rm -r` of the work directory; `SHTEST:
PASS`.

## Interactive harness (`tests/boot/shelltest.py`)

For each entry of `COMMANDS`, waits until the serial log holds one more
`cosmo$ ` prompt than commands sent, writes the command and `\n` to
QEMU's stdin, then checks the listed patterns against the whole log at
the end:

| Typed | Required in the log |
|---|---|
| `echo interactive-ok` | `^interactive-ok$` |
| `ls /bin` | `^sh$`, `^cat$` |
| `ps` | a running `init` line with one thread, a `ps` line |
| `echo $((` | nothing crashes (the line prints `$((`) |
| `pwd` | `^/$` |
| `cd /tmp && pwd && cd /` | `^/tmp$` |
| `sysctl kernel.name` | `^kernel.name = CosmoOS$` |
| `dmesg` | the `serial: console input on IRQ 4` line |
| `nosuchprogram` | `^sh: nosuchprogram: not found$` |
| `pkg install hello && hello && pkg list` | `^hello, world \(hello 1\.1\)$`, `^hello\s+1\.1\s+prints a greeting$` (`docs/pkg/testing.md`) |
| `exit 0` | the run ends: `init: shell exited with status 0` |

Failures appear as `shell harness: ...` lines (`no prompt before
command N`, `never sent`, `after 'cmd' missing /pattern/`).

## Measured results (2026-09-05, QEMU TCG, Apple Silicon host)

| Configuration | Result |
|---|---|
| debug, `-smp 4` | `SELFTEST: PASS (61 tests)`, `USERTEST: PASS`, `SHTEST: PASS`, harness complete, about 10 s |
| debug, `-smp 1` | PASS |
| release | PASS (no self-tests; `rc.test` absent; the interactive harness runs) |
| `make test-crash`, `make host-test`, `make analyze`, `make reproducible` | PASS |

## Gaps and planned tests

- No test of a redirected builtin (`pwd > file`), of `sh file args`
  with `$1`, of `set -e`, or of `.`/`source`.
- No test types an editing key (`^U`, backspace) at the interactive
  prompt; the tty self-test covers them.
- No test of `cp`/`mv` across mounts (`EXDEV` path), of `mount`/`umount`
  from the shell, or of `kill` of a live process from the shell.
- `ls -l` output is checked only for presence, not for field values.
