# TTY: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**T1. `tty_input` never blocks, allocates or logs, and never releases
the tty lock while a byte is half processed.** It runs from the UART
interrupt handler (any CPU, interrupts off) and possibly during a
panic; its only calls out are `console_write` (IRQ-safe, polling) and
`waitqueue_wake_all` (IRQ-safe). Check: review of `tty_input` and
`commit`; the self-test `tty-ldisc` feeds thousands of bytes from thread
context, the boot harness types through the real interrupt path. Gap: no
assertion that `tty_input` is never entered with the tty lock already
held on the same CPU (it is not re-entrant, and nothing calls it from
under the lock today).

**T2. Only printable ASCII, tab and the editing keys change state; every
other byte is dropped.** The classifier in `tty_input` admits `0x20..0x7e`
and `\t` as characters, `\n` (and `\r` under `TTY_ICRNL`), `0x7f`/`\b`,
`^U` and `^D` as editing, and drops the rest without echo. No escape
sequence is ever interpreted. Check: `tty-ldisc` feeds `^C`, ESC and
`^A` inside a line and reads back only the printable characters. Gap:
bytes above 0x7f are dropped too, so there is no UTF-8 input yet.

**T3. Bounds are the two constants and nothing else grows.** A line
holds at most `TTY_LINE_MAX - 1` characters plus its terminator; the
ring holds at most `TTY_INPUT_MAX` bytes of records; overflow drops the
newest input (a bell is echoed, a counter grows) and never the oldest,
so a slow reader sees a consistent prefix of what was typed. Check:
`tty-ldisc` (1100 characters give 1023 plus the newline and 77
`dropped_bytes`; 100 hundred-byte lines leave 40 in the ring and
`dropped_lines > 0`, and the 40 are read back intact).

**T4. A record is delivered whole or as a prefix followed by its
remainder, never merged with the next.** `tty_read` stops after `'\n'`,
at the mark, or at `len`; `lines` is decremented only when a terminator
is consumed; an empty `^D` record returns 0 once and is then gone.
Check: `tty-ldisc` (a 3-byte buffer takes `hel` then `lo\n`; two lines
typed together are two reads; `^D` on an empty line reads 0 and the next
line reads normally; `abc^D` read with a 3-byte buffer leaves `lines`
at 0). Gap: no test with two competing readers.

**T5. A blocked reader wakes on every commit and on kill, and only
then.** `commit` calls `waitqueue_wake_all` after a successful record;
`tty_read` uses `wait_event_killable`, so `process_kill` on the reader's
process ends the wait with `-EINTR`. Check: `tty-ldisc` (a reader thread
blocks for 20 ms with no input and completes when a line arrives);
`process-spawn` kills `init --block`, which is blocked in a console
read, and sees status 143 within 2 s. Gap: a woken reader that loses the
race for the record goes back to sleep correctly (Mesa), but no test
provokes the race.

**T6. The console tty is the only consumer of console input and is
reached only through the console kobject.** `tty_console()` is static;
`serial_rx_irq` is registered with it as its argument; the only
`tty_read` caller outside the tests is `console_obj_read`. A process
reads the console only if it holds a handle to the console object
(inherited from init's handles 0, 1, 2 through `spawn`). Check: review;
`init --selftest` confirms `fstat(0)` reports a character device and
`isatty(0)`. Gap: no device node exists, so a process that closed handle
0 cannot get the console back.

**T7. Console input is enabled exactly once and only when the device
exists.** `arch_console_input_init` runs after `arch_irq_enable` and
`tty_init`; it requests, configures and unmasks the line, or logs a
warning and leaves input disabled. Check: the boot log line
`serial: console input on IRQ 4` is what the harness's `dmesg` check
looks for; the interactive harness proves bytes arrive. Gap: no test
boots without a UART.

**T8. `^C` and `^\` reach the terminal's foreground group and nothing
else, and a batch delivers every one of them.** The line discipline
raises `SIGINT`/`SIGQUIT` on every process of `fg_pgid`, echoes the
keystroke and throws the line under edit away; with no foreground group
the byte is dropped as any other control character. The signal is sent
after `tty->lock` is released, so the process table walk and the thread
wake-ups never happen under a lock taken in interrupt context, and
`tty.lock` is never held while the process table's lock is taken --
which is why the byte loop stops at each signal and resumes after
sending, rather than remembering one signal for the whole batch. Check:
`tty-intr` (a single write of `^\` then `^C` at a process that catches
the interrupt and dies of the quit); the interactive harness types a
bare `0x03` at a running `sleep`, which exits 130.

**T9. A terminal belongs to one session, and only that session names its
foreground group.** An unclaimed terminal is claimed by a session
leader; afterwards `tcsetpgrp` from another session is `-EPERM` and
`tcgetpgrp` is `-ENOTTY`, and the group named must have a member in the
terminal's session. When the session's leader exits the terminal is
released (`SIGHUP` first), so a dead session cannot keep the keyboard --
without which the shell could never claim the console after the
self-tests have used it. Check: `tty-intr` (a second session refused,
the release asserted after the leader exits).

**T10. A terminal is left usable.** Releasing a terminal -- which
happens when its session leader exits -- resets the modes as well as the
foreground group. A program that dies in raw mode has no shell left to
restore anything, so without this one crash leaves a machine nobody can
type at. Check: `tty-raw` and `tty-nosig` both leave the terminal
non-canonical when they fail, and every later test would fail with them
if the reset were not there (it did, before it was).

**T11. Canonical and raw input are never mixed.** The ring holds records
in one mode and bare bytes in the other, and `lines` counts records or
bytes to match. Changing `ICANON` drops what is queued, because the two
shapes cannot be told apart afterwards. Check: `tty-raw`, which switches
both ways and reads in each.

**T12. `ISIG` off does not loosen the job-control rules.** A background
reader is still refused, because that rule is about who may read rather
than what the bytes mean. Check: `tty-nosig` reads `^C` as byte 3;
`tty-ttin` is unaffected by it.

**T13. A terminal is what answers terminal calls, not what has a
character-device type.** `isatty` asks the terminal layer, so
`/dev/vmm` -- a character device -- is not one. It was, until this unit.
Check: `tty-isatty`.

**T14. A mode change never strands a reader.** A thread already blocked
in `tty_read` arrived under the old modes; `tcsetattr` wakes every
reader so each re-reads them. This matters for `VMIN` 0, which withdraws
the promise the sleeping thread is waiting on -- "answer with whatever
is there, including nothing" cannot be honoured by a thread that is
still asleep. Check: `tty-ldisc`, where a reader blocked under `VMIN` 1
is released, returning 0, by a `tcsetattr` that sets `VMIN` 0 and
nothing else.

## Gaps (documented, not invariants)

- **The raw *read* branch is not distinguishable by any test here.**
  Disabling it leaves `tty-raw` passing, because the canonical reader
  also returns a single byte that carries no terminator: it copies until
  the ring empties and stops. What actually differs is the bookkeeping
  -- the canonical path does not decrement `lines` for a record it never
  saw a terminator for, so the count grows without bound -- and nothing
  a program can call observes that. The branch is kept because the
  accounting is right; it is recorded here because nothing proves it.
  (The raw *input* branch is decisively proved: without it the boot
  hangs.)
- `VTIME` is accepted and not implemented: there is no timed read, so a
  `VMIN`/`VTIME` combination asking for one behaves as `VMIN` alone. The
  visible cost is in the shell: with no way to wait a moment for the
  rest of an escape sequence and give up, `Escape` alone blocks the line
  editor until the next key is pressed. That key is then handled as a
  keystroke rather than swallowed, so nothing is lost -- but the pause
  is real, and only `VTIME` removes it.
- No output processing beyond the serial sink's `\n` to `\r\n`: no
  `OPOST` to turn off, and the Linux translation reports it always on.
- The control characters are fixed: `^C`, `^\`, `^Z`, `^U`, `^W` and
  backspace cannot be reassigned, so `c_cc` carries only `VMIN`/`VTIME`.
- Line editing does not know the terminal's width: a line longer than
  the screen wraps in the terminal's own way and the shell's redraw does
  not account for it.
- No `TOSTOP`: a background process writing to the terminal is not sent
  `SIGTTOU` (Linux's default too). Reads are stopped and `tcsetpgrp`
  from the background does raise it.
- One tty; no pseudo-terminals. (`/dev/console` and `/dev/tty` exist
  since the terminal-modes unit.)
- Only the UART feeds the tty; the virtio-console receive queue and a
  keyboard driver are future producers.
- Output processing is the serial sink's `\n` to `\r\n` only.
