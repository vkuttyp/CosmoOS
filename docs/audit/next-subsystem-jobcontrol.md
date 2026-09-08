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
calling thread on `stopped_wq`. Nothing is frozen mid-kernel, which is
the property that makes this safe, and it is inherited free from where
signals are delivered.

**Getting the *other* threads to park is not free, and the core does not
do it today.** Three facts decide the mechanism, and all three are worth
stating before anything is written:

- A signal sent to the process, rather than to a thread, sets
  `p->sig_shared_pending` and wakes **one** eligible thread —
  `route_locked` breaks out of its walk at the first one. That is right
  for an ordinary signal, where exactly one thread should take it, and
  wrong for a stop, where every thread must park.
- **Waking a sibling is not enough.** `wait_event_killable` re-evaluates
  its own condition and `signal_pending()`, and `signal_pending()` reads
  `kill_sig`, the process state, and the pending sets — which the thread
  that dequeued the stop has already cleared. A woken sibling therefore
  finds nothing to report and blocks again. Waking without also giving
  it something to see is a no-op with extra steps.
- `wait_event` — the non-killable wait — never consults
  `signal_pending()` at all. A thread inside one cannot be made to come
  out; it parks when whatever it waits for completes.

So the stop is a **per-thread flag, set on every thread at once**, in
the same shape `kill_sig` already uses: a process-wide decision made
visible to each thread's own wait predicate. `signal_pending()` reports
it, so `wait_event_killable` returns `-EINTR` for a stop exactly as it
does for a kill, and the return-to-user path parks the thread and clears
its flag. The `stopped` flag on the process is then a summary — true
while any thread still carries its bit or is parked — rather than the
thing the waits consult.

**And the interrupted call must be restarted, not failed.** A syscall
cut short by a stop has to resume when the process continues; a `read`
that returns `-EINTR` because someone pressed `^Z` and then `fg` would
be a bug in every program that does not expect it. The core already has
the machinery — `thread.syscall_nr`, `syscall_arg0` and
`arch_user_regs_restart_syscall` — but it gates restart on `SA_RESTART`,
and a stop has no action to carry a flag. Parking for a stop must
therefore mark the call for restart unconditionally, which is what Linux
does and what this design has to say explicitly because the existing
gate would otherwise say no.

The remaining consequence is worth being honest about rather than
discovering in a test: **a stop is still not instantaneous across
threads.** A thread inside a non-killable wait parks only when that wait
ends. Every such wait in the tree is bounded by an I/O completion or a
timer, so this is a latency rather than a hang — but a `waitpid` that
reports the stop must report it when the process is *fully* parked, or a
shell will take the terminal back while a thread of the job is still
running. All of the tree's own user processes are single-threaded today,
so only the Linux personality's `clone` reaches any of this; that is
exactly why it needs designing rather than finding.

**`SIGCONT`** clears `stopped`, clears the per-thread stop flag on
**every** thread, wakes `stopped_wq`, and discards any pending stop
signal; posting a stop signal discards a pending `SIGCONT`. Both leave
the ignore table: `SIGCONT`'s default becomes *continue*, and the four
stop signals' default becomes *stop*. `SIGKILL` un-stops and kills — a
process that could not be killed while stopped would be worse than one
that cannot stop.

**Clearing every thread's flag is not on its own enough**, and the case
that shows why is the one the non-killable wait creates. A thread inside
`wait_event` never sees the stop; if `SIGCONT` arrives before that wait
ends, the continue sweeps a flag on a thread that is not looking, the
wait then finishes, and the thread arrives at the return-to-user path
carrying a flag from a stop that is over. Parking it there would stop a
process that has already been continued, with nothing left to continue
it again — a hang, from the same gap between per-thread and
process-wide state that the routing above had in the other direction.

So **the park is decided by re-reading the process's stop state under
`p->lock` at the parking point**, and the per-thread flag is only a
reason to *look* — it makes `signal_pending()` true so a killable wait
returns and the return path runs, and it is never itself the authority
on whether to park. A stale flag then costs one lock acquisition and a
re-check, which is the correct amount for a race that cannot otherwise
be closed: the thread that must observe the continue is by definition
the one that was not watching when it happened.

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
  a per-thread stop flag that `signal_pending()` reports, beside
  `kill_sig`; `process_group_is_orphaned(pgid, sid)`; the delivery-side
  park in `signal_deliver`; `process_fully_stopped(p)`, which is what
  `waitpid` reports on; `tty_read`'s foreground check.

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
- **`signal-stop-threads`** — a Linux-personality program with two
  threads, one of them blocked in a `read` that will never complete: the
  stop must park both, and the parent's `waitpid` must not report the
  stop until it has. This is the test for the routing above, and the
  only one that needs more than one thread, since the native ABI has no
  way to make one. It is also the test that fails if waking a sibling is
  mistaken for stopping it.
- **`signal-stop-restart`** — a process blocked in a `read` on a pipe is
  stopped and continued, and the `read` then returns the byte that
  arrives afterwards rather than `-EINTR`. Without the unconditional
  restart this fails, and it fails in the way programs actually notice.
- **`signal-stop-late`** — the continue that outruns the thread: a stop
  and then a `SIGCONT` while one thread is still inside a wait it cannot
  be interrupted from. When that wait ends the thread must return to
  user mode and keep running, not park. A design that trusts the
  per-thread flag rather than re-reading the process's state hangs here,
  and hangs in the way that needs a reset button, so the test is
  bounded and fails rather than waiting.
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
- **Reporting a stop before every thread has parked.** The shell would
  take the terminal back while a thread of the job was still writing to
  it. The report above makes `waitpid` wait for the last thread; the
  risk is that "the last thread" is easy to get wrong when threads are
  exiting at the same time.
- **A continue that outruns the thread it is continuing.** The stop is
  per-thread and the continue is process-wide, so any design in which
  the thread's own flag decides whether to park has a window where a
  flag outlives the stop that set it. Parking on a stale flag is a hang
  rather than a wrong answer, which is why the park re-reads the
  process's state under the lock instead.
- **A syscall that is failed rather than restarted.** The restart gate
  exists and says no to anything without `SA_RESTART`; a stop has no
  action. Getting this wrong turns `^Z`/`fg` into spurious `-EINTR` in
  every program that was blocked, and it is the kind of thing that looks
  fine in a test that only stops an idle process.
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

`docs/kernel/process/architecture.md` had it twice more, in two
"non-responsibilities" lists that read as current status. They are
records of what a *phase* deferred, so rather than keep three lists in
sync, both now say which of their entries has since been built and point
at the gaps section of `invariants.md` as the one that is maintained.
Writing that correction produced two false claims of its own on the
first attempt -- set-uid binaries and file-backed `mmap` do not exist,
and `sys_mmap` still says so -- which is its own argument for one
maintained list rather than several remembered ones.

The rule this unit should carry forward is narrower than "sweep
everywhere": **after changing what the system can do, grep for the
sentences that said it could not.** The signals unit swept the files it
edited. Four rounds of review found nine places that merely *described*
what it edited, in five documents it never opened.

## A kernel fix this report's CI forced

CI failed the first push of this report on a check it does not touch:
`kill(pid, 0)` on a child whose `waitpid` had just returned answered 0
instead of `-ESRCH`. The cause is older than this branch and older than
the signals unit -- `process_lookup` did not consult `reaped`, so a
process stayed findable by pid from the moment its status was collected
until its last reference dropped, which is a window the reaper usually
closes first and CI happened not to.

`process_lookup` now refuses a reaped process. It is one line and it
belongs to this PR only because a red CI cannot be merged; it is
separable if the tree would rather have it on its own.

The test for it is worth a note, because the obvious one does not work:
reaping in a loop from user mode and asking immediately never lost the
race in 200 consecutive tries here, so it would have shipped as a test
that proves nothing at the price of 1.4 seconds. The kernel can hold the
process object alive on purpose and ask while it is provably still in
the table, which is `process-reaped` and fails every time the rule is
removed. That is the same lesson the signals unit learned three times:
a test that cannot fail on demand is not yet a test.

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

## Outcome (2026-09-08)

Built, all five steps. Audit finding #30 is closed: `^Z` stops the
foreground job, `jobs`/`fg`/`bg` manage it, `&` backgrounds a pipeline,
and a background job that reads the terminal is stopped rather than
allowed to steal the shell's line.

**The wait-status question resolved as the report argued.** The
encoding was extended upward and nothing that already read a status
changed meaning; `lx_wait_status` gained two cases. No caller needed
touching, which was the whole argument.

**What the design got right in advance**, because four rounds of review
had already found it: the split between the per-thread flag (a reason to
look) and the process's own state (the authority), in both directions.
Implementing it was mechanical once stated.

**What it got wrong, and the tests found:**

- **`SIGCONT` must stay an *ignore*.** The report treated "continue" as
  a new default action and said the stop signals and `SIGCONT` both
  leave the ignore table. `SIGCONT` performs its continue in
  `route_locked` *before* the default-action table is consulted, so
  what is left for its default to do is nothing -- and taking it out of
  the ignore list made an uncaught `SIGCONT` **terminate the process it
  had just continued**. Two tests caught it as one root cause.
- **The parent's wait scan must not reach into a child's lock.** The
  first version asked `process_fully_stopped(child)` while holding the
  parent's lock -- two process locks nested, an order this kernel does
  not have. It surfaced as a global slowdown (`syscall-fuzz` 3 s to
  10.5 s) before it could surface as a deadlock. The last thread to park
  now sets `stop_reportable` itself, so the scan tests one bool.
- **The `SIGKILL` un-stop was redundant** and is deleted: the park's own
  wait ends on `kill_sig`. No test could tell it from its absence, which
  is the honest sign that it did nothing.

**Three tests did not test what they claimed**, all found by
reintroducing the bug, and all the same lesson as the signals unit:

- `signal-stop-kill` passed with the un-stop removed (above).
- `signal-stop-restart` passed twice with the restart deliberately
  broken. First it stopped the child between its handshake and its
  sleep; then it measured *elapsed time*, which includes however long
  the process sat parked, so a failed call and a restarted one are
  indistinguishable by duration. It now uses `SIGTTIN`, where **the
  call under test causes its own stop** -- the only way to aim one
  reliably.
- `tty-ttin` could let its child reach the read before the terminal had
  a foreground group, in which case the read was legitimately allowed.

**Not built, and recorded in the gaps**: the multi-threaded half of
stopping. That a parent is told of a stop only when the *last* thread
has parked is implemented and exercised only with one thread, where it
is trivially true; `signal-stop-threads` would need a Linux-personality
program and was not written. One guard in `process_stop_park` is
likewise not distinguishable by any test, and is recorded rather than
quietly kept.

**Size**: about 1 500 lines changed against the report's estimate --
roughly 400 of kernel, 350 of shell, 500 of tests, the rest
documentation.
