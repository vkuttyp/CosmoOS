# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the seventh such
report (the NIC, the USB host stack, AHCI, the machine's own console,
floating point, and the signals a person can send; all six record their
outcomes). Nothing in this one is implemented.

**Subsystem: job control — `^Z`, a process that can be stopped, and a
shell that can run more than one thing.**

## Problem

The signals unit gave this machine `^C`. A person can sit at the
keyboard, start a command, and interrupt it. What they cannot do is
*keep* it: there is no `^Z`, no `&`, no `fg`, no `bg`, and no `jobs`.
One command at a time, and the only way to be rid of one is to kill it.

The reason is a single missing idea — **a process that is neither
running nor ending** — and the tree records the consequence in four
places:

- **The stop signals are ignored.** `kernel/process/signal.c:33` says
  why, and it is the honest reason: "No job control: the stop signals
  cannot stop anything and must not kill anything (audit finding #30);
  they are ignored, a recorded deviation from Linux." `SIGSTOP`,
  `SIGTSTP`, `SIGTTIN` and `SIGTTOU` all sit in
  `signal_default_is_ignore` next to `SIGCHLD`.
- **`SIGCONT` is ignored too**, for the symmetric reason: there is
  nothing to continue.
- **`^Z` is dropped in the line discipline.** The signals unit taught
  `tty_input` about `^C` and `^\` and left `0x1a` in the "other control
  bytes are dropped" case, because sending `SIGTSTP` to the foreground
  group would have been sending a signal that is defined to do nothing.
- **The shell waits for every job it starts.** `run_pipeline` spawns,
  hands the terminal over, and calls `waitpid` on each stage. There is
  no job table and no `&` — `userland/shell/sh.c:11` has said "no
  background jobs" since it was written.

Underneath, `struct process` has `enum process_state { PROCESS_RUNNING,
PROCESS_EXITING, PROCESS_EXITED }` and nothing between the first and the
second, and `process_wait_child` reports a child exactly once, when it
has become a zombie.

## Current implementation

Almost everything this needs is already here, which is why it is the
next unit rather than a large one.

- **The signal core** delivers at a return to user mode and nowhere
  else (`signal_deliver`, from `syscall_dispatch` and
  `process_return_to_user`). That is precisely where a process may be
  stopped safely: no kernel lock is held, no half-finished operation is
  in flight, and the register set is already saved. The hard question a
  stopped state usually raises — *where is it safe to freeze a thread?*
  — is already answered by the machinery the signals unit uses.
- **`UNBLOCKABLE` is `SIGKILL | SIGSTOP`.** The core already refuses to
  let a process block or catch either, so the one rule that makes
  `SIGSTOP` trustworthy is enforced before anything uses it.
- **Process groups and sessions** landed with the signals unit, along
  with `process_group_next` for walking a group, and the terminal knows
  its foreground group. `^Z` has somewhere to go the moment it means
  anything.
- **Killable waits already wake for a pending signal.**
  `wait_event_killable` calls `process_kill_pending()`, which is
  `signal_pending()` — true for any deliverable signal. A thread blocked
  in `read` will already come out of the wait when a stop signal is
  posted; it just has nothing to do when it gets to the return path.
- **`waitpid` blocks on the parent's `child_wq`** and the child's exit
  wakes it. A stop can use the same queue.
- **The Linux personality already translates wait statuses.**
  `compat/linux/convert.c:64`, `lx_wait_status`, turns the native status
  into Linux's `(status << 8)` / bare-signal encoding. This is the fact
  that settles this unit's design question, below: the native encoding
  is the tree's own and nothing outside the tree reads it.

What is missing is the state itself, the two ends of the terminal rule
(`SIGTTIN`, `SIGTTOU`), a way for `waitpid` to say "stopped" rather than
"exited", and a shell that keeps a list.

## Why it matters

1. **The machine can run exactly one thing.** That is the plain user
   consequence. A person who starts a long build and wants a prompt has
   to kill the build. Everything the tree has built to run more than one
   process — groups, sessions, a terminal that knows its foreground —
   exists to support a distinction the shell cannot yet make.
2. **Audit finding #30 is still open**, and this is the last piece of
   it. The finding was recorded as a deliberate deviation, and the
   signals unit deliberately did not close it; leaving it open
   indefinitely turns a deviation into a decision nobody made.
3. **`SIGTTIN` is a correctness problem, not a convenience.** The moment
   `&` exists, a background job that reads the terminal steals the
   shell's input. POSIX's answer is to stop it. Without that answer,
   background jobs and a usable terminal are mutually exclusive, so this
   unit cannot ship half of itself.
4. **It is the last thing between this shell and a usable one.** After
   it, the shell's gaps are features (control flow, globbing, command
   substitution) rather than a missing kernel capability.

## Proposed design

Five steps, in dependency order. The wait-status question in step 3 is
the one the tree should settle before any of it is written.

### 1. A stopped process

A process gains, under `p->lock`:

```c
bool stopped;        /* parked at a return to user mode */
int stop_sig;        /* what stopped it, for the wait status */
struct waitqueue stopped_wq;
```

**Not a fourth value in `enum process_state`.** The enum today means
"alive / on its way out / gone", and every `state != PROCESS_RUNNING`
test in the tree means "on its way out" — `route_locked` discards
signals to such a process, `process_kill` refuses to touch one,
`find_reapable_locked` looks for `PROCESS_EXITED`. A fourth value would
silently change all of them, and the most damaging change would be
`route_locked` discarding the `SIGCONT` that is the only way out. A
separate flag leaves every existing test meaning what it says.
`cosmo_procinfo.state` gains a fourth value so `ps` can print `T`.

**Where a process stops** is `signal_deliver`, beside the handler case:
a stop signal whose action is the default sets `stopped` and parks the
calling thread on `stopped_wq`. Every thread of the process parks as it
reaches its own return to user mode; threads blocked in a killable wait
are woken by the same `sched_wake` the signal core already does, come
out with `-EINTR`, and park at the syscall's return. Nothing is frozen
mid-kernel, which is the property that makes this safe and is inherited
free from where signals are delivered.

**`SIGCONT`** clears `stopped`, wakes `stopped_wq`, and discards any
pending stop signal; posting a stop signal discards a pending `SIGCONT`.
Both leave the ignore table: `SIGCONT`'s default becomes *continue*, and
the four stop signals' default becomes *stop*. `SIGKILL` un-stops and
kills — a process that could not be killed while stopped would be worse
than one that cannot stop.

### 2. `^Z`, `SIGTTIN` and `SIGTTOU` at the terminal

`tty_input` gains `0x1a` beside `^C` and `^\`, sending `SIGTSTP` to the
foreground group through the same path the signals unit built (recorded
under the lock, sent once the lock is dropped, one signal at a time so a
batch cannot collapse).

`tty_read` gains the rule that makes background jobs survivable: a
reader whose process group is not the terminal's foreground group is
sent `SIGTTIN` and stops, rather than stealing the line the shell is
waiting for. Writes are allowed, as they are on Linux by default;
`TOSTOP` is not built.

**The orphaned-group rule is not optional.** POSIX says a process group
whose members' parents are all outside the session and dead is
*orphaned*, and that stopping it would leave it unreachable — nothing
survives to continue it. Such a group gets `-EIO` from the read instead
of a `SIGTTIN`, and is never sent `SIGTSTP` from the terminal. Without
this rule a stopped orphan is a wedged machine, so it is part of the
step rather than a refinement of it.

`tcsetpgrp` from a process that is not in the terminal's foreground
group should raise `SIGTTOU` on the caller — it does not today, and it
is a two-line rule that keeps a background process from silently taking
the terminal.

### 3. What a wait status is — the architectural question

Today the status is the exit status itself: `0..255` from `exit`,
`128 + sig` from a kill, `139` for a fault, with `WIFEXITED(s)` reading
`s < 128`. That encoding is full. It has no room to say *stopped by
signal N* or *continued*, and this unit must say both.

Three answers, in the order this report ranks them:

- **Extend the native encoding upward.** Keep `0..255` and `128 + sig`
  exactly as they are, and put the new outcomes above them —
  `0x200 | sig` for stopped, `0x400` for continued. Nothing that exists
  changes meaning: every `status == 0` in the tree's tests, every
  `status == 128 + sig`, every recorded status in the docs stays true.
  `sys/wait.h` gains `WIFSTOPPED`, `WSTOPSIG` and `WIFCONTINUED`, and
  `WIFEXITED`/`WIFSIGNALED` gain an upper bound so they cannot claim a
  stopped status. `lx_wait_status` gains two cases.
- **Adopt Linux's encoding** (`(status << 8)`, `sig`, `(sig << 8) |
  0x7f`, `0xffff`). The model rather than the extension — and here the
  model buys nothing, because **the thing it would buy is already
  built**: `lx_wait_status` translates for Linux binaries, so no program
  outside this tree ever sees the native encoding. What it costs is a
  breaking change to a working ABI and to every caller and test that
  reads a status.
- **A separate out-parameter** on `sys_wait` saying exited / signalled /
  stopped / continued, leaving `status` alone. No breakage, but it makes
  the libc synthesise POSIX macros from two values, and it invents a
  shape for the one case where a well-known one exists.

The signals unit's session question resolved toward the model because
the model was *cheaper*. This one resolves the other way for the same
kind of reason: the extension is cheaper and the model's benefit is
already provided by a translation layer that exists. That asymmetry is
the argument, and the report is the place to make it rather than
discover it halfway through.

One wart to name and *not* fix: `WIFSIGNALED(139)` is false today,
because `COSMO_EXIT_FAULT` is `128 + SIGSEGV` and the macro excludes it
so that a program calling `exit(139)` is not misread as having been
killed. The ambiguity is in the encoding, not the macro, and this unit
does not remove it.

### 4. `waitpid` learns to report a stop

`WUNTRACED` reports a stopped child without reaping it; `WCONTINUED`
reports a continued one. Both are edge-triggered — a stop is reported
once per stop, so the child carries a "reported" bit that the stop
clears and the report sets, or a parent polling in a loop would see the
same stop forever.

A stop wakes the parent's `child_wq`, the same queue an exit wakes, so
`process_wait_child` grows a case rather than a mechanism. `SIGCHLD` is
sent to the parent on a child's stop and continue as well as its exit —
today it is never sent at all, which is a gap in its own right and one
this unit closes because a shell that wants to notice a stopped job
without blocking needs it.

### 5. The shell

`&` at the end of a pipeline, a job table, and `jobs`, `fg`, `bg`. The
shell already puts each pipeline in its own process group and hands the
terminal over, so the machinery is in place; what it gains is not
throwing the job away after `waitpid`.

- `^Z` stops the foreground job; the shell sees `WIFSTOPPED`, prints
  `[1]+ Stopped   sleep 30`, takes the terminal back, and prompts.
- `fg %1` puts the job's group back in the foreground, sends `SIGCONT`
  to the group, and waits for it again.
- `bg %1` sends `SIGCONT` without the terminal.
- `jobs` lists them; a job that finished in the background is reported
  before the next prompt, as a shell does.
- The `wait` builtin, which today loops on `waitpid(-1)`, waits for the
  job table to empty instead.

## Affected files

| File | Change |
|---|---|
| `kernel/include/kernel/process.h` | `stopped`, `stop_sig`, `stopped_wq`; the stop and continue calls; `process_group_is_orphaned` |
| `kernel/process/process.c` | the stopped state, `SIGCHLD` to the parent, the stop path through `process_wait_child`, the orphan test |
| `kernel/process/signal.c` | stop and continue as default actions rather than ignores; parking at delivery; `SIGKILL` un-stops |
| `kernel/include/kernel/signal.h` | the default-action table's new outcomes |
| `kernel/tty/tty.c`, `kernel/include/kernel/tty.h` | `^Z`; `SIGTTIN` in `tty_read`; `SIGTTOU` from `tcsetpgrp` |
| `kernel/syscall/native.c` | `wait` flags; the new status values |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_WUNTRACED`, `COSMO_WCONTINUED`, the stopped/continued status values, `procinfo.state` 3 |
| `compat/linux/convert.c` | `lx_wait_status` gains stopped and continued |
| `compat/linux/syscalls.c` | `wait4`'s `WUNTRACED`/`WCONTINUED` |
| `libc/include/sys/wait.h`, `libc/include/signal.h` | `WIFSTOPPED`, `WSTOPSIG`, `WIFCONTINUED`; the bounded `WIFEXITED`/`WIFSIGNALED` |
| `userland/shell/sh.c` | `&`, the job table, `jobs`/`fg`/`bg`, the stopped-job report |
| `userland/system/ps.c` | `T` for a stopped process |
| `userland/init/init.c`, `kernel/process/proctest.c` | the probes and self-tests below |

## New APIs

- **User-visible**: `waitpid` flags `WUNTRACED` and `WCONTINUED`; the
  macros `WIFSTOPPED`, `WSTOPSIG`, `WIFCONTINUED`; the shell's `jobs`,
  `fg`, `bg` and `&`. No new system call numbers — `wait` gains flags
  and `kill` already sends every signal this needs.
- **Kernel-internal**: `process_stop(p, sig)` and `process_continue(p)`;
  `process_group_is_orphaned(pgid, sid)`; the delivery-side park in
  `signal_deliver`; `tty_read`'s foreground check.

## Migration plan

1. **The stopped state**, proved by a program that stops itself with
   `raise(SIGSTOP)` and a parent that sees `WIFSTOPPED`, continues it,
   and sees it finish — with no terminal in sight.
2. **`SIGKILL` and the edge cases** — killing a stopped process,
   stopping a process blocked in `read`, a stop and a continue racing.
3. **`^Z` and `SIGTTIN`** at the terminal, including the orphaned-group
   rule, which is the one that can wedge the machine if it is wrong.
4. **The wait-status change**, which touches every caller and is
   therefore done in one commit with the tests that read statuses.
5. **The shell**: `&`, the job table, `jobs`/`fg`/`bg`, and the boot
   test typing `^Z`.

## Tests

- **`signal-stop`** — a process raises `SIGSTOP` on itself; the parent
  sees `WIFSTOPPED` with the right signal, sees it *again* only after a
  second stop (the edge-triggered rule), sends `SIGCONT`, and collects a
  normal exit. Reintroducing "stop is ignored" fails it.
- **`signal-stop-kill`** — a stopped process is killed and dies; a
  stopped process blocked in `read` stops there and resumes; a `SIGSTOP`
  and a `SIGCONT` in one batch leave it running, not parked.
- **`signal-stop-mask`** — `SIGSTOP` cannot be blocked, caught or
  ignored, which the core already enforces and this makes explicit.
- **`tty-stop`** — the two-ended shape the signals unit used: a process
  claims the terminal, the kernel types `^Z`, the process must stop
  (not die, not continue); then `SIGCONT` and it finishes.
- **`tty-ttin`** — a process in a background group reads the terminal
  and stops; the foreground group's read is unaffected; an *orphaned*
  background group gets `-EIO` instead of stopping, which is the check
  that proves the machine cannot wedge.
- **`wait-status`** — a host test over the encoding: every existing
  status still classifies as it did, and the two new ones do not
  collide with them.
- **The interactive boot test** — `sleep 30`, `^Z`, a prompt, `jobs`
  showing it stopped, `fg`, `^C`. Bounded in time the way the `^C` test
  now is, so that a run in which `^Z` did nothing fails rather than
  passing slowly.

## Benchmarks

Two, and the report should say plainly that neither is likely to change
the design:

- **`^Z` to prompt**, the same shape as the `^C` latency the signals
  unit measured: keypress to the shell's next prompt. It is the number a
  person feels, and it is worth having beside the `^C` one.
- **`tty_read` with and without the foreground check.** The check is one
  comparison against a field already in the struct, on a path that runs
  once per line, so the expected answer is "unmeasurable". The reason to
  measure is that the alternative placement — in the console object
  rather than the line discipline — would only be worth arguing about if
  the number said so.

The measurement this unit would most like is the one it cannot easily
make: how often a stop lands on a thread blocked deep in a wait versus
one already at a syscall return. That is a distribution, not a
benchmark, and the tests cover both cases explicitly instead.

## Risks

- **A wedged machine.** A stopped orphaned process group has nothing
  left to continue it. This is the one failure mode that is worse than
  the feature is good, which is why the orphan rule is in the same step
  as `SIGTTIN` and has its own test.
- **A stopped `init`.** Must be impossible; `init` is a session leader
  in no terminal's foreground group, but the rule should be explicit
  rather than incidental.
- **The self-test harness.** A probe that stops and is never continued
  hangs the boot test rather than failing it. Every stop in a test needs
  a bounded continue, the way the signal probes' spin loops are bounded.
- **The wait-status change touches every caller.** Nine or so places in
  the tree read a status. The extension is chosen partly because it lets
  them all keep working, but the ones that switch on it must still be
  swept — the tree has lost findings to exactly this before.
- **Reporting a stop twice, or not at all.** Edge-triggered reporting is
  where this kind of code usually goes wrong; a parent polling with
  `WNOHANG | WUNTRACED` must not spin on the same stop.
- **A `SIGCONT` that races a stop.** Both directions must be tried: the
  signal core's per-process lock covers the state, but the ordering
  rules (a stop discards a pending continue and the reverse) are the
  kind of thing that is right in one direction and wrong in the other.

## Documentation the signals unit left stale

Found while writing this report, and corrected in the same commit
because a report that quotes the tree should not quote it wrongly:

- `docs/libc/invariants.md` still said the libc had no `signal` or
  `sigaction` (the signals unit added both) and no floating point (the
  FP/SIMD unit gave `printf` `%f`/`%e`/`%g`).
- `docs/kernel/process/architecture.md` still said the native ABI had
  no handlers.
- `docs/userland/design.md` and `userland/system/svc.c` said the
  supervisor *cannot* catch `SIGTERM`. It can now; it does not, which is
  a different sentence and a change to `svc` rather than to the kernel.
- `compat/linux/syscalls.c` said "process groups do not exist" where
  `wait4` refuses a group. They exist; `wait4` has not been taught them,
  which is now a recorded gap in `docs/compat/linux/invariants.md`.

Four of the five are the same failure the tree has lost findings to
before: a design rule stated in one place and not swept everywhere it is
quoted. The signals unit swept the files it changed and not the files
that described what it changed.

## Alternatives considered

- **GICv3** — a third interrupt-controller backend behind a seam two
  backends already prove, for hardware this project cannot test on yet.
  The right unit the day a real ARM machine arrives; it has been the
  runner-up for three reports and the reason has not changed.
- **ASID allocation** on AArch64 instead of the full TLB invalidate per
  context switch — a measurable optimisation of something that works,
  with a context-switch benchmark already in the tree to measure it. A
  good small unit that makes nothing new possible.
- **`fork` and `exec`** — the larger absence in `struct process`'s gap
  list, and the one a ported program is most likely to want. It is also
  a much larger unit, and job control is a prerequisite for the shell
  that would use it well.
- **Device nodes and `/dev/tty`** — the gap that stops a process from
  reopening the console after closing handle 0. Real, small, and
  independent; a good unit after this one, and arguably a better one
  before it if the terminal rules turn out to want a `/dev/tty` to name.
- **Nothing: leave finding #30 open.** Defensible while one command at a
  time is enough, which is exactly as true as it was before the machine
  had a keyboard.
