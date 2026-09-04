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
