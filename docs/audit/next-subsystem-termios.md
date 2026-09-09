# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the eighth such report
(the NIC, the USB host stack, AHCI, the machine's own console, floating
point, the signals a person can send, and job control; all seven record
their outcomes). Nothing in this one is implemented.

**Subsystem: a terminal a program can drive — terminal modes, `/dev/tty`,
and the line editing that has been impossible without them.**

## Problem

Three units in a row have been about this terminal. The console unit
gave the machine a screen and a keyboard; the signals unit gave it `^C`
and a foreground process group; the job-control unit gave it `^Z`,
`SIGTTIN` and a shell that manages jobs. A person can sit at this
machine and drive it.

What no *program* can do is drive the terminal. There is no way to turn
echo off, no way to read a keystroke without waiting for a newline, no
way to ask how many rows the screen has, and no way to open the terminal
at all except by inheriting a handle to it. The consequences are
concrete:

- **The shell has no line editing.** Backspace and `^U` are all there is,
  because they are implemented *in the kernel's line discipline*. No
  arrow keys, no history, no word erase — not because the shell has not
  got round to it, but because a shell can only do those things by
  taking the terminal into raw mode and drawing the line itself.
- **No program can be interactive** in any richer sense. An editor, a
  pager, a menu, anything that reads a single keypress: all impossible.
  That is a hard ceiling on what the ports system can ever carry.
- **The Linux personality tells libcs the console is not a terminal.**
  `lx_ioctl` returns `-ENOTTY` for every request
  (`compat/linux/syscalls.c:672`), and the comment says what follows:
  "libcs then treat the console as a plain file". A ported program's
  stdout is fully buffered rather than line buffered, and its
  `isatty()` is a lie in the other direction from ours.
- **A process that closes handle 0 can never get the terminal back.**
  There is no `/dev/tty` and no `/dev/console`, so the console is
  reachable only as an inherited handle. `docs/kernel/tty/invariants.md`
  has recorded this since the tty was written.

## Current implementation

Less is missing than the list above suggests, which is what makes this
the next unit rather than a large one.

- **The line discipline already has modes** — `TTY_ECHO` and
  `TTY_ICRNL`, with `tty_set_flags` to change them. It is
  kernel-internal, used by exactly one caller (the keyboard self-test,
  which turns echo off while the harness types). The mechanism for "the
  terminal behaves differently now" exists; nothing user-facing reaches
  it.
- **Character device nodes already exist.** `ramfs_mkchr` registers one,
  and `/dev/vmm` is one (`kernel-services/virtualization/vmm.c:136`),
  with `read`/`write` through `struct vnode_ops`. `/dev/tty` and
  `/dev/console` are a use of a mechanism that is already built and
  tested, not a new one.
- **The terminal knows its own size.** `struct fbcon` carries `cols` and
  `rows` (`kernel/include/kernel/fbcon.h:36`); the serial console does
  not, which is a question the design has to answer rather than a gap.
- **`isatty` answers, but answers the wrong question.** It tests
  `fstat`'s type for `COSMO_DT_CHR` (`libc/src/unistd.c:109`), and
  `vfs_stat` reports a vnode's type directly (`vfs.c:167`) — so *every*
  character node passes, `/dev/vmm` included. A program holding
  `/dev/vmm` is told it has a terminal and then refused every terminal
  operation. Nothing has noticed because nothing asks, and this unit is
  where it starts mattering: it is the call a ported program uses to
  decide whether to draw a prompt at all.
- **The session and foreground-group machinery is complete**, which is
  what `/dev/tty` needs to mean anything: "the terminal of my session"
  is a well-defined thing now, and was not two units ago.

What is missing is the user-facing shape: a way to ask for and set the
terminal's modes, a node to open it by, and a shell that uses both.

## Why it matters

1. **It is the last hard ceiling on interactive software.** Everything
   else the ports system needs is a matter of writing more C. Raw mode
   is not: no amount of userland work produces it.
2. **Line editing is the most visible thing this machine lacks.** A
   person who mistypes a long command retypes it. That is the first
   thing anyone notices after `^C` and `^Z` start working.
3. **It completes the terminal rather than extending it.** Three units
   built the terminal's *signal* semantics — who a keystroke reaches.
   This is its *mode* semantics — what a keystroke means. The pair is
   what "terminal" refers to everywhere else.
4. **The Linux personality is lying to every libc it hosts**, and the
   lie costs correctness (buffering) rather than just fidelity.

## Proposed design

Four steps.

### 1. The architectural question: what shape terminal control takes

Three answers, and the report ranks them in this order:

- **Typed calls over the tree's own structure**: `tcgetattr(handle,
  struct cosmo_termios *)`, `tcsetattr(handle, const struct
  cosmo_termios *)` and `ttysize(handle, struct cosmo_ttysize *)`, with
  `compat/linux/convert.c` translating to and from Linux's `termios` and
  its `TCGETS`/`TCSETS`/`TIOCGWINSZ`. This is exactly what the tree
  already does for `sigaction`, `spawn` and the wait status: its own
  shape at the native ABI, a translation layer for Linux binaries.

  A single `SYS_ttyctl(handle, op, arg)` was considered and is rejected
  **for the same reason as `ioctl` below**: its argument type would
  depend on `op`, which is the property being avoided. One multiplexer
  is not better than the thing it imitates because it has fewer
  operations.
- **POSIX `termios` verbatim** as the native structure, with
  `tcgetattr`/`tcsetattr` as system calls. Smaller translation layer,
  and the struct is one of the most copied in Unix, so a ported program
  would find what it expects. Against it: `c_cflag`, the baud rates and
  most of `c_iflag` describe a serial line this tree does not model, and
  adopting them wholesale means carrying fields nothing will ever read.
- **A general `ioctl`.** The tree has deliberately not had one; every
  other subsystem got a typed call instead. Introducing it for the
  terminal would be the first place a system call's argument type
  depends on its argument value, which is the property that makes
  `ioctl` hard to review and hard to filter (`syscall_filter` works on
  numbers).

The first is proposed. The measurement that could settle it is in
"Benchmarks": count how much of `termios` this tree would actually
implement, because if the honest answer is "eight flags and two control
characters" then the second option is carrying forty fields for
familiarity alone.

### 2. Modes, and what the line discipline does with them

The tty gains a mode set — at minimum:

- `ECHO`, `ICRNL` (both exist internally today),
- `ICANON`: with it off, `tty_read` returns bytes as they arrive rather
  than lines, and the editing characters lose their meaning,
- `ISIG`: with it off, `^C`/`^Z`/`^\` are ordinary bytes rather than
  signals — which the job-control unit's rules must respect, since a
  program in raw mode wants the keystroke, not the signal,
- `VMIN`/`VTIME`: how many bytes a raw read waits for, and how long.
  `VMIN=1, VTIME=0` (block for one byte) is what an editor wants and is
  the only combination this unit needs to get exactly right; the others
  can be recorded as approximations.

**The interaction with the last two units is the risky part**, not the
modes themselves. `ISIG` off must not defeat `SIGTTIN` (a background
read is still refused, because that rule is about *who* may read, not
about what the bytes mean), and a process that leaves the terminal in
raw mode and dies must not leave the machine unusable — the shell
restores the modes it set, and a terminal whose session leader exits is
already released.

`isatty` moves with them: the question a program means is "is this a
terminal", not "is this a character device", so the libc asks the
terminal layer -- a successful `tcgetattr` is the natural test -- and
`/dev/vmm` stops claiming to be one.

### 3. `/dev/tty` and `/dev/console`

Two character nodes on the existing `ramfs_mkchr` mechanism.
`/dev/console` is the machine's console whoever opens it; `/dev/tty` is
*the caller's controlling terminal*, which is now a well-defined thing —
it resolves through the session, and a process with no controlling
terminal gets `-ENXIO`. That distinction is the whole reason `/dev/tty`
exists, and it is only expressible because the signals unit built
sessions.

### 4. The shell, and the size of the window

With raw mode the shell can draw its own line: left and right arrows,
word erase, and a history the up arrow walks. That is the payoff, and it
is also the only part of this unit a person will notice.

The terminal's size (`cols`, `rows`) is worth reporting at the same
time, because line editing needs it the moment a line is longer than the
screen. The framebuffer console knows its size; the serial console does
not, and the honest answer there is a default of 80x24 with a note
saying so, rather than inventing a probe.

## Affected files

| File | Change |
|---|---|
| `kernel/include/uapi/cosmo/syscall.h` | `struct cosmo_termios`, the mode flags, `tcgetattr`/`tcsetattr`/`ttysize` numbers |
| `kernel/include/kernel/tty.h`, `kernel/tty/tty.c` | the mode set; `ICANON` off in `tty_read`; `ISIG` in the control-character path; `VMIN`/`VTIME` |
| `kernel/syscall/native.c` | the new calls, resolving a tty from a handle as `tcsetpgrp` already does |
| `kernel/object/console_obj.c`, a new `kernel/tty/ttydev.c` | `/dev/tty` and `/dev/console` through `ramfs_mkchr` |
| `compat/linux/syscalls.c`, `compat/linux/convert.c` | `TCGETS`/`TCSETS`/`TIOCGWINSZ` in `lx_ioctl` instead of a blanket `-ENOTTY`, and the translation |
| `libc/include/termios.h`, `libc/src/termios.c` | `tcgetattr`, `tcsetattr`, `cfmakeraw` |
| `libc/src/unistd.c` | `isatty` asks the terminal layer instead of the file type |
| `userland/shell/sh.c` | raw-mode line editing and history |
| `kernel/core/fbcon.c` | the size the terminal reports |

## New APIs

- **User-visible**: `tcgetattr`, `tcsetattr` and `ttysize` as typed
  system calls (no multiplexer), `cfmakeraw` in the libc, `/dev/tty`,
  `/dev/console`, a truthful `isatty`; the shell's line editing.
- **Kernel-internal**: `tty_get_modes`/`tty_set_modes` replacing
  `tty_set_flags`; `tty_read` in non-canonical mode; a controlling-tty
  lookup for `/dev/tty`.

## Migration plan

1. **The mode set and `ICANON`**, proved by a program that turns
   canonical mode off and reads one keystroke without a newline.
2. **`ISIG`**, and its interaction with the job-control rules.
3. **`/dev/tty` and `/dev/console`**, including `-ENXIO` for a process
   with no controlling terminal.
4. **The Linux side**: `TCGETS`/`TCSETS`/`TIOCGWINSZ`, so a hosted libc
   stops treating the console as a file.
5. **The shell**: line editing and history, and the boot test typing an
   arrow key.

## Tests

- **`tty-raw`** — canonical mode off: a single byte is readable without
  a newline, the editing characters are ordinary bytes, and turning it
  back on restores line-at-a-time reads. Driven from both ends the way
  `tty-intr` and `tty-stop` are, since the kernel must type.
- **`tty-nosig`** — `ISIG` off: `^C` arrives as byte 3 rather than a
  signal, and `SIGTTIN` still refuses a background reader, because that
  rule is about who reads and not about what the bytes mean.
- **`tty-modes-restore`** — a process that dies in raw mode leaves a
  terminal the next one can use.
- **`isatty-chr`** — `isatty` is true for the console and **false for
  `/dev/vmm`**, which is a character device and not a terminal. It is
  true for both today; that is the smallest thing in this unit and the
  one most likely to be left as it was.
- **`dev-tty`** — `/dev/tty` opens the caller's controlling terminal,
  `-ENXIO` without one, and a process that closed handle 0 can get the
  terminal back through it.
- **`lxsig termios`** or similar — a Linux program's `TCGETS` succeeds
  and its libc line-buffers as a result.
- **The interactive boot test** — the harness types an arrow key and a
  history recall, and the line the shell runs is the edited one. This is
  the first boot-test entry that will need to send an escape sequence.

## Benchmarks

- **How much of `termios` this tree would really implement.** Not a
  benchmark but a count, and the thing that settles the design question
  in step 1: if the answer is a dozen flags, a native structure is
  honest and the POSIX one is forty fields of decoration; if most of
  `c_iflag` and `c_lflag` turn out to mean something here, the argument
  flips.
- **The cost of a raw-mode read.** Canonical reads deliver a line per
  syscall; raw reads deliver a byte. An editor at typing speed will not
  notice, but the number is worth having before the shell starts doing
  one syscall per keystroke, and it decides whether `VMIN`/`VTIME`
  batching is worth implementing properly or approximating.

## Risks

- **A terminal left unusable.** The failure mode of this unit is a
  machine that cannot be typed at, which is worse than the feature is
  good. The shell restoring modes is not enough on its own — a program
  that dies in raw mode has no shell to restore anything — so the
  session-release path must reset the modes as well as the foreground
  group.
- **Raw mode against job control.** `ISIG` off means `^C` no longer
  reaches the foreground group, which is correct and also means a
  runaway program in raw mode cannot be interrupted from the keyboard.
  That is how every Unix works and is worth stating rather than
  discovering.
- **`ioctl` creeping in through the Linux door.** `lx_ioctl` will grow
  three requests; the pressure to keep adding them is real, and the
  native ABI should not follow.
- **The boot test typing escape sequences.** Every terminal test so far
  has typed single bytes. An arrow key is three, and the harness's
  timing assumptions are worth re-checking rather than assumed.
- **Two consoles, one size.** The framebuffer knows its geometry and the
  serial line does not. Reporting a plausible default for the serial
  case is a lie a program cannot detect; the alternative is refusing to
  answer, which breaks every program that asks.

## Documentation the job-control unit left stale

The fourth report in a row to open with this section, which is itself
the finding. Nine sentences said the tree had no job control, in files
the unit did not touch: `docs/libc/api.md` and `design.md`,
`docs/userland/architecture.md` and `invariants.md`,
`docs/compat/linux/architecture.md`, `invariants.md` and `design.md`,
`docs/kernel/process/design.md`'s own signal-core paragraph, and the
header comment of `userland/shell/sh.c` -- the file that *implements*
background jobs still said it had none. All corrected here.

The memory note about this failure mode has been written twice and has
not become a habit, so the useful thing is to make it mechanical rather
than remembered: **before opening a unit's PR, grep the tree for the
sentences that stated the limit the unit removes** -- "no job control",
"no handlers", "no groups", "stop signals are ignored" -- and read every
hit. It takes a minute and has caught nine, nine and now nine again.

## Alternatives considered

- **`fork` and `exec`** — still the largest single absence, and the one
  a ported program is most likely to want. It stays the runner-up for
  the reason the job-control report gave: it is a much larger unit
  (copy-on-write needs the VM object layer), and `spawn` covers what
  this tree actually does today. It becomes urgent when something real
  is being ported; nothing is yet.
- **GICv3** — a third interrupt-controller backend for hardware this
  project cannot test on. The right unit the day a real ARM machine
  arrives; the runner-up for four reports and the reason has not
  changed.
- **ASID allocation** on AArch64 — a measurable optimisation of
  something that works, with a benchmark already in the tree. Good and
  small; it makes nothing new possible.
- **Shell control flow** (`if`, `while`, `for`, globbing, command
  substitution) — the shell's own gap list, and entirely userland. It
  needs no kernel work, which is exactly why it does not need a
  §68 report: it can be done whenever someone wants it.
- **Nothing: leave the terminal cooked.** Defensible while every program
  reads whole lines, which is true of every program in this tree today
  and false of every program anyone would want to port.

## Outcome (2026-09-08)

Built, all four steps. A program can drive this terminal: modes,
`/dev/tty`, and a shell with line editing and history.

**The design question resolved as proposed.** Typed `tcgetattr`,
`tcsetattr` and `ttysize` over a native structure, with
`lx_termios_from_native`/`to_native` for Linux binaries. The count the
report asked for came out at **four flags and two numbers** against
POSIX's four flag words and nineteen control characters, which settles
it: carrying `c_cflag` and the baud rates would have been forty fields
of decoration. The libc's `struct termios` therefore *omits* what it
cannot honour rather than accepting and ignoring it, so a program
setting a baud rate fails to compile instead of silently doing nothing.

**The risk the report led with arrived early, and from the tests.** An
early `tty-raw` failed *while the terminal was raw*, and every test
after it failed too -- the shell never started. The mitigation was
already designed (the release path resets the modes) and had simply not
been written yet. It is now, and the invariant is recorded.

**What the report did not predict:**

- **`tty_set_flags(t, 0)` changed meaning underneath its one caller.**
  The keyboard self-test used it to mean "no echo"; once `0` also meant
  "no `ICANON`, no `ISIG`" it put the console into raw mode for the rest
  of the boot. The function is deleted -- the `termios` pair replaces it
  -- so a caller has to say which modes it means. A widening set of
  flag bits silently reinterprets every literal already passed.
- **The Linux ABI test asserted the limitation.** `lxtest` checked that
  `TCGETS` returns `-ENOTTY`, pinning the very behaviour this unit
  removes. It now checks that the console answers and a plain file still
  does not.
- **The shell's redraw broke the boot harness.** Redrawing prints the
  prompt, so a full redraw per keystroke printed one prompt per
  character and the harness -- which counts prompts to pace itself --
  raced the shell badly. Two changes: typing at the end of a line echoes
  one character instead of redrawing, and the harness counts a prompt
  only at the start of a line.
- **`tty_of_handle` had to learn about files.** A program that opened
  `/dev/tty` *because* it had closed handle 0 was told the thing it had
  just opened was not a terminal, since the lookup only knew the console
  kobject. Fixing it produced a refcount underflow panic on the first
  try, because `file_from_kobject` converts without taking a reference.
  And fixing it in the native call alone left the Linux `ioctl` path
  answering the same question differently -- the second door again, the
  lesson of the personality's `setpgid`. Both now go through one
  `tty_of_open`, which is the only shape in which the two ABIs cannot
  drift apart.

**Seven of the eight behaviours are proved by reintroducing the bug**;
the eighth is recorded rather than counted. Disabling the raw *read*
branch leaves `tty-raw` passing, because the canonical reader also
returns a lone byte with no terminator -- only the `lines` bookkeeping
differs, and nothing a program can call observes it. The raw *input*
branch, by contrast, hangs the boot when removed, as does answering
`/dev/tty` for a session with no terminal, and leaving a released
terminal in raw mode fails four self-tests and the shell harness at once
-- which is the report's leading risk, demonstrated.

**Not built, and recorded in the gaps**: `VTIME` (accepted, no timed
read), output processing beyond the sink's newline translation,
reassignable control characters, and a redraw that knows the terminal's
width.
