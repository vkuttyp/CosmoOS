# TTY: API

Every entry follows constitution section 52: purpose, inputs, outputs,
ownership, lifetime, concurrency, blocking, interrupt context, failure
modes, ABI stability. Everything here is kernel-internal (**ABI
stability: internal**); user space reaches the console tty only through
the console handle's `read`, `write` and `fstat` system calls, whose
contract is in `docs/kernel/syscall/api.md`.

## Shared contracts

- One spinlock per tty, taken `irqsave`; `tty_input` runs under it in
  interrupt context and never blocks, allocates or logs. Lock order:
  `tty.lock → console lock` (echo). Console code never calls into the
  tty.
- Readers block on `tty->readers` with `wait_event_killable`; the
  interrupt handler wakes them with `waitqueue_wake_all`.
- The ring holds *records*: a line's bytes followed by one terminator,
  `'\n'` for a line ended by newline (or carriage return) or the mark
  byte `0x04` for a line ended by `^D`. An empty record whose only byte
  is the mark is an end of file. `lines` counts records.

## kernel/tty.h

### Constants and flags

| Name | Value | Meaning |
|---|---|---|
| `TTY_LINE_MAX` | 1024 | line under edit, including its terminator; 1023 characters at most |
| `TTY_INPUT_MAX` | 4096 | bytes of completed records waiting for readers |
| `TTY_ECHO` | bit 0 | echo accepted input through `console_write` (on by default) |
| `TTY_ICRNL` | bit 1 | `'\r'` becomes `'\n'` (on by default) |

### `struct tty`
`lock`; `line[TTY_LINE_MAX]`, `line_len` (the line under edit);
`ring[TTY_INPUT_MAX]`, `head`, `tail`, `used`; `lines` (complete records
in the ring); `readers` (wait queue); `stats`; `flags`; `name`. About
5.2 KiB; never placed on a stack.

### `struct tty_stats`
`rx_bytes` (bytes handed to `tty_input`), `lines_in` (records
committed), `lines_read` (successful `tty_read` calls), `dropped_lines`
(records that did not fit the ring), `dropped_bytes` (characters beyond
the line limit), `eofs` (empty `^D` records committed).

### `void tty_setup(struct tty *t, const char *name)`
- Purpose: zero the structure, initialise the lock and wait queue, set
  `TTY_ECHO | TTY_ICRNL`.
- Concurrency: once, before the tty is shared. Self-tests use it on a
  private static tty and clear `TTY_ECHO`.

### `void tty_init(void)`, `struct tty *tty_console(void)`
- Purpose: set up the console tty (`kernel_main`, after `sched_init`,
  before `process_init`) and return it. Static, lives forever.

### `void tty_input(struct tty *t, const uint8_t *bytes, size_t n)`
- Purpose: deliver received bytes; the line discipline runs here.
- Inputs: any bytes; classified one at a time (`docs/kernel/tty/design.md`).
- Effects: printable ASCII and tab append to the line (echoed); `\r`
  becomes `\n` (`TTY_ICRNL`); `\n` echoes and commits a record; `0x7f`
  and `\b` erase (echo `"\b \b"`); `^U` (0x15) kills the line; `^D`
  (0x04) commits the line with the mark terminator (an EOF record when
  the line is empty); other control bytes are dropped, `^C` included.
  A character beyond `TTY_LINE_MAX - 1` is dropped with a bell
  (`dropped_bytes`); a record that does not fit the ring is dropped
  with a bell (`dropped_lines`); the line under edit is reset either way.
- Concurrency: any context, including the UART interrupt handler and
  the panic path; `waitqueue_wake_all` on a commit is interrupt-safe.
- Failure modes: none reported; overflow is counted.

### `int64_t tty_read(struct tty *t, void *buf, size_t len)`
- Purpose: deliver one record, or a prefix of it, to a reader.
- Inputs: a kernel buffer (`sys_read` bounces through its 1024-byte
  chunk buffer) and its length; `len == 0` returns 0 at once.
- Outputs: the number of bytes copied. Copying stops after a `'\n'`
  (included) or at the mark (consumed, not delivered) or when `len` is
  reached (the rest of the record stays for the next read; a mark that
  then heads the ring is consumed with the next read of the record's
  tail, so a `^D`-ended line whose text exactly fills the buffer does
  not become a spurious end of file). 0 means an empty `^D` record: end
  of file for this read only, the next read blocks again.
  `-EINTR` when the calling process is being killed.
- Blocking: `wait_event_killable(&t->readers, t->lines > 0)`; several
  readers may race for one record, the first to take the lock wins and
  the others wait again. Thread context only.
- Ownership: nothing; bytes are copied.

### `void tty_get_stats(struct tty *t, struct tty_stats *out)`
Snapshot under the lock. For tests and diagnostics.

## The console kobject (`kernel/object/console_obj.c`)

`read(obj, buf, len)` is `tty_read(tty_console(), buf, len)`;
`write(obj, buf, len)` is `console_write` and returns `len`;
`stat(obj, st)` fills `type = COSMO_DT_CHR`, `mode = 0620`, `nlink = 1`,
everything else 0. The object is static with refcount 1; its release
panics. See `docs/kernel/object/api.md`.

## arch/console.h

### `void arch_console_input_init(void)`
- Purpose: route the console device's receive interrupt into the
  console tty. Called by `kernel_main` after `arch_irq_enable()`.
- x86-64 (`kernel/arch/x86_64/serial.c`): when the UART probe succeeded,
  `irq_legacy_to_gsi(4)` gives the GSI and trigger flags, `irq_request`
  registers `serial_rx_irq` (drains the receive FIFO while `LSR.DR` is
  set, one `tty_input` per byte) on CPU 0, the receive-available bit is
  set in the UART's IER, and `irq_enable` unmasks the line
  (`irq_request` leaves it masked). Logs `serial: console input on IRQ
  4`. Without a UART, or when the request or enable fails (logged at
  WARN), the console tty never receives input and reads block for ever.
- Concurrency: boot CPU, once.

## Failure modes

| Condition | Behaviour |
|---|---|
| line longer than 1023 characters | extra characters dropped, bell echoed, `dropped_bytes` |
| ring full when a line completes | line dropped, bell echoed, `dropped_lines`; older input kept |
| `^C`, escape sequences, other control bytes | dropped silently |
| reader killed while blocked | `tty_read` returns `-EINTR`; the process exits at the system-call boundary |
| no UART or IRQ request failed | input never arrives; `sys_read` on handle 0 blocks (killable) |
