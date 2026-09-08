# TTY: design

## Data structures

```c
#define TTY_LINE_MAX   1024u    /* one line under edit */
#define TTY_INPUT_MAX  4096u    /* completed lines waiting for readers */

struct tty {
    spinlock_t lock;                 /* IRQ-safe: tty_input runs in interrupt context */
    pid_t sid;                       /* the session that controls this terminal; 0 for none */
    pid_t fg_pgid;                   /* the foreground process group within it; 0 for none */
    /* line under edit (canonical mode) */
    uint8_t line[TTY_LINE_MAX];
    unsigned line_len;
    /* completed input: records, each a line's bytes plus one terminator */
    uint8_t ring[TTY_INPUT_MAX];
    unsigned head, tail, used;       /* head = next byte to read, tail = next free */
    unsigned lines;                  /* records in the ring */
    struct waitqueue readers;
    struct tty_stats { uint64_t rx_bytes, lines_in, lines_read, dropped_lines, dropped_bytes, eofs; } stats;
    unsigned flags;                  /* TTY_ECHO (on), TTY_ICRNL (on); room for a raw mode */
    const char *name;
};
```

The ring holds records: a line's bytes followed by one terminator,
`'\n'` for a line ended by newline (or carriage return) or the mark
byte `0x04` (`TTY_EOF_MARK`) for a line ended by `^D`. An empty record
whose only byte is the mark is an end of file. Since printable input
never contains `0x04` (control bytes other than the editing keys are
dropped before they reach the ring) the mark is unambiguous. A read that
asks for fewer bytes than a record holds takes a prefix and leaves the
rest; `lines` counts records, which is what a reader waits on.

## Algorithms

### tty_input(tty, bytes, n) — interrupt context

Under `tty->lock` for each byte `c`:

1. `\r` becomes `\n` (`TTY_ICRNL`).
2. `\n`: echo `\n`, then *commit* with the terminator `'\n'`: copy the
   line and the terminator into the ring if `TTY_INPUT_MAX - used >=
   line_len + 1`, `lines++`, wake readers (`waitqueue_wake_all`,
   interrupt-safe); else count `dropped_lines` and echo a bell. Reset
   `line_len` either way.
3. `0x7f` or `\b`: if `line_len > 0`, `line_len--` and echo `"\b \b"`.
4. `^U` (0x15): echo `"\b \b"` per character, `line_len = 0`.
5. `^D` (0x04): commit with the mark as terminator. An empty line gives
   an empty record (end of file, `eofs++`); a partial line gives a
   record without a newline (Unix semantics: the read returns the
   partial line).
6. `^C` (0x03) or `^\` (0x1c) **when the terminal has a foreground
   group**: echo `"^C"` (or `"^\"`) and a newline, throw the line under
   edit away, and raise `SIGINT` (or `SIGQUIT`) on every process of that
   group. The line is discarded because what was typed before an
   interrupt is not part of the next command. With no foreground group
   these are dropped as any other control byte, which is what a terminal
   nobody has claimed does.
7. Printable (0x20..0x7e) or tab: if `line_len < TTY_LINE_MAX - 1`,
   append and echo; else count `dropped_bytes` and echo a bell.
8. Anything else: dropped silently.

The signal is sent **after** `tty->lock` is released: sending walks the
process table and wakes threads, and this loop runs in interrupt
context, so nothing that long belongs under the lock. That also keeps
the lock order simple -- `tty.lock` is never held while the process
table's lock is taken.

So the byte loop is `feed_locked`, which stops at the first byte that
raises a signal and tells `tty_input` what to send; `tty_input` sends it
with the lock dropped and comes straight back for the rest. The
alternative -- one pass recording "the signal to send" -- loses every
signal in a batch but the last, and `tty_input` takes a batch: one write
of `^\` then `^C` has to deliver both, in that order.

Echo happens through `console_write`, which takes its own IRQ-safe lock
and only polls the UART; it is called with `tty->lock` held. Lock order
therefore is `tty.lock → console.lock`, and console code never calls
into the tty (so no inversion). The serial sink's `\n → \r\n`
translation applies to echo as well.

### tty_read(tty, buf, len) — thread context

```text
if len == 0: return 0
loop:
    wait_event_killable(&tty->readers, tty->lines > 0)      -> -EINTR when the process is being killed
    lock; if lines == 0 (another reader won): unlock, loop
    copy bytes from head while used > 0:
        the mark: consume it (not delivered), record ended, stop
        n == len: stop (the rest waits for the next read)
        deliver the byte; '\n' ends the record, stop
    if the record ended: lines--
    unlock
    return copied            (0 only for an empty ^D record: end of file)
```

A read never merges two records and never returns a partial one unless
the caller's buffer is smaller than the record (then the remainder is
returned by the next read, still followed by its terminator; a mark
that then heads the ring is consumed with the tail, so it cannot become
a spurious end of file). Several readers may race for the same line;
the first to take the lock wins and the others go back to waiting: the
console is a shared resource, and the shell/init discipline of "one
foreground reader" is what makes it well-behaved, as on Unix without job
control.

### The console tty and the UART

`tty_init()` (called from `kernel_main` after `sched_init`, before
`process_init`) sets up `g_console_tty`. `arch_console_input_init()`
(x86-64: `kernel/arch/x86_64/serial.c`, called after `arch_irq_enable`)
registers `serial_rx_irq` for ISA IRQ 4 through `irq_legacy_to_gsi` and
`irq_request`, enables the receive data available interrupt (IER bit 0),
and unmasks the line with `irq_enable` (which `irq_request` leaves
masked). The handler drains the FIFO:

```text
while (LSR & DATA_READY): byte = RBR; tty_input(tty_console(), &byte, 1)
```

The handler runs on the CPU the GSI is routed to (CPU 0 by the existing
IRQ layer); `tty_input` is safe from any CPU. When no UART is present
(no `serial0` sink) nothing is registered and the console tty simply
never receives input.

### The controlling terminal

A terminal belongs to a *session*, and within that session one *process
group* is in the foreground: the group whose processes `^C` and `^\`
signal (`docs/kernel/process/design.md`, "Sessions and process groups").
Two fields hold it, both under `tty->lock`, and three rules govern them:

- **Claiming.** `tty_set_pgrp` on a terminal with no session (`sid ==
  0`) claims it for the caller's session, provided the caller *leads*
  that session. A process that is not a session leader, calling on an
  unclaimed terminal, is refused: leading a session is what makes a
  terminal yours to take.
- **Using.** Once claimed, only processes of that session may name a
  foreground group, and only a group of that session -- checked through
  `process_group_in_session`, outside `tty->lock`, because it walks the
  process table. `tty_get_pgrp` answers `-ENOTTY` to anyone outside the
  session: to them this is not a controlling terminal at all.
- **Releasing.** When the session's leader exits, the terminal is
  released -- `SIGHUP` to what was its foreground group, then `sid` and
  `fg_pgid` back to 0 -- so the next session leader can claim it. Without
  this rule a terminal would be owned by a dead session forever, and the
  next shell would never get the keyboard.

That is the whole session model the tty needs. The alternative the
design considered -- remembering one "controlling process" instead of a
session -- was rejected in `docs/audit/next-subsystem-signals.md` for
being the same machinery with a worse name: it still has to answer who
may take the terminal, who inherits it, and when it is released, which
are the three questions a session answers.

`tty_of_object` maps a handle's kobject to a tty. The console object is
the only terminal there is, so the mapping is a comparison; a second tty
would put a `struct tty *` in the object instead.

### The console kobject

`console_obj_read` → `tty_read(tty_console(), buf, len)`. `sys_read`
issues one object read of at most `IO_CHUNK` (1024) bytes, so a typed
line of up to 1024 bytes comes back in one call and a longer one in
several. `console_obj_write` is unchanged. A `stat` operation is added
to `struct kobject_io_type` (`int (*stat)(obj, struct cosmo_stat *)`),
and the console reports `type = COSMO_DT_CHR`, size 0, mode 0620; the
same hook lets pipe ends report `COSMO_DT_FIFO` and sockets
`COSMO_DT_SOCK`, so `sys_fstat` works on files, the console, pipes and
sockets.

## Where the bytes come from

`tty_input` has three callers now, and they are all the same shape: a
device driver with a byte in interrupt context.

- `kernel/arch/x86_64/serial.c` and `kernel/arch/aarch64/pl011.c`: a
  UART interrupt, one byte at a time.
- `drivers/usb/usb_hid.c`: a USB keyboard's interrupt endpoint, whose
  eight-byte reports the driver turns into presses and then characters,
  including the control bytes this line discipline already understands
  (`^D`, `^U`, backspace). It is a module, which is why `tty_console`
  and `tty_input` are exported.

Nothing above the tty can tell them apart, which is the point: the
shell blocks in `tty_read` on a machine with a serial cable and on a
machine with a keyboard, and the same test script drives both. The line
discipline gained nothing for the keyboard -- no key codes, no modifier
state, no event queue -- because a keyboard driver's job is to produce
characters, and everything else it might produce has no consumer (§21).

## Ownership and lifetime

The console tty is static and lives forever. A private tty created by
the self-test is a static object (5 KiB does not belong on a kernel
stack); its reader thread is joined before the test returns. No tty is ever
freed by the design, which is why there is no reference count; a
dynamic tty (pseudo-terminals) would embed a `kobject`.

## Concurrency

One spinlock per tty, IRQ-safe, held for a bounded number of byte
operations (a commit copies at most `TTY_LINE_MAX` bytes). Readers
block on `readers`; wakers are the interrupt handler and, for kill,
`process_kill` through the thread's `waiting_on` queue. `tty_input`
must never block, allocate, or log (it may run on the panic path when
someone types during a panic; the echo path tolerates panic mode).

## Memory

All static: `sizeof(struct tty)` ≈ 5.2 KiB. No allocation on any path.

## Error handling

`tty_read` returns `-EINTR` only when the process is being killed;
`-EFAULT` handling is the caller's (`sys_read` copies through a kernel
buffer). Overflow drops the newest input with a bell and a counter,
never the oldest (a reader that is slow sees a consistent prefix of what
was typed). Malformed bytes cannot exist: every byte is classified.

## Performance

Irrelevant at a human's typing rate; the design keeps interrupt time
bounded and echo cost proportional to the bytes typed.

## Security

Input bytes are untrusted: the classifier admits only printable ASCII,
tab, newline and the four editing keys; nothing is interpreted (no
escape sequences). Lengths are bounded by the two constants. Which
process reads the console is decided by handle inheritance (a process
without handle 0 cannot read it), which `spawn` makes explicit.

## Future extensibility

- Raw mode and `termios` through a `tty_set_mode` call and an `ioctl`
  system call; the classifier becomes a table indexed by mode.
- A foreground process group and `^C`/`^Z` when signals exist.
- Pseudo-terminals: `struct tty` with a kobject and a master side.
- More producers: PS/2 keyboard (scancode translation lives in the
  driver, the tty sees bytes), virtio-console receive.
- Device nodes (`/dev/console`, `/dev/tty`) once the VFS has them; the
  console kobject then becomes the file's backing object.
