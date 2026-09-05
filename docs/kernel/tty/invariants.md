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

## Gaps (documented, not invariants)

- No raw mode, no `termios`, no window size, no `ioctl`.
- No job control and no signals from the keyboard: `^C` is dropped.
- One tty; no pseudo-terminals; no `/dev/console` or `/dev/tty` nodes.
- Only the UART feeds the tty; the virtio-console receive queue and a
  keyboard driver are future producers.
- Output processing is the serial sink's `\n` to `\r\n` only.
