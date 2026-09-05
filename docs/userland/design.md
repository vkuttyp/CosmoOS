# Userland: design

## Build and delivery

`userland/userland.mk` builds every program the same way:

```text
$(OUT)/userland/<name>.elf : crt0.o <objects> libc.a   linked with -T userland/user.ld (shared script; since Phase 11 the ELF and
                                                       program headers are inside the text segment and .note.cosmo sits in a PT_NOTE)
USER_CFLAGS = --target=x86_64 $(COMMON_CFLAGS) -fno-pic -fno-pie -mgeneral-regs-only -ffreestanding? no:
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
