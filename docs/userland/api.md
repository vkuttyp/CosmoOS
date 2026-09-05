# Userland: API

The interfaces of the userland are what a person types and what init
promises the kernel. Everything here is user-facing behaviour of the
programs under `userland/`; the library they are built on is in
`docs/libc/api.md`. **ABI stability**: the command lines and outputs
below are meant to stay; the exit statuses follow Unix convention.

## Delivery

| Archive entry | Path in the running system | Mode | Built from |
|---|---|---|---|
| `init` | `/boot/init` (started by the kernel) | 0644 | `userland/init/init.c` |
| `bin/<name>` | `/bin/<name>` | 0755 | `userland/shell/sh.c`, `userland/coreutils/<name>.c` |
| `sbin/<name>` | `/sbin/<name>` | 0755 | `userland/system/<name>.c` |
| `etc/rc` | `/etc/rc` | 0644 | `userland/etc/rc` |
| `etc/rc.test` | `/etc/rc.test` (self-test builds only, `SELFTEST=1`; its Linux and virtualization sections report `skipped` when the x86-only fixtures are absent) | 0644 | `userland/etc/rc.test` |

`USER_BIN_PROGRAMS` (sh echo cat ls cp mv rm mkdir rmdir pwd true
false sleep) and `USER_SBIN_PROGRAMS` (mount umount ps kill dmesg
sysctl vmctl) in `userland/userland.mk` generate the entries; the kernel's
`ramfs_populate_boot` places `bin/`, `sbin/` and `etc/` entries at the
root and everything else under `/boot`.

## init (`/boot/init`)

| Invocation | Behaviour | Exit status |
|---|---|---|
| `init` | prints `init: CosmoOS userland, pid N`; sets `PATH=/bin:/sbin:/usr/bin:/usr/sbin`, `HOME=/`; if `/etc/rc` exists runs `sh /etc/rc`, waits, prints `init: rc exited with status N`; runs `sh` on the console (handles 0, 1, 2 inherited), waits, prints `init: shell exited with status N` | the shell's status (the kernel treats init's exit as the end of the boot) |
| `init --selftest` | `fs_selftest`, `net_selftest`, `proc_selftest`, then the Phase 4 checks; prints `USERTEST: PASS` or `USERTEST: FAIL (n checks)` | 0 or 1 |
| `init --crash` | prints `init: crashing on purpose` and writes to address 0 | 139 (fault) |
| `init --block` | reads one byte from handle 0 and exits 5 | 5, or 128 + sig when killed |
| `init --spin` | loops for ever | 128 + sig when killed |

While waiting for the shell init reaps every child that exits
(`waitpid(-1)`), so orphans the kernel reparents to it never linger. If
the shell cannot be started init prints `init: cannot start the shell:
<reason>` and exits 1.

## sh (`/bin/sh`)

`sh` (interactive: prompt `cosmo$ ` on handle 2, one `read` per line),
`sh file [args]` (script; `$0` is the file, `$1..$9` and `$#` the
arguments), `sh -c 'command' [args]`, `sh -e ...` (exit on the first
failing command outside `&&`/`||`). Reads lines of at most 1023
characters; end of file at the prompt (`^D`) prints a newline and exits
with the last status.

**Words**: single quotes (literal), double quotes (`$` expansion and
`\"`, `\\`, `\$` escapes), backslash escapes outside quotes, `#` starts
a comment at a word boundary. **Expansion**: `$NAME`, `${NAME}` (shell
variables first, then the environment; unset is empty), `$?` (last
status), `$$` (the shell's pid), `$0`..`$9`, `$#`. No word splitting of
expansions, no globbing, no command substitution.

**Operators**: `|` (pipeline), `;`, `&&`, `||` (the right side is skipped
when the left side's status decides), `<`, `>`, `>>`, `2>`, `2>&1`
(redirections apply to the command they follow; `2>` must start a
word).

**Assignments**: `NAME=value ...` alone sets shell variables (or updates
an environment variable of that name); `NAME=value command` exports the
assignment to the environment before running (a simplification: it
stays exported).

**Builtins** (run in the shell; with redirections they run with the
shell's handles temporarily replaced): `cd [dir]` (`$HOME` or `/`),
`pwd`, `exit [n]` (default `$?`), `export NAME[=value] ...`, `unset
NAME ...`, `set` (prints variables and environment) and `set -e`, `:`,
`true`, `false`, `wait`, `.` and `source file`.

**Programs**: found through `spawnvp` (`PATH`, default
`/bin:/sbin:/usr/bin:/usr/sbin`,
or a name containing `/`). Each pipeline stage receives exactly handles
0, 1, 2: the previous stage's pipe, the next stage's pipe, or the
shell's, overridden by redirections. The shell waits for every stage;
`$?` is the last stage's status. A command that is not found prints
`sh: name: not found` and counts as 127; not executable, 126; a
syntax error prints `sh: syntax error near 'x'` and counts as 2; a
redirection that cannot be opened prints `sh: path: <reason>` and the
stage is skipped with status 1.

## coreutils (`/bin`)

| Program | Usage | Notes |
|---|---|---|
| `echo` | `echo [-n] args...` | arguments joined by single spaces |
| `cat` | `cat [files...]`, `-` or no arguments reads handle 0 | 16 KiB copies; continues past a failing file, exits 1 |
| `ls` | `ls [-la] [paths...]` | sorted names; `-a` shows dot entries; `-l` prints type letter, mode bits, links, uid, gid, size, inode, name; several paths print headers |
| `cp` | `cp [-r] source... target` | into a directory target by base name; `-r` for directories |
| `mv` | `mv source target` | `rename`; on `EXDEV` copies and unlinks (files only); into a directory by base name |
| `rm` | `rm [-r|-rf] paths...` | refuses a directory without `-r` |
| `mkdir` | `mkdir [-p] dirs...` | mode 0755 |
| `rmdir` | `rmdir dirs...` | |
| `pwd` | `pwd` | `getcwd` |
| `true`, `false` | | 0 and 1 |
| `sleep` | `sleep seconds[.fraction]` | `nanosleep` |

Errors go to handle 2 as `name: path: <strerror>`; the exit status is 0
when everything succeeded, 1 otherwise, 2 for usage errors.

## system (`/sbin`)

| Program | Usage | Notes |
|---|---|---|
| `mount` | `mount [-r] source target fstype` | `-r` is `MS_RDONLY`; `source` is a block device name (`vda`) or `none` |
| `umount` | `umount [-f] target` | `-f` is `MNT_FORCE` |
| `ps` | `ps` | `procinfo`; columns `PID PPID UID S THR SYSCALLS TIME(ms) NAME`; `S` is `R` running, `X` exiting, `Z` zombie |
| `kill` | `kill [-sig | -s sig] pid...` | `sig` numeric or `KILL`, `TERM` (default), `INT`, `HUP`, with or without `SIG` |
| `dmesg` | `dmesg` | the newest 32 KiB of kernel log lines |
| `sysctl` | `sysctl -a` or `sysctl name...` | prints `name = value`; names from `sysctl.names` |
| `vmctl` | `vmctl probe`, `vmctl info`, `vmctl run [-m KIB] [-a GPA] [-e ENTRY] IMAGE` | `probe` prints the `/dev/vmm` line (exit 2 when the backend is `none`); `info` prints `hv.*`; `run` loads a flat image (default 1 MiB of guest memory, image at 0x1000, real-mode entry there) and runs one vCPU until `HLT`, echoing the guest's debug console and reporting other exits; exit 0 on `HLT`, 1 on `MMIO`/`SHUTDOWN`/`FAIL` (`docs/kernel-services/virtualization/api.md`) |

## Scripts (`/etc`)

`/etc/rc`: prints `CosmoOS userland ready` and, when `/etc/rc.test`
exists (`ls /etc/rc.test 2> /tmp/.rc.probe && sh /etc/rc.test`; the
probe file is removed afterwards), runs it. `/etc/rc.test`:
the shell's own test (`docs/userland/testing.md`), ending with
`SHTEST: PASS` or `SHTEST: FAIL n`.

## Serial-log contract (`tests/boot/run_boot_test.py`)

Lines the boot test requires from userland: `init: CosmoOS userland,
pid N`, `CosmoOS userland ready`, `init: rc exited with status 0`,
`interactive-ok` (typed by the harness), `init: shell exited with
status 0`; in self-test builds also `USERTEST: PASS` and `SHTEST: PASS`.
