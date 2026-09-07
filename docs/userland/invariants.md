# Userland: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**U1. A child receives exactly the handles its parent maps, and the
parent closes its own copies of what it handed over.** The shell builds
a three-entry map (0, 1, 2) per pipeline stage from the pipe ends and
redirection files it opened, spawns, then closes the pipe ends and the
files it opened; init spawns with the default map (0, 1, 2 inherited).
Because the kernel installs only mapped handles, a pipe reaches end of
file when the last stage that had its write end exits. Check:
`/etc/rc.test` three-stage pipelines complete; `init --selftest` reads
EOF from a pipe whose only other writer was a spawned `echo`. Gap: no
test hands a child more than three handles.

**U2. Builtins that change the shell run in the shell; everything else
runs in a child.** `cd`, `export`, `unset`, `set`, `exit`, `.` affect
the shell's own state, so they never spawn; when redirected they run
with the shell's handles temporarily replaced through `dup`/`dup2` and
restored afterwards. A program is never mistaken for a builtin: the
builtin names are a fixed list. Check: `/etc/rc.test` (`cd /tmp/shtest
&& pwd`, `cd dir`, `cd ..`, `export Y=hello; sh -c 'echo "exported
$Y"'`); `init --selftest` confirms a child's `cd` does not move the
parent. Gap: `pwd > file` (a redirected builtin) has no automated
check.

**U3. Exit statuses mean what Unix says.** 0 success; a utility's 1 for
a failed operation and 2 for usage; the shell's 127 for a command not
found, 126 for not executable, 2 for a syntax error; `exit n` gives
`n & 0xff`; a killed child reports `128 + sig`. `$?` is the last
pipeline's last stage. Check: `/etc/rc.test` (`false; echo "status $?"`
prints 1, `sh -c "exit 3"` prints 3, a missing command prints 127);
`init --selftest` (`exit 7` seen through `waitpid`; a killed `cat`
reports 137). Gap: none.

**U4. The boot ends when init's console shell exits, with the shell's
status.** init runs `/etc/rc` to completion first, then one shell, and
returns that shell's status; the kernel logs `init exited with status N`
and shuts down with success only for 0. Check: the boot test requires
`init: shell exited with status 0` and the success exit code after the
harness types `exit 0`. Gap: this single-shell policy is the bring-up
choice; a respawn or service supervisor replaces it and this invariant
with it.

**U5. init reaps everything it is given.** While waiting for the shell,
init waits for any child and loops until the shell's own pid is
returned, so orphans reparented by the kernel are collected. Check:
`process-spawn` and `init --selftest` cover reparenting and reaping at
the kernel level; the shell's pipelines (children of `sh`, not of init)
are reaped by the shell. Gap: no test creates an orphan under the real
init (a grandchild outliving its parent).

**U6. Output that the tests depend on is flushed before the process
ends or hands over.** Programs return from `main` (which flushes) or
call `fflush(stdout)` before `_exit` or before spawning a child that
writes to the same handle; the shell flushes before every pipeline and
before printing the prompt. Check: the serial log's line order in
`make test` (the harness requires prompts to appear after the previous
command's output). Gap: none known.

**U7. Nothing in userland is trusted with kernel facts it did not get
from the kernel.** `ps` shows only what `procinfo` returns, `dmesg` only
`klog_read`, `sysctl` only `sysctl_get`; init's pid is whatever the
kernel gave it (not assumed to be 1) and `getppid` of init is 0. Check:
`init --selftest` (`getppid() == 0`, its own record in `procinfo`).
Gap: none.

## Gaps (documented, not invariants)

- Every process runs as uid 0; the kernel's uid checks (`kill`,
  `mount`, privileged ports) are enforced but never fail in practice.
- No control flow, globbing, background jobs, job control,
  here-documents, command substitution or arithmetic in the shell.
- No `getopt`; options are parsed by hand and must precede operands.
- No service supervision, `getty`, login, users.
- The utilities are a rescue set; the ports system (Phase 10) brings
  real ones.

**U8. A service that keeps dying stops being restarted, and says so.**
The supervisor waits `backoff-ms` before a restart and doubles it each
time to a five-second cap, and gives up after `retries` restarts. A
supervisor without both of those turns a service that fails instantly
into a machine that does nothing else, and one that gives up silently
leaves a dead service looking like a running one. Giving up is written
to the service's log with the reason, and the supervisor then exits, so
its pid file going away is what says the service is not coming back.

Check: the self-test runs a service that always fails with
`restart on-failure`, `retries 2` and a short backoff, and requires the
supervisor to exit on its own, the log to record two restarts and the
reason it stopped, and the elapsed time to be at least the two backoffs
-- so a supervisor that restarted without waiting would fail it.

**U9. What a service is allowed is what its file says.** A definition is
`key value` lines and an unknown key is an error, which fails that
service rather than starting it. The keys that reduce authority --
`user`, `root`, `mountns`, `utsns`, `domain`, `limit-*` -- turn into the
spawn flags and limits of `docs/kernel/security/design.md` and nothing
else; `svc` grants no authority of its own. A typo in one of them would
otherwise leave a service running with more than its author wrote, and
the file is the only place that is stated.

Check: the self-test requires a definition with an unknown key to fail
and the service not to run; requires a service given `root` to be unable
to name a file outside it; and requires `limit-nofile` to bind on the
service, which it inherits.

**U10. A dependency that fails stops what depends on it.** `svc boot`
starts in `after` order, refuses a cycle and names it, and does not
start a service whose dependency did not start -- reporting which one.
A dependency that is ignored when it fails is a dependency in name only,
and the failure it hides is exactly the one worth seeing.

Check: the self-test requires a two-service chain to start in order,
requires a cycle to be refused rather than run in some order, and
requires that when a dependency's definition is bad the dependent is not
started and the message names the dependency.
