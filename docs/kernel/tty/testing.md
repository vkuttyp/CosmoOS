# TTY: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Target, private tty | Self-test `tty-ldisc` (`kernel/tty/ttytest.c`): the line discipline through `tty_input` and `tty_read` with echo off | `make test` |
| Target, real interrupt path | `tests/boot/shelltest.py` types commands into QEMU's serial port (its stdin) after each `cosmo$ ` prompt; the tty echoes them and the shell runs them | `make test`, release included |
| User mode | `init --selftest`: `fstat(0)` is a character device, `isatty(0)`, a zero-length console read returns 0 without blocking | `make test` (self-test builds) |
| Kill of a blocked reader | Self-test `process-spawn` kills `init --block` (a console read) and requires status 143 | `make test` |
| Boot marker | `[ INFO] serial: console input on IRQ 4` must appear in the `dmesg` output the harness requests | `make test` |

The boot test's total is `SELFTEST: PASS (61 tests)`; `tty-ldisc` runs
after the network tests and before `ipc-pipe` and the process tests.

## Self-test (`kernel/tty/ttytest.c`)

**`tty-ldisc`** uses a static `struct tty` set up with `tty_setup` and
`TTY_ECHO` cleared so nothing reaches the console:

| Input | Expected |
|---|---|
| `abc`, DEL, `d`, `\n` | one read of `abd\n` (4 bytes) |
| `xyz`, `^U`, `q`, `\n` | `q\n` |
| `\b`, `nothing to erase`, eight `\b`, `\n` | `nothing \n` (backspace on an empty line does nothing) |
| `a\rb\r` | two reads, `a\n` then `b\n` (CR becomes NL) |
| `hello\n` read with a 3-byte buffer | `hel`, then `lo\n` |
| `^D` alone | read returns 0 (end of file); the next line reads normally |
| `par^D` | `par` (3 bytes, no newline) |
| `abc^D` read with a 3-byte buffer | `abc`; `lines` is 0 afterwards (the mark went with the text) |
| `x`, `^C`, `y`, ESC, `^A`, `z`, `\n` | `xyz\n` (control bytes dropped) |
| 1100 `a` then `\n` | `dropped_bytes` = 77; the read returns 1024 bytes ending in `\n` |
| 100 lines of 99 `a` + `\n` | `dropped_lines > 0`, `lines == 40`; 40 reads of 100 bytes drain the ring (`used == 0`) |
| a reader thread with nothing queued, then `wake\n` after 20 ms | the thread was still blocked, then returned `wake\n` |
| a reader thread blocked non-canonically under `VMIN` 1, then a `tcsetattr` setting `VMIN` 0 | the thread was still blocked, then returned 0 -- a mode change releases a reader it no longer promises anything to |
| `tty_read(t, buf, 0)` | 0 without blocking |
| final statistics | `eofs == 1`, `lines_in > 0` |

## Interactive harness (`tests/boot/shelltest.py`)

`run_boot_test.py` starts QEMU with a pipe as its stdin (the serial
port's chardev). A thread follows the serial log; when it holds the
`n`-th `cosmo$ ` prompt it writes the `n`-th command from the list in
`shelltest.py` followed by `\n` and waits for the next prompt. Every
byte crosses the UART receive interrupt, `tty_input`, the echo path and
`tty_read` in the shell. Required afterwards (all builds):
`^interactive-ok$`, `sh` and `cat` in the `ls /bin` output, an `init`
line and a `ps` line in the `ps` output, `^/$` and `^/tmp$` from `pwd`,
`kernel.name = CosmoOS`, the serial input line in `dmesg`,
`sh: nosuchprogram: not found`, and finally `exit 0`, after which the run
must end through `init: shell exited with status 0`. Failures are
reported as `shell harness: ...` lines by `run_boot_test.py`.

## The foreground group (`tty-intr`, `kernel/process/proctest.c`)

Driven from both ends, because neither end alone proves it. A user
process (`init --probe signal-tty`) claims the console with
`tcsetpgrp` and waits; the kernel side polls `tty_foreground_pgrp` until
it is that process's group, then runs a second process from another
session (`signal-tty-steal`) which must be refused `tcsetpgrp`
(`-EPERM`) and `tcgetpgrp` (`-ENOTTY`); then it types `abc` and `^C` and
requires the child to exit 130, no line to have been committed (the
partial line is discarded -- checked through `lines_in`, because the
console may already hold input the harness typed), and the terminal to
be released once its session leader is gone.

A second phase then checks that a batch delivers every signal it
carries: one process with `SIGINT` caught and `SIGQUIT` left fatal, and
one `tty_input` of `^\` followed by `^C`, which must end it with 131. A
line discipline that remembered only the last signal of a batch would
send the interrupt alone, the handler would run, and the process would
still be there.

## Terminal modes (`tty-raw`, `tty-nosig`, `tty-isatty`, `dev-tty`, `dev-tty-none`)

The first three are driven from both ends, because the kernel has to
type: the probe claims the terminal and changes its modes, and the
kernel side **waits for the mode rather than for a handshake** before
typing. A byte typed while the line discipline was still canonical would
be edited rather than delivered, and a kernel-created probe has no spare
handle to say "ready" on -- the terminal's own state is the readiness
signal, which is both simpler and impossible to get out of step.

**Only a probe that is typed at is waited for.** A probe with nothing to
type claims the terminal, does its work and exits, and releasing the
terminal on the way out puts the foreground group back to 0: watching
for the claim is watching a window that closes on its own, and a run
that sampled a moment late called a probe that had already passed a
failure. `dev-tty` did exactly that once, on `nic-virtio aarch64`, with
its own probe logged as exiting 0 two lines above. What such a probe
proves, it proves by its exit status. Where a wait is needed the loop
**sleeps rather than yields**, because the thing it is waiting for is a
process that needs the CPU to get there.

- **`tty-raw`** -- a terminal starts cooked; `cfmakeraw` turns echo,
  canonical mode and signals off and reads back as it was set; one byte
  is readable with no newline ever typed; and restoring the saved modes
  brings line-at-a-time reads back.
- **`tty-nosig`** -- with `ISIG` off, `^C` arrives as byte 3. The probe
  would die if the signal still arrived, so surviving to report is the
  check.
- **`tty-isatty`** -- true for the console, **false for `/dev/vmm`**.
  It was true for both until this unit.
- **`tty-pollraw`** -- with `VMIN` 0 and an empty terminal, `ioready`
  reports readable and the read returns 0. Readiness that consulted
  only the queue said "would block" about a read that returns at once,
  which parks a poll or an I/O ring entry until something is typed.
- **`dev-tty`** -- `/dev/tty` and `/dev/console` open and are terminals.
- **`dev-tty-none`** -- a process whose session holds no terminal gets
  `-ENXIO`. It does not call `setsid` to get there: a probe the kernel
  starts already leads a session of its own, and a session leader is
  refused `setsid` anyway.

The interactive boot test types the editing itself: `echo edit-okXY`
followed by two backspaces must run `echo edit-ok`, and four left arrows
followed by `-2` must run `echo -2edit`. Those are the first entries in
the harness to send an escape sequence.

## Bring-up findings

- `irq_request` registers and routes a legacy IRQ but leaves it masked;
  the first receive path never fired until `irq_enable` was added.
  Recorded in `api.md` so the next legacy-IRQ driver does not repeat it.
- Typing into QEMU's `-serial stdio` from a pipe works; the serial log
  shows the echoed command right after the prompt.

## Gaps and planned tests

- No test with two readers competing for one record.
- No test of `TTY_ICRNL` or `TTY_ECHO` turned off from user space
  (there is no interface to do so).
- No fuzzing of `tty_input` with random bytes; the classifier is
  reviewed and the control-byte case is covered by one line.
- Raw mode and pseudo-terminals arrive with their own tests.

## Input from a keyboard

`hid-arm` and `hid-keyboard` (`kernel/device/hidtest.c`) are the tty's end-to-end test
with a real device in front of it: the boot harness types on QEMU's
emulated USB keyboard over the monitor protocol, the driver hands the
characters to `tty_input` from interrupt context, and the test reads the
line back with `tty_read` and compares it exactly. Shift produces
capitals and symbols, so the line covers more than the echo path does.
The tty's own counters make the check strict: the bytes must have
arrived while the test waited (`rx_bytes` and `lines_in` both move) and
nothing may be dropped.

The pair is split because the typing happens on the host and takes as
long as the host takes: `hid-arm` asks for it (and turns echo off, so
what arrives does not land in the middle of a log line), everything else
in the run happens, and `hid-keyboard` reads the lines at the end and
puts echo back for the shell. See `docs/drivers/usb/testing.md`, "The
keyboard", for the harness side and the shapes (`QEMU_KBD=root`, `hub`,
`0`).
