# Userland: design

## Build and delivery

`userland/userland.mk` builds every program the same way:

```text
$(OUT)/userland/<name>.elf : crt0.o <objects> libc.a   linked with -T userland/user.ld (shared script; since Phase 11 the ELF and
                                                       program headers are inside the text segment and .note.cosmo sits in a PT_NOTE)
USER_CFLAGS = --target=x86_64 $(COMMON_CFLAGS) -fno-pic -fno-pie -ffreestanding? no:
              (no -mgeneral-regs-only: user code uses the FP/SIMD registers, which the kernel saves per thread)
              -nostdinc is NOT used (compiler builtin headers are wanted) but -nostdlib and
              -I libc/include -I kernel/include (for uapi/) -isystem <clang resource dir> are.
```

Programs are listed once (`USER_PROGRAMS := init sh echo cat ls ...`
with a directory per family); the archive entries are generated from
the list: `bin/<name>=$(OUT)/userland/<name>.elf`, `sbin/<name>` for
the system family, `etc/rc=userland/etc/rc`, and `etc/rc.test` only
when `SELFTEST=1`. `init` keeps its `init=` entry (the kernel finds it
by that name) and is also visible at `/boot/init`.

The kernel's boot namespace (`ramfs_populate_boot`) creates `/bin`,
`/sbin`, `/etc` and places archive entries whose first component is
one of those there with mode 0755 (`bin/`, `sbin/`) or 0644 (`etc/`);
all other entries go under `/boot` as before. The rule is the
bootstrap namespace policy of the archive (an initramfs-like
convention), documented in `docs/kernel/module/` (the archive) and
`docs/kernel-services/vfs/` (ramfs).

## init

```text
main(argc, argv):
  --selftest → selftest(); exit(failures ? 1 : 0)        (as before, now on libc)
  --crash    → *(volatile int *)0 = 1
  --block    → read(0, &c, 1); exit(5)                    (a killable console read, for the kernel test)
  --spin     → for (;;) ;                                 (a CPU-bound loop, for the kernel test)
  print "init: CosmoOS userland, pid N"                   (N is 1 only when no self-tests ran first)
  setenv PATH=/bin:/sbin:/usr/bin:/usr/sbin HOME=/
  if stat("/etc/rc") ok: pid = spawnvp("/bin/sh", {"sh", "/etc/rc"}); waitpid(pid); print "init: rc exited N"
  for (;;)
      pid = spawnvp("/bin/sh", {"sh"}, NULL, 0)          (inherits 0,1,2)
      if pid < 0: print error; exit(1)
      loop: w = waitpid(-1, &st, 0)                        (reaps orphans too)
            if w == pid: break
      print "init: shell exited with status N"
      exit(st)                                             (single-shell bring-up policy)
```

`init --selftest` grows checks for every new call (`proc_selftest` in
`init.c`; the full list is in `testing.md`): pipes with `dup`/`dup2`,
EOF and `EPIPE`; `spawn` of `echo` into a pipe and its status; `sh -c
"cd /tmp && pwd && exit 7"` for cwd inheritance and the status; a `cat`
blocked on a pipe killed with `SIGKILL` (137); `chdir`/`getcwd` with
`..` normalisation, `ENOTDIR`, `ENOENT`, `ERANGE`; `getppid() == 0`;
its own `procinfo` record; `klog_read`; `sysctl_get`; `fstat(0)` is a
character device and `isatty(0)`; and the hostile spawn requests (a
closed parent handle `EBADF`, a duplicate child slot `EINVAL`, a
non-executable file and a directory `EACCES`, a missing file `ENOENT`,
an empty `argv` `EINVAL`), `waitpid` with no children `ECHILD`,
`kill(999999)` `ESRCH`, `kill(pid, 0)` `EINVAL`, `dup2(h, 64)` `EINVAL`.

## The shell

### Structure (`userland/shell/sh.c`, one file)

```text
main           options (-c, -e), interactive loop (read(0) per line, prompt on handle 2) or script loop (fgets)
lex            line → tokens: WORD (quotes and escapes handled per character, $ expanded while lexing),
               PIPE, SEMI, AND_IF, OR_IF, LESS, GREAT, DGREAT, GREAT2 ("2>"), GREAT2AND ("2>&1"), END
expand_dollar  $VAR ${VAR} $? $$ $0-$9 $#; no splitting of results (recorded gap)
parse_pipeline tokens → struct pipeline { struct command cmds[16] }; command → words + redirs
run_line       walks the token list: pipelines separated by ; && || with skip logic, -e handling
run_pipeline   assignments alone → variables; a lone builtin runs in-process (redirected through dup/dup2
               when it has redirections); otherwise spawn each stage with a three-entry handle map
builtin        cd pwd exit export unset set : true false wait . source
var_*          shell variables (a flat array of 64); export moves them to the environment
```

### Running a pipeline

```text
run_pipeline(pl):
  nstage = pl->n; pids[nstage]
  prev_read = -1
  for i in stages:
     map = { {0, prev_read or 0}, {1, stdout}, {2, 2} }         (child slots 0,1,2 always given)
     if i < nstage-1: pipe(p); map[1].parent = p[1]; next_read = p[0]
     apply redirections: open files in the parent, override map entries; 2>&1 → map[2].parent = map[1].parent
     pids[i] = spawnvp(argv[0], argv, map, 3)
     close in the parent: the pipe ends and redirection files just passed (the child has its copies)
     prev_read = next_read
  for i: waitpid(pids[i], &st); $? = st of the last stage
```

Because the child receives exactly the mapped handles, the parent closes
its copies immediately after `spawn` and pipes reach EOF correctly. A
failed `spawn` (command not found) prints `sh: name: not found`, the
stage's status is 127, and the other stages still run (they see EOF).

Builtins that must affect the shell (`cd`, `export`, `exit`, ...) run
in-process only when they are the sole command; in a pipeline they run
in-process too (there is no fork), with their input/output redirected
by temporarily `dup2`-ing the shell's handles and restoring them: this
is what makes `pwd > file` work.

### Jobs, the terminal, and ^C

An interactive shell runs the terminal. At start-up `job_control_init`
does three things, and gives up on any of them failing -- a shell
started inside another shell's job has no terminal to take, and
commands still run, they just share the shell's group and its signals:

1. `setsid()`, unless it already leads a group. A session of its own is
   what makes this terminal *this* shell's.
2. `tcsetpgrp(0, getpgrp())`, which claims the terminal for the shell's
   own group.
3. `signal(SIGINT, SIG_IGN)` and the same for `SIGQUIT`. The shell is
   about to make each job the foreground group, and it has to still be
   here to print a prompt when the job dies of the interrupt.

Then every pipeline is one job and therefore one process group: the
first stage is spawned with `spawnvp_pgrp(..., 0)` -- a group named by
its own pid -- and the rest name that group, so `^C` interrupts the
whole pipeline rather than whichever stage happened to be reading. The
shell hands the terminal to the job with `tcsetpgrp` before waiting and
takes it back afterwards; both calls may fail on a job that has already
exited, and neither failure changes what happens next.

(Until job control existed, nothing observable distinguished a per-job
group from the shell's own group. It does now: a background job is in a
group that is not the terminal's foreground one, which is what stops it
from reading the line the shell is waiting for.)

Worth saying plainly about the original signals unit: with no background
jobs, **nothing observable distinguished a per-job group from the
shell's own group** -- with
either, the terminal's foreground group contains the job, and `^C`
reaches it. The per-job group is here because it is what the group model
is for, and because the moment `&`, `fg` or `bg` exists the shell's
group and the job's must differ. The mechanism itself is tested on its
own (`signal-group`); the shell's use of it is not separately
observable, and no test pretends otherwise.

A job that died of a signal is reported (`sh: sleep: segmentation
fault`) rather than left to look like an exit status, except for
`SIGINT` -- the terminal has already echoed `^C` -- and `SIGPIPE`, which
is how the second half of a pipeline tells the first to stop.

### Jobs proper

`&` puts a pipeline in the background; `jobs`, `fg` and `bg` manage what
is running. A job is remembered by its process group and its stages:

- A job takes a **number only when it outlives the foreground** -- when
  it is backgrounded, or when it stops. Numbering every pipeline would
  print `[9]+ Stopped` on the ninth command of the session, which is not
  what a job number means.
- `^Z` stops the foreground job; the shell waits for **every** live
  stage to report stopped before it prints `[1]+  Stopped` and takes the
  terminal back. Returning on the first would hand the terminal to the
  shell while the other stages were still on their way to parking, and
  they would write over the prompt. `fg` hands the terminal over, sends
  `SIGCONT` to the *group* so every stage resumes together, and waits
  again; `bg` continues it without the terminal.
- A stopped foreground job is not reported as a death: `job_wait_foreground`
  answers `JOB_STOPPED` (128 + `SIGTSTP`, which is what `$?` is after a
  `^Z`) and the caller tests for it by name before calling
  `report_signal`.
- Background jobs are collected before each prompt with
  `waitpid(-1, WNOHANG | WUNTRACED | WCONTINUED)`, which is where a
  shell reports `Done` and `Stopped`.
- The shell ignores `SIGTTOU`, `SIGTTIN` and `SIGTSTP` as well as
  `SIGINT` and `SIGQUIT`. `SIGTTOU` is the load-bearing one: taking the
  terminal back after a job is done from the background, so a shell that
  did not ignore it would stop itself every time a job finished.

Without job control -- a non-interactive shell, or one that could not
claim a terminal -- `&` says so and runs the pipeline in the foreground
rather than pretending to background it.

### Line editing

An interactive shell reads its line in **raw mode** and draws it itself,
which is the only way to have arrow keys or history: in canonical mode
the kernel owns the line and hands it over only when it is finished.
Left and right arrows move the cursor, up and down walk a
thirty-two-entry history, `^W` erases a word, `^U` the line, `^A`/`^E`
jump to its ends, and `^C` abandons it and prompts again.

Three details are load-bearing:

- **An escape sequence is consumed whole, and an `Escape` that is not
  the start of one gives its next byte back.** A CSI sequence is
  parameter bytes, then intermediates, then one final byte in
  `0x40`-`0x7e`. Reading a single byte after `[` was right only for the
  four arrow keys and left the tail of everything else on the line, so
  Delete (`Esc [ 3 ~`) typed a `~`; the shell now reads to the final
  byte and ignores what it does not know. A byte that no CSI sequence
  can contain -- a control byte, Enter among them -- ends the sequence
  and is given back, so an unfinished `Esc [` cannot swallow the Enter
  that would have submitted the line. A byte after `Escape` that is
  not `[` is not part of a sequence at all, so it is handled as an
  ordinary keystroke -- `Escape` then `x` leaves an `x` on the line,
  where the first version silently ate it. The reads inside a sequence
  do block: with no `VTIME` there is no way to wait a moment for the
  rest and give up, so `Escape` alone waits for the next key. That is
  recorded as part of the `VTIME` gap rather than solved.
- **Typing at the end of the line echoes one character; only an edit
  that moves text about redraws.** A redraw is a carriage return, the
  prompt and the line, so redrawing on every keystroke would print a
  prompt per character -- and anything reading the serial log, the boot
  harness included, counts prompts to know when the shell is ready.
- **It falls back to a plain `read`** when the terminal cannot be put
  into raw mode: a script, a pipe, or a shell without job control. A
  shell that required a terminal would not be able to run `/etc/rc`.

The modes are restored before every command runs, so a job inherits a
cooked terminal and `^C` reaches it through the kernel as before. What
the shell does not know is the terminal's width, so a line longer than
the screen wraps in the terminal's own way and the redraw does not
account for it -- recorded rather than solved, because solving it means
tracking the cursor across rows.

### Input

Interactive: write the prompt to handle 2, `read(0, line, 1023)` (the
tty delivers one line, at most 1024 bytes per call; a longer typed line
arrives in pieces that run as separate lines, a recorded limit), strip
the newline; EOF (0 bytes) prints a newline and exits with the last
status. Script: `fgets` on the file. `-c`: the string is the whole
input.

### Errors

Syntax errors print `sh: syntax error near 'x'` and set `$? = 2`. A
command that cannot be spawned: 127 (not found) or 126 (not
executable). `-e` exits on any non-zero status outside `&&`/`||`
contexts.

## Utilities

Every utility follows the same shape: parse options by hand (a
`while (argc > 1 && argv[1][0] == '-')` loop; no `getopt` yet), do the
work with libc, `perror`-style messages `name: path: strerror`, exit 0
on success and 1 if anything failed. Buffers are 16 KiB stack arrays
for copies. `ls` sorts names with `qsort`. `cp -r`/`rm -r` recurse with
`opendir` (depth bounded by `PATH_MAX`). `mv` tries `rename`, falls
back to copy plus unlink on `EXDEV`. `ps` reads `procinfo` twice if the
first buffer was too small. `kill` accepts `-9`, `-KILL`, `-s KILL`.
`dmesg` writes the `klog` buffer. `sysctl -a` lists `sysctl.names` then
each value as `name = value`.

## Test script (`userland/etc/rc.test`)

Straight-line shell (no control flow exists): every check is a command
whose failure is detected with `||`:

```sh
echo hello | cat > /tmp/t1 || echo "SHTEST: FAIL pipe"
cat /tmp/t1 | cat | cat > /tmp/t2 || echo "SHTEST: FAIL pipe3"
...
false; test "$?" ...   (no `test`: use the utilities' own exit codes and `&&`/`||`)
echo "SHTEST: PASS"
```

Two sections depend on fixtures only the x86-64 build carries: the Linux
programs run through `sh /etc/rc.linux` only if `/boot/tests/linux/lxhello`
exists, else the script prints `LINUXTEST: skipped`; `vmctl probe` fails
without `/dev/vmm` and the script prints `HVTEST: skipped`. The harness
requires the `skipped` lines on AArch64 and forbids them on x86-64.

Since there is no `test` utility, checks compare through behaviour:
commands that must succeed carry `|| FAILS=1`, commands that must fail
carry `&& FAILS=1` (`rm` of a file that must not exist, `ls` of a
removed directory, `cat` of a missing file, `kill` of a bad pid), and
outputs are re-read with `cat` into the log where a person can see
them. The last line, `sh -c "exit $FAILS" && echo "SHTEST: PASS" ||
echo "SHTEST: FAIL $FAILS"`, prints the verdict; the harness requires
`SHTEST: PASS` in self-test builds. The exact sequence is in
`testing.md`.

## Interactive harness (`tests/boot/shelltest.py`)

Started by `run_boot_test.py` for normal runs (never for the panic
run): QEMU's stdin is a pipe; a thread follows the serial log; when it
holds one more `cosmo$ ` prompt than commands sent it writes the next
command from `COMMANDS` (`echo interactive-ok`, `ls /bin`, `ps`, an odd
`echo $((`, `pwd`, `cd /tmp && pwd && cd /`, `sysctl kernel.name`,
`dmesg`, `nosuchprogram`, `exit 0`) with a `\n`; the run must then end
through init's exit. Each command has patterns the log must contain
(`testing.md`); `run_boot_test.py` also requires `^interactive-ok$`,
`^init: shell exited with status 0`, `^init: CosmoOS userland, pid \d+`
and, in self-test builds, `^SHTEST: PASS`.

## Services (`userland/system/svc.c`)

Constitution section 55 asks for start, stop, restart, dependencies,
logging, a restart policy, resource limits and supervision, and says not
to reproduce systemd. What it describes is daemontools' shape, and that
is what this is.

**There is no service daemon.** One supervisor process per service, and
the state is in the filesystem. A central manager would need a control
channel, and the two Unix answers to that -- a named pipe and a unix
socket -- are both things this kernel does not have (`mknod` is not in
`vnode_ops`, and sockets are AF_INET). Inventing a third would mean
building an IPC mechanism in order to build a service manager, which is
backwards. It also means a supervisor that dies takes one service with
it rather than all of them, and that `svc` is an ordinary program with
no privileged position: what it knows, `cat` can read.

```text
  /etc/svc/<name>        the service: key value lines, one per line
  /run/svc/<name>.pid    the supervisor's pid, once the service has run
  /var/log/svc/<name>    the service's output and its supervisor's notes
```

A definition is `key value`, no sections and no expressions:

```text
  exec /sbin/thing -f        what to run (required)
  after net time             start these first
  restart on-failure         never (default) | on-failure | always
  retries 5                  give up after this many restarts (default 5)
  backoff-ms 100             wait this long, doubled each retry, capped at 5s
  user 1000                  and group, via COSMO_SPAWN_SETCRED
  root /srv/thing            confine it there (COSMO_SPAWN_SETROOT)
  mountns yes                a mount namespace of its own
  utsns yes                  a uts namespace of its own
  domain yes                 a process domain of its own
  limit-nofile 32            any COSMO_RLIMIT_* by name
```

**A definition that is not understood fails the service**, and that
covers more than unknown keys. A typo in `root` or `user` would
otherwise leave a service running with more authority than its author
wrote down, and the file is the only place that authority is stated.
So: an unknown key is an error; a number that is not a number is an
error -- `user daemon` through `atoi` is uid 0, which makes a typo in
the one key that reduces privilege grant the most of all; a limit that
cannot be set stops the service rather than running it unrestricted;
and an unreadable definition is fatal to that service rather than a
default.

The same rule applies to reporting. `svc start` says a service started
only when it has seen it running or seen its supervisor finish having
run it -- reporting success without looking would make `svc boot` start
the dependents of a service that never ran. And more definitions than
`svc` can hold is a message naming the first one left out, not silence,
because which ones were dropped would otherwise depend on the order the
directory happened to be read in.

This is where the container primitives earn their place: a service is
confined by naming it in a file, and `svc` does no more than turn those
lines into the flags that already exist (`docs/kernel/security/design.md`
sections 1--1f).

### Supervision

`svc --supervise <name>` is the supervisor: it spawns the service, waits
for it, and decides. It writes its pid file only after the first spawn
succeeds, because that file is what `svc start` reads as "the service is
up" -- written earlier, a spawn that then failed would look like a
running service for as long as it took to fail. `svc start` spawns that and returns, so the
supervisor is reparented to init, which reaps it -- there is no fork
here, and none is needed.

The restart policy is bounded in both directions. `never` and a clean
exit under `on-failure` mean the supervisor exits with the service.
Otherwise it waits `backoff-ms`, doubles it each time up to five
seconds, and gives up after `retries` restarts, writing why. **A service
that dies instantly must not spin the machine**, which is what a
supervisor without a backoff and a limit does; and a supervisor that
gives up must say so where somebody will find it, which is the log.

`svc stop` is two kills, and their order matters. The supervisor goes
first, or it would see its service die and start another one -- killing
the service alone is a restart, not a stop. Then the service itself,
whose pid the supervisor records in `/run/svc/<name>.child` for exactly
this purpose.

The supervisor does not do the second kill, and that used to be a fact
about this kernel rather than a choice: `svc` installs no signal
handler, so being told to stop kills it where it stands with no chance
to tidy up. Since the signals unit a native process *can* catch
`SIGTERM`; teaching the supervisor to is a change to `svc`, which
nothing has asked for yet.
`svc` therefore removes the pid files as well. The first version of this
did signal only the supervisor, and a `sleep 30` service outlived its
own stop by half a minute.

### Order

`svc boot` reads every definition, sorts by `after`, and starts in that
order. A cycle is refused and named. A service whose dependency did not
start is not started either, and says which one -- a dependency that is
ignored when it fails is a dependency in name only.

Order here is *start order*, not readiness: `svc` knows a service is
running, not that it is ready to serve. Readiness needs the service to
say so, which needs a channel, which is the thing this design does
without. Where that matters the dependent must retry, which it must do
anyway on a machine where anything can restart.

Not done, and each for a reason: socket activation (there are no unix
sockets to activate on), a syscall filter per service (it would need a
name-to-number table in userland that nothing else wants yet), timers,
and any readiness protocol.

## Security

Programs are uid 0 like everything else so far. The shell passes only
the handles a child needs. Paths from the command line go to the kernel
which validates them. Nothing in userland trusts input length: the
shell's line is bounded (1024, the tty's limit), words are bounded by
the line, the argument vector by `ARG_MAX`.

## Future extensibility

- Control flow, functions, globbing, command substitution in `sh`
  (the parser produces a tree already; `expand.c` gains word splitting
  and pathname expansion; `$(...)` needs a pipe and a spawn of `sh -c`).
- Services in init: a directory of service descriptions, dependency
  order, restart policy, a control socket; `init` becomes the
  supervisor and `getty`/login appear with users.
- `userland/networking/`: `ping`, `ifconfig`, `nc` on the libc socket
  API; `dhcp` when the kernel has UDP broadcast on eth0 configured.
- Ports (section 48) bring real coreutils; the hand-written ones remain
  the rescue set.
