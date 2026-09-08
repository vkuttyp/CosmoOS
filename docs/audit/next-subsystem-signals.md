# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the sixth such report
(the NIC, the USB host stack, AHCI, the machine's own console, and
floating point; all five record their outcomes). Nothing in this one is
implemented.

**Subsystem: the signals a person can send — `^C`, and everything the
kernel needs before a keypress can interrupt a program.**

## Problem

The console unit gave this machine a screen and a keyboard. A person can
sit at it, type a command, and watch it run. If the command does not
stop, neither does the machine: **`^C` is dropped in the line
discipline** (`kernel/tty/tty.c:108`, "other control bytes are dropped
(no signals yet, so `^C` too)"), and the only way out of a program that
loops is the reset button.

That is not an oversight in the tty. It is the visible end of a
capability the tree never needed until it had a keyboard:

- **The native personality installs no handlers.** `signal.h` says so in
  its first paragraph, and `kernel/process/signal.c:312` acts on it: a
  process whose personality has no frame builder and whose action is not
  the default is *terminated*. `p->pers->signal_frame` is set in
  `compat/linux/syscalls.c` and nowhere else.
- **The native ABI has the numbers and not the mechanism.**
  `uapi/cosmo/syscall.h` offers `kill(pid, sig)` and five constants
  under a comment that says what it means: "Signals are numbers only in
  this phase: every one terminates the target with status 128 + sig;
  there are no handlers."
- **There are no process groups.** No `pgid` anywhere in the kernel, so
  there is nothing for a `^C` to be sent *to*: a terminal interrupt goes
  to the foreground process group, and this system has neither.
- **Job control is a recorded deviation.** `SIGSTOP`, `SIGTSTP`,
  `SIGTTIN` and `SIGTTOU` are *ignored* rather than honoured, because
  nothing can stop (audit finding #30, written down at the time as a
  deliberate deviation from Linux).
- **The libc's `signal.h` is five constants and `kill`** — no
  `sigaction`, no `signal`, no mask, because there is nothing behind
  them. `userland/system/svc.c` even says so where it explains why it
  does not bother: "a native process here has no signal handlers". And
  the shell has no notion of interrupting anything: it spawns, it waits,
  and a child that never exits keeps it waiting.

What *is* here is most of the hard part. The signal core is complete and
in use: per-process actions under `p->lock`, per-thread pending and
blocked sets plus a process-wide pending set, delivery at a return to
user mode, the default-action table, faults turned into signals, masks
saved and restored around a handler. The Linux personality drives all of
it, with frames on both architectures — and since the FP/SIMD unit,
those frames carry the vector registers too. The missing piece is a
frame builder for the tree's own ABI, and the plumbing that decides who
a keypress is talking to.

## Current implementation

- **The core** (`kernel/process/signal.c`, `kernel/include/kernel/signal.h`):
  `signal_send`, `signal_send_thread`, `signal_fault`, `signal_deliver`
  (at return to user), `signal_return`, `signal_set_action`,
  `signal_blocked`/`set_blocked`, `signal_pending`. Numbers are Linux's
  1..64. `SIGKILL` and `SIGSTOP` cannot be blocked.
- **The Linux personality** (`compat/linux/signal.c`): `rt_sigaction`,
  `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`, `sigaltstack`, the
  x86-64 and AArch64 frames (the latter now carrying an
  `fpsimd_context`), the trampoline page, `SA_RESTART` and restart
  handling. `lxtest` and `lxsig` cover it.
- **The native personality** (`kernel/syscall/native.c`): `SYS_kill`
  (34) and nothing else about signals. No `sigaction`, no mask, no
  frame builder, so every signal is its default and a handler is fatal.
- **The tty** (`kernel/tty/tty.c`, 168 lines): a line discipline with
  echo, `^H`/`^U`/`^D`, a ring of completed lines, one console tty. It
  drops every other control byte and says why.
- **Processes** (`kernel/process/process.c`): pid, ppid, credentials,
  domains, handles, a process table — and no groups or sessions.
- **The shell** (`userland/shell/sh.c`): spawn, pipelines, redirection,
  `wait`; no signal handling, no job notion.

## Why it matters

1. **It is the first thing a person tries.** The console unit's whole
   argument was a machine somebody can use; the second thing they do
   after typing a command is interrupt one. Every other capability in
   this tree is reachable only through a shell that cannot be
   interrupted.
2. **The native ABI is a personality with a hole in it.** Everything
   else the tree offers natively — processes, files, pipes, sockets,
   mounts, packages — is usable from a native program. Signals are the
   one service where a native program must pretend to be Linux to get
   the real thing, which is backwards.
3. **A recorded deviation gets a chance to be retired.** Audit finding
   #30 is a deviation the tree took deliberately because it could not do
   better; job control is what makes `SIGSTOP` mean something. Whether
   this unit takes that step is a question below, not an assumption.
4. **The architectural question: how much of the session model?** In
   POSIX, a terminal has a foreground process group, a process group
   belongs to a session, a session has a controlling terminal, and the
   rules about who may change what are written in terms of all three.
   This system has one tty and one shell. The minimum that makes `^C`
   correct is a foreground group on the tty and a way for the shell to
   set it; the question is what stops any process from stealing it, and
   whether the honest answer to that is "sessions" — in which case the
   unit builds them and says so, rather than inventing a smaller rule
   that turns out to be sessions wearing a different name.
5. **It is where the previous unit pointed.** The FP/SIMD report
   deferred "a native signal ABI — its own unit, whenever someone wants
   handlers outside the personality". Somebody does: the person at the
   keyboard.

## Proposed design

Five steps; the last is droppable.

### 1. A native signal ABI

New native calls, numbered in the free range of
`uapi/cosmo/syscall.h`: `sigaction(sig, act, old)`,
`sigprocmask(how, set, old)`, `sigreturn()`, and `sigpending(set)`. The
structures are the tree's own and deliberately smaller than Linux's: a
handler, a mask, flags (`SA_RESTART`, `SA_RESETHAND`, `SA_NODEFER`, and
`SA_SIGINFO` only if a use appears), and a `siginfo` carrying the
signal, the sender's pid and, for faults, the address — which is what
`struct signal_info` already holds.

The frame builder is the personality's, in a new
`kernel/process/native_signal.c` beside the Linux one, and follows its
shape: build on a copy of the registers, write the frame to the user
stack, block the handler's mask plus the signal, and return through a
restorer. The libc supplies the restorer, so no trampoline page is
needed for the native ABI.

**No alternate signal stack**, and therefore no `sigaltstack` and no
`SA_ONSTACK` (§21: an earlier draft of this report promised the
alternate stack in one paragraph and left it out of the API list in the
next). What an alternate stack buys is a `SIGSEGV` handler that still
runs when the thread's own stack is what overflowed; nothing in this
tree wants one, and the Linux personality already has `sigaltstack` for
a program that does. It is named under what this unit does not cover, so
that adding it later is a decision rather than a discovery.

There is no compatibility burden here at all: the layout is whatever
this project chooses, so it is chosen to be the smallest thing that
`sigreturn` can restore exactly — the general registers, the mask, and
the FP/SIMD image the accessors already produce
(`arch_user_fpu_image_size/save/restore`, which the FP/SIMD unit put
behind one interface for exactly this).

### 2. Process groups

`pgid` on `struct process`, inherited across `spawn`; `setpgid(pid,
pgid)` and `getpgid(pid)`; `kill(-pgid, sig)` to a whole group, which is
what a terminal interrupt uses. The rules are POSIX's where they cost
nothing: a process may set its own group or that of a child it has not
yet waited for, and may not move a process into a group belonging to
another session — the last clause being where step 3's answer decides
the shape.

### 3. A foreground group on the tty, and `^C`

The tty gains a foreground process group and the two calls that read and
write it (`tcgetpgrp`/`tcsetpgrp`, or the native equivalents). The line
discipline stops dropping `^C` and `^\`: it sends `SIGINT` and `SIGQUIT`
to that group from interrupt context — through `signal_send` on each
member, which already wakes blocked threads — and echoes `^C` as Linux
does.

**The question this unit exists to answer** is what guards that group.
The options, in the order this report ranks them:

- **Sessions, properly**: `setsid`, a controlling terminal per session,
  `tcsetpgrp` refused from another session, `SIGHUP` on hangup. It is
  the model every Unix converged on, and the reason is that the smaller
  rules do not compose.
- **A controlling-process rule**: the tty remembers the process that
  first claimed it (the shell) and lets only it, or its descendants,
  change the foreground group. Smaller, and probably sessions with the
  serial numbers filed off.
- **No guard at all**: any process may set the foreground group. Honest
  for a single-user machine with one shell, and the thing that has to be
  undone the day a second shell exists.

The plan builds the first, because the tree's habit is to build the
model rather than the shortcut — but the report is the place to argue
it, and a measurement cannot settle this one. What can settle it is
counting what the model costs: if sessions are eighty lines and a field,
the argument for the shortcut is thin.

### 4. The libc and the shell

`signal.h` grows `signal()`, `sigaction()`, `sigprocmask()`, `raise()`
and the flag constants beside the `kill()` and the five numbers it
already has, and `signal.c` supplies the restorer. The shell then does what a shell does:
ignore `SIGINT` in itself, put each job in its own process group, hand
the terminal to it, take the terminal back when the job ends, and print
a fresh prompt when a job died of a signal rather than pretending it
exited.

### 5. Job control (droppable)

`^Z`, a stopped state for a process, `SIGSTOP`/`SIGCONT` that stop and
continue rather than being ignored, `fg`/`bg` in the shell, and
`waitpid`'s `WUNTRACED`. This is the step that retires audit finding
#30, and the step to drop if the rest runs long: a stopped process is a
scheduler state, a `wait` result and a shell feature, and none of it is
needed for `^C` to work.

## Affected files

| File | Change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `sigaction`/`sigprocmask`/`sigreturn`/`sigpending`, `setpgid`/`getpgid`, `tcgetpgrp`/`tcsetpgrp`, the structures and the flag constants |
| `kernel/syscall/native.c` | the new calls |
| `kernel/process/native_signal.c` (new) | the native frame builder and `sigreturn` |
| `kernel/process/process.c`, `kernel/include/kernel/process.h` | `pgid` (and `sid` if step 3 builds sessions), inheritance, the group lookup |
| `kernel/process/signal.c` | sending to a group; the stopped state if step 5 happens |
| `kernel/tty/tty.c`, `kernel/include/kernel/tty.h` | the foreground group, `^C`/`^\`, the echo |
| `libc/include/signal.h` (exists: five constants and `kill`), `libc/src/signal.c` (new) | `sigaction`, `signal`, `sigprocmask`, `raise`, the flag and `sa_mask` types, and the restorer |
| `userland/shell/sh.c` | groups, the terminal, and a prompt after an interrupted job |
| `kernel/device/…test.c`, `tests/boot/shelltest.py` | the tests below |
| `docs/kernel/process/*`, `docs/kernel/tty/*`, `docs/libc/*`, `docs/userland/*`, `docs/compat/linux/*` | the model, and every place that says signals have no handlers |

## New APIs

- **Native**: `sigaction`, `sigprocmask`, `sigpending`, `sigreturn`,
  `setpgid`, `getpgid`, `tcgetpgrp`, `tcsetpgrp`, and (step 3, if
  sessions) `setsid`/`getsid`. `kill` gains the negative-pid form.
- **Kernel-internal**: a group lookup (`process_group_for_each`), the
  tty's foreground group, and `signal_send_group`.
- **libc**: `signal.h` as above — an extension of the existing header,
  not a new one. No `sigaltstack`: see step 1.
- **Unchanged**: the signal core's interface. Everything the native
  personality needs is already there, which is the argument that this
  unit is plumbing rather than invention — and the check on that claim
  is whether `kernel/process/signal.c` changes at all beyond group
  sending.

## Migration plan

1. **The native ABI**, proved by a test program that installs a handler,
   raises the signal, and returns from it — with no tty in sight.
2. **Process groups**, proved by `kill(-pgid)` reaching every member and
   nothing else.
3. **The tty's foreground group and `^C`**, proved by the boot test
   typing `^C` at a program that would otherwise never end.
4. **The libc and the shell**, which is where a person's `^C` becomes a
   new prompt.
5. **Job control**, or the note saying it was dropped and why.

Nothing is removed at any step: a native program that installs no
handler keeps today's behaviour exactly, and the Linux personality is
not touched except where a shared rule changes.

## Tests

**`signal-native`** (new self-test through a user process): a handler
installed, `raise` called, the handler observed, `sigreturn` restoring
the general registers, the mask, and — because the FP/SIMD unit makes
this possible and therefore obligatory — the vector registers, with the
handler deliberately clobbering them.

**`signal-mask`**: a blocked signal stays pending and is delivered by
the unblock; `SIGKILL` refuses to be blocked; the mask a handler runs
under is the action's plus the signal, and it is back afterwards.

**`signal-group`**: three processes in one group and one outside it;
`kill(-pgid, SIGTERM)` ends exactly three.

**`tty-intr`**: the line discipline's own test — `^C` with a foreground
group set sends `SIGINT` to it and to nothing else; with no foreground
group, it is dropped as before; the echo is right.

**The boot test types `^C`** (`tests/boot/shelltest.py`, and over the
keyboard through QMP for the console shape): a program that never exits,
then `^C`, then a prompt. This is the test the whole unit exists for,
and it is one line in a script.

**`signal-latency`** (reports): from the keypress to the handler, and
from `kill` to the handler, for a program spinning in user mode and one
blocked in a read.

**Shapes**: both architectures, `QEMU_SMP=1` and 4 (a signal sent from
another CPU must be delivered by the target's own return to user), the
usual chain.

## Benchmarks

1. **How long does `^C` take?** From `tty_input` seeing the byte to the
   handler's first instruction, for a program spinning in user mode
   (delivery waits for the next return to user, so this measures the
   tick) and for one blocked in `read` (delivery waits for the wake).
   The number decides nothing on its own, but a `^C` that takes more
   than a tick to be noticed is a bug this measurement would name.
2. **What does a handler cost?** Frame build, delivery, `sigreturn` —
   with and without the FP/SIMD image, which is 528 bytes of the frame
   on AArch64. If the image dominates, the question of whether a frame
   should carry it unconditionally is worth asking, and the answer is
   probably still yes.

## Risks

- **Signal delivery is where reentrancy lives.** A handler runs on the
  user stack of a thread that was somewhere in the kernel; the frame
  must be written with faults possible, the mask restored exactly, and
  `sigreturn` must not trust anything in the frame it did not put there.
  The Linux side has all of this and is the model.
- **The foreground group is a policy question wearing a mechanism's
  clothes.** Choosing wrong means either a security hole (any process
  steals the terminal) or machinery nobody uses (sessions for one
  shell). This is what the report is for.
- **`^C` from interrupt context.** The line discipline runs in a UART or
  USB interrupt; sending a signal from there must not sleep, allocate,
  or take a sleeping lock — `signal_send` already claims to be callable
  from any context, and this unit is where that claim gets used.
- **Step 5 is a scheduler change.** A stopped process is a state the run
  queue, `wait` and the shell all have opinions about. It is last, and
  droppable, for that reason.
- **Not covered, and named now rather than discovered later**: an
  alternate signal stack (`sigaltstack`, `SA_ONSTACK`), real-time
  signals and queued `siginfo`, `sigsuspend`/`sigwait`, and — unless
  step 5 survives — anything that stops a process.
- **Size**: about 300 lines for the native ABI and frame, 150 for groups
  and sessions, 100 for the tty, 150 for the libc, 100 for the shell,
  and 350 of tests — roughly 1 150, plus whatever step 5 costs if it
  survives.

## Alternatives considered

- **GICv3** — a third interrupt-controller backend behind a seam two
  backends already prove, for hardware this project cannot test on yet.
  The right unit the day a real ARM machine arrives.
- **ASID allocation** on AArch64 instead of the full TLB invalidate per
  switch — a measurable optimisation of something that works, and the
  FP/SIMD unit left a context-switch benchmark that would measure it.
  A good small unit; it makes nothing possible that is impossible today.
- **A HID report parser and a mouse** — still circular: a pointer needs
  something to point at.
- **GPU, Wi-Fi, Bluetooth** — §60 says later.
- **Nothing: leave `^C` dropped.** Defensible while the only user is a
  test harness that never needs to interrupt anything, which stopped
  being true when the machine grew a keyboard.

## Outcome (2026-09-08)

Built: steps 1 to 4. Step 5, job control, was not built -- the report
called it droppable and last, and that is what happened to it.

**What the unit answered.** The architectural question was how much of
the POSIX session model to build, and the answer the code gives is
"all of the small part of it": `sid` and `pgid` on the process,
`setsid`/`setpgid`/`getsid`/`getpgid`, a controlling session and a
foreground group on the tty, and a release when the session leader
exits. The count the report asked for -- *if sessions are eighty lines
and a field, the argument for the shortcut is thin* -- came out at two
fields and about 200 lines of kernel, of which the session rules
themselves are perhaps 90. The shortcut ("remember one controlling
process") would have saved almost none of it, because it still has to
answer who may take the terminal, who inherits it, and when it is
released. Sessions were the cheaper answer, not the more thorough one.

**What the frame ended up carrying.** The general registers, the
blocked mask, the FP/SIMD image and the siginfo, behind a magic number,
opaque to the program, found from the stack pointer at `sigreturn`. The
one thing the report did not anticipate is the failure mode that cost
the most time: the frame originally hard-coded the FP image size per
architecture, AArch64's is 520 bytes rather than the 528 assumed, and
the mismatch made the frame carry *nothing* -- silently, with the only
symptom a handler's vector registers surviving into the interrupted
code. The frame now records the length it carries and reports the one
case a length cannot express.

**A second thing the tests found**, and it is a test-writing lesson
rather than a kernel one: the first version of the asynchronous probe
loaded its vector pattern before spawning the child that would signal
it, and the library's string handling -- compiled with the vector
registers available -- overwrote the pattern on the way through
`spawn`. A test of the kernel's saving and restoring has to own the
registers for the whole window it measures.

**Three delivery points, three tests.** The report named the frame
builder as the risk; what the tests actually cover is that the frame is
built correctly at each of the three returns to user mode -- a system
call, an interrupt, a fault -- because each starts from a different arch
frame. The asynchronous one holds sentinels in four registers no calling
convention preserves, and reports how many times its loop went round, so
that a signal arriving before the loop started fails the probe rather
than passing it having proved nothing.

**Size**: about 2 100 lines changed against the report's estimate of
1 150 for five steps; four steps came to roughly 900 lines of code and
650 of tests, and the rest is documentation.

**What is left for a later unit**, all of it named in the gaps of
`docs/kernel/process/invariants.md`: job control (a stopped state,
`^Z`, `SIGCONT`, `fg`/`bg`, `SIGTTIN`/`SIGTTOU`), an alternate signal
stack, real-time signals and queued siginfo, and `sigsuspend`/`sigwait`.
