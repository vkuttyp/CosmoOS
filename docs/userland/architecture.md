# Userland: architecture

Phase 9 of the roadmap ("libc, shell, coreutils, init/services").
Constitution section 45 (a traditional Unix userland: small, composable
utilities; the initial program list), section 69 (the first visible
shell prompt), section 63 (`userland/{init,shell,coreutils,system,
networking}`), section 44 (credentials; no reliance on a single root
identity beyond what exists), section 46 (programs use libc, never the
kernel directly).

## Where it sits

```text
   kernel_main ──spawns──▶ init (pid 1)                            userland/init/
                             │  runs /etc/rc (a shell script) and waits
                             │  spawns /bin/sh on the console, waits, exits with its status
                             ▼
                           sh                                      userland/shell/
                             │  reads lines from the console, spawns programs with
                             │  pipes and redirections, waits, reports status
                             ▼
              echo cat ls cp mv rm mkdir rmdir                     userland/coreutils/
              mount umount ps kill dmesg sysctl                    userland/system/
                             │
                             ▼
                           libc (docs/libc/)  ──▶  system calls
```

Programs are delivered in the boot archive under `bin/`, `sbin/` and
`etc/`, which the kernel's boot namespace exposes as `/bin`, `/sbin`,
`/etc` (the archive's other entries stay under `/boot`). There is no
disk installation yet: the running system is the archive.

## Purpose

Make the machine usable from a terminal: boot to a prompt, run
programs, connect them with pipes, look at files and processes, and
shut down cleanly. Every program is small, does one thing, and is
written against libc like any Unix utility, so that Phase 9's real
product, a userland that can grow, has a shape.

## Responsibilities

**init** (`/boot/init`, pid 1): mounts nothing (the kernel already
provides `/`, `/tmp`, `/mnt`, `/dev`), runs `/etc/rc` through `/bin/sh`
if it exists and waits for it, then runs `/bin/sh` on the console
(handles 0, 1, 2 inherited) and waits; when the shell exits, init logs
the status and exits with it, which ends the boot (the kernel treats the
exit of init as "boot complete", as in earlier phases). Orphans are
reparented to init by the kernel; init reaps them with `waitpid(-1,
WNOHANG)` whenever its wait returns. The `--selftest` and `--crash`
modes remain (the kernel self-tests use them). Service supervision
(restart policies, dependency order, a service description format) is
the recorded next step; the single-shell policy is what a bring-up
system needs.

**sh** (`/bin/sh`): prompt `cosmo$ ` (the constitution's `myos$` with
the project's name, as the kernel banner already did); reads a line,
tokenises it (words, single and double quotes, backslash escapes,
`#` comments), expands `$VAR`, `${VAR}`, `$?`, `$$`, `$0`..`$9`, `$#`,
parses pipelines (`|`), redirections (`<`, `>`, `>>`, `2>`, `2>&1`),
lists (`;`, `&&`, `||`), assignments (`NAME=value` alone: shell
variable; `export NAME[=value]`: environment), and runs commands:
builtins (`cd`, `pwd`, `exit`, `export`, `unset`, `set` (list), `:`,
`true`, `false`, `wait`, `.`/`source`) or programs found through `PATH`
with `spawnvp` and explicit handle maps for pipes and redirections;
waits for every process of a pipeline and sets `$?` to the last one's
status. Non-interactive use: `sh file [args]` and `sh -c 'command'`;
with `-e` a failing command ends a script. No control flow (`if`,
`while`, `for`, functions), globbing, background jobs, job control,
here-documents, command substitution or arithmetic: recorded for the
next shell phase; the parser is written so that these slot in.

**coreutils** (`/bin`): `echo [-n]`, `cat [files]` (stdin without
arguments), `ls [-la] [paths]` (names; `-l` type, mode, size, ino),
`cp [-r] src dst`, `mv src dst` (rename; falls back to copy and unlink
across mounts), `rm [-r] paths`, `mkdir [-p] dirs`, `rmdir dirs`,
`pwd`, `true`, `false`, `sleep seconds` (small extras the scripts and
tests need; all under thirty lines).

**system** (`/sbin`): `mount source target fstype [-r]`, `umount [-f]
target`, `ps` (pid, ppid, uid, state, threads, syscalls, cpu time,
name), `kill [-sig] pid...` (numeric or `KILL`/`TERM`/`INT`), `dmesg`
(the kernel log ring), `sysctl [-a] [name...]` (read-only values).

**Scripts** (`/etc`): `rc` (prints the userland banner and, in
self-test builds, runs `/etc/rc.test`), `rc.test` (the shell's own test
script: pipelines, redirections, status, variables, builtins, the
utilities; prints `SHTEST: PASS` or `SHTEST: FAIL <what>`). Only debug
builds (`SELFTEST=1`) carry `rc.test`.

## Non-responsibilities

- Users, login, passwords, a user database, `su`: one uid (0) runs
  everything; the kernel enforces uid rules that nothing yet exercises.
- Services and supervision beyond one shell; a `getty`; multiple ttys.
- Editors, `grep`, `sed`, `awk`, `find`, `tar`, `less`: they arrive with
  the ports system, not as hand-written utilities.
- Networking tools (`userland/networking/`: `ping`, `ifconfig`-like,
  `nc`): next phase; the libc has the socket API ready.
- The package manager (`pkg/`, `ports/`): constitution sections 47–48,
  a later phase.

## Interfaces at a glance

| Interface | Where | Used by |
|---|---|---|
| The boot archive's `bin/`, `sbin/`, `etc/` → `/bin`, `/sbin`, `/etc` | `Makefile`, `kernel-services/vfs/ramfs.c` | init, sh (`PATH=/bin:/sbin`) |
| `sh` command language (above) | `userland/shell/` | people, `rc`, tests |
| `USERTEST: PASS/FAIL`, `SHTEST: PASS/FAIL`, `init: ...` lines | serial log | `tests/boot/run_boot_test.py` |
| The interactive harness: after `cosmo$ ` types commands, expects their output | `tests/boot/shelltest.py` | `make test` |

Tests (`testing.md`): `init --selftest` (kernel self-test
`process-user`) covers the system calls through libc; `/etc/rc.test`
covers the shell and the utilities non-interactively; the boot harness
types `echo interactive-ok`, `ls /bin`, `ps`, `exit` at the prompt and
requires the answers, covering the tty input path; `make BUILD=release
test` runs the interactive part without the self-tests.
