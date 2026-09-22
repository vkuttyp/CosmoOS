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

Listed in `docs/kernel/process/testing.md` (`process-user`), which also
records what each section costs: the suite times every section and
prints `USERTEST: section <name> <ms> ms` plus a total, so a slow one is
named rather than the whole run (invariant **F13**). In short:
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
Since the unix-sockets unit a `unix` section (`unix_selftest`): a
stream pair carries bytes both ways and `SO_PEERCRED` names this
process; a child echoes over the end it was given through the spawn
map and answers a message carrying a file handle with the file's bytes;
`sendmsg` of a handle without TRANSFER is `EPERM` and of a unix socket
`EINVAL`; a listener bound at `/tmp/ux-sock` is a `DT_SOCK` node of mode
0755 that `open` refuses `ENXIO`, a child connects to it and the
accepted socket's `SO_PEERCRED` is the child's pid, `getsockname` is
the path and a second bind `EADDRINUSE`; a uid-1000 child is refused
`EACCES` by the node's mode; a jailed child (`spawnve_in`) reaches
neither the parent's abstract name (`ECONNREFUSED`) nor its path
(`ENOENT`) while an unjailed connect to the abstract name works; the
name outlives the socket (`ECONNREFUSED`) until `unlink` (`ENOENT`);
datagrams by name with the sender's abstract name back through
`cosmo_recvmsg` and `MSG_TRUNC`; `SOCK_SEQPACKET` `ESOCKTNOSUPPORT`; and
`USERBENCH: unix`, a one-byte round trip to a child over a stream pair
against the same over two pipes. Since the file-regions unit an `mmap` section (`mmap_selftest`): a
`MAP_SHARED` mapping of a `/tmp` file is coherent with `read()` and
`write()` in both directions with no `msync` between; a spawned child
maps the same file shared and writes a byte the parent reads (**the
first shared memory between two processes in this system**); a
`MAP_PRIVATE` mapping's written page is a copy (`vm.file_cow_faults`
+1, the file and the shared mapping untouched) while its unwritten page
still shows a later `write()`; the offset; `msync`'s rules (`SYNC|ASYNC`
and an undefined bit `EINVAL`, an unmapped page `ENOMEM`, an unaligned
address `EINVAL`, an anonymous range 0); the native rules a program can
probe (neither or both of `SHARED`/`PRIVATE`, `SHARED|ANONYMOUS`, an
unaligned offset, an undefined bit `EINVAL`; a bad fd `EBADF`; a
directory `ENODEV`); a read-only fd (`MAP_SHARED|PROT_WRITE` `EACCES`,
a private writable mapping fine, `mprotect(PROT_WRITE)` of its shared
mapping `EACCES`, `PROT_NONE` and back allowed); a read/write handle
duplicated with `COSMO_RIGHT_READ` alone (`cosmo_dup_rights`): the same
two refusals, the rights and not only the mode deciding; four children judged by
status -- `mmap-past-end` (a two-page mapping of a five-byte file: the
second page is `SIGBUS`, 135, not zeros), `mmap-truncate` (three pages
mapped and installed, the file reopened `O_TRUNC`, the touch is
`SIGBUS`, not the old bytes), `mmap-as-limit` (`COSMO_RLIMIT_AS` lowered
to a page: a file mapping is `ENOMEM`), `mmap-mem-limit` (a private page read, then
`COSMO_RLIMIT_MEM` set to 0, then written: the copy is refused and the
touch is fatal, 139 -- the check review found missing on the replace
path), `mmap-cycle` (200 map/write/unmap cycles, shared and private;
its exit runs the kernel's `file_pages == 0` check by construction); a
`write()` into a page mapped `PROT_READ|PROT_EXEC` shared runs the
kernel-alias instruction-cache sync (`vm.cache_exec_syncs` +1; a
regression check, TCG cannot show coherence); **the futex keyed by what
the word maps** (the shared-futex unit): a child (`mmap-futex-wait`)
maps the file shared and sleeps on a word in it, the parent counts it
asleep through a requeue of the word onto itself -- a count that
crosses the process boundary only if the key does -- then writes and
wakes it (1 woken, the child exits 0: the first wait across two
processes in this system); the wake before the sleep (0 woken, the
child's wait `EAGAIN`); a private mapping's word at the same offset is
neither counted nor woken by the shared key and is by its own; a thread
asleep on a second shared mapping's word whose mapping is then unmapped
times out cleanly; a waiter on the shared word requeued onto a private
word and woken there, the counts moving with it; private waiters
requeued onto a shared mapping's word and the mapping then unmapped
(they time out, the reference each moved waiter was given outliving it); a
double requeue shared (file A) -> private -> shared (file B) with file A
unmapped, closed and unlinked in between: its one page leaves the cache
(`vm.cache_pages` -1, the exchanged reference having let it go) and the
wake through B finds the waiter; `USERBENCH: futex` (50 000 wakes with
no waiter before any shared mapping exists and again with one, and 500
cross-process round trips through two shared words with
`mmap-futex-pingpong`); on cosmofs (`vda` at `/mnt`), a write through a
shared mapping dirties by one fault, `MS_SYNC` writes exactly that page
(`vm.cache_writebacks` +1), a second `MS_SYNC` writes nothing, a second
write faults again (the PTE was lowered) and the third `MS_SYNC` writes
it again; the bench (`USERBENCH: mmap ...`: `read()` of a cached 2 MiB
file, the first touch of every page through a shared mapping, and the
dirtying write of every page); and at the end, the files gone,
`vm.cache_pages` back at its start.

## `/etc/rc.test`

Straight-line shell (no control flow exists); `FAILS` is set to 1 by
`||` on a command that must succeed or by `&&` on a command that must
fail, and the last line runs `sh -c "exit $FAILS"` to print the verdict.
That line is `A && B || C`, and until the shell's AND-OR lists were made
left-associative it could print only `SHTEST: PASS`: on a failing run
neither branch ran, so the `SHTEST: FAIL n` this table promises was
unreachable and a failure showed up only as a missing marker. The script
now asserts the shape it depends on -- both branches of `false && … || …`
and of `true && … || …`, each checked with a two-term list that means the
same thing under either parse.
What it covers, in order: `mkdir -p` of a nested path; `>` and `>>`;
one-, two- and three-stage pipelines through `cat`; `cat` of the
results (the log shows `hello` and `hello` then `world`); `cp` into a
directory and `cp -r`; `ls` of the copy; `mv`; `rm` of a moved-away
file fails; `rm -r`; `ls` of a removed directory fails; a named pipe
(the named-pipes unit): `mkfifo`, then `sh -c "echo via-fifo > fifo" |
cat fifo` -- the two commands of a pipeline start together and meet on
the name, the inner shell's open waiting for `cat`'s, and the log shows
`via-fifo` (a required marker; the redirection sits inside `sh -c`
because this shell opens a command's redirections before it spawns,
and a trailing `&` runs in the foreground without job control);
`false && ...` and `true || ...`; `false; echo "status $?"` (prints `status 1`);
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
| `sleep 5` then a bare `0x03` | `\^C` (the terminal's echo); the job exits 130 and the next prompt arrives without waiting out the five seconds |
| `echo after-interrupt-ok` | `^after-interrupt-ok$` (the shell survived the interrupt it sent to the job) |
| `pkg install hello && hello && pkg list` | `^hello, world \(hello 1\.1\)$`, `^hello\s+1\.1\s+prints a greeting$` (`docs/pkg/testing.md`) |
| `exit 0` | the run ends: `init: shell exited with status 0` |

The interrupt is the one entry that is not a line: it is sent as a raw
byte half a second after the command it interrupts, without waiting for
a prompt -- the shell is waiting for the job, not for a line, and that
is the only state in which the byte means anything. It is also the only
entry that does not advance the prompt count.

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
