# TTY: architecture

Phase 9 of the roadmap (userland needs a keyboard). Constitution
sections 11 (handles, not pointers), 18 (system-call layering), 45
(a traditional Unix userland), 55 (the serial console is the first
diagnostic), invariant 1 (generic code does not depend on the CPU
architecture) and invariant 14 (privileged interfaces validate
untrusted input; here: bytes from the wire).

## Where it sits

```text
   user process       read(0, ...) / write(1, ...) on the standard handles
        │             kernel/syscall/native.c → handle table → console kobject
        ▼
   kernel/object/console_obj.c    the console kobject: write → console_write, read → tty_read
        │
        ▼
   kernel/tty/tty.c               the line discipline: input queue, canonical editing, echo, readers
        ▲                                  │ echo
        │ tty_input()                      ▼
   kernel/arch/x86_64/serial.c    UART receive interrupt (IRQ 4)      kernel/core/console.c → sinks
   (later: keyboard, virtio-console receive)
```

The console kobject that Phase 4 installed as handles 0, 1 and 2 of
every process keeps its identity. Its write path is unchanged. Its read
path, which returned 0 bytes until now, becomes a read from the console
tty. Nothing above the object layer learns a new type.

## Purpose

Let a process read what a person types. The kernel collects bytes from
the console device in interrupt context, assembles them into lines with
the classic editing keys, echoes them, and hands complete lines to
readers in thread context, one `read` at a time. Without this the shell
cannot exist.

## Responsibilities

- **Input from any context** (`tty_input`): the UART interrupt handler
  delivers received bytes; the line discipline runs under one IRQ-safe
  spinlock and never blocks, allocates or logs.
- **Canonical line editing**: erase (`^H`, `DEL`), kill line (`^U`),
  end of file (`^D`), newline (`\n`, and `\r` translated to `\n`
  because serial terminals send carriage return). Printable bytes are
  appended; other control bytes are dropped. A line is at most
  `TTY_LINE_MAX` (1024) bytes; further input before the newline is
  dropped and the terminal bell is echoed.
- **Echo**: what is accepted is echoed through `console_write`
  (already interrupt-safe and non-blocking); an erase echoes
  `"\b \b"`, a kill line erases the whole line the same way.
- **Delivery to readers** (`tty_read`): complete lines sit in a ring
  (`TTY_INPUT_MAX` 4096 bytes); a read blocks until at least one
  complete line exists and returns at most one line (never more than
  `len`, the remainder stays queued). `^D` on an empty line delivers an
  end of file: `read` returns 0 once, and the next read blocks again.
  Blocking is killable: `kill` on the reading process makes the read
  return `-EINTR` and the process exits. A line ended by `^D` is
  delivered without a newline.
- **Overflow policy**: when the completed-line ring is full the newest
  line is dropped and counted; input never blocks the interrupt path.
- **The console tty**: one `struct tty`, created at boot, fed by the
  serial receive interrupt, read through the console kobject. The
  kobject's `fstat` reports a character device.

## Non-responsibilities

- Raw (non-canonical) mode, `termios`, window size, `ioctl`. Programs
  see one canonical, echoing terminal; an editor or a program that wants
  keystrokes waits for a later phase (the structure is a `flags` word
  away).
- Job control, a foreground process group, `^C` and `^Z` delivering
  signals: there are no signals beyond kill in this phase (see
  `docs/kernel/process/`). `^C` is dropped.
- Pseudo-terminals, multiple ttys, `/dev/tty` and `/dev/console` nodes:
  the VFS has no device nodes yet; processes reach the console only
  through inherited handles.
- Output processing beyond what the serial sink already does (`\n` →
  `\r\n`).
- Keyboard (PS/2, USB) and virtio-console receive: the interface is
  ready (`tty_input` from any producer); the drivers are not.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `struct tty`, `tty_setup`, `tty_init`, `tty_console`, `tty_input`, `tty_read`, `tty_get_stats` | `kernel/tty.h` | serial receive, the console kobject, self-tests |
| `arch_console_input_init` (arch; x86-64 in `serial.c`) | `arch/console.h` | kernel_main after `arch_irq_enable` |
| console kobject `read`/`write`/`stat` | `kernel/object.h` | `sys_read`, `sys_write`, `sys_fstat` |

Tests (`testing.md`): a self-test (`tty-ldisc`) feeds bytes through
`tty_input` on a private tty and checks line assembly, erase, kill,
`^D`, `\r` translation, the length limit and the ring limit with a
reader thread; the boot harness types commands to the shell over the
QEMU serial port and checks their output, which exercises the interrupt
path end to end.
