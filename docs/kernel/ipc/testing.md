# IPC: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| Target, kernel threads | Self-test `ipc-pipe` (`kernel/ipc/pipetest.c`) drives the end objects' operations directly | `make test` |
| User mode | `init --selftest`, `proc_selftest()`: `pipe`, `dup`, `dup2`, EOF, `-EPIPE`, `fstat`, a spawned `echo` writing into a pipe, a spawned `cat` blocked on one and killed | `make test` (self-test builds) |
| Shell | `/etc/rc.test` runs three-stage pipelines (`cat a | cat | cat > c`) and the interactive harness runs none, but every `sh -c` in the tests moves its output through a pipe in `init --selftest` | `make test` |

The boot test's total is `SELFTEST: PASS (61 tests)`; `ipc-pipe` runs
after `tty-ldisc` and before the process tests.

## Self-test (`kernel/ipc/pipetest.c`)

**`ipc-pipe`**:

| Step | Proves |
|---|---|
| A writer thread streams 1 MiB of a pattern in chunks of `(step * 613) % 9000 + 1` bytes; the test thread reads in chunks of `(step * 331) % 7000 + 1` and verifies every byte; the writer puts its end and the reader gets 0 | ordering, short reads, blocking both ways, EOF after the last write end (I2) |
| `read(rd, buf, 0)` | 0 without blocking |
| a new pipe, the read end put, `write` | `-EPIPE` (I2) |
| `write "abc"`, `stat` on the read end | `COSMO_DT_FIFO`, `size == 3`; the read returns `abc`; after the write end's put the read returns 0 |
| two writer threads each write 200 records of 1000 bytes (`0x11`, `0x22`) through one write end (`kobject_get` for the second reference); the reader assembles 1000-byte records | 400 records, every record uniform: writes of at most `PIPE_BUF` never interleave (I3) |
| `pipe_stats` | `created` grew by 4, `alive` is back to its start (I1) |

The test logs `selftest: ipc-pipe: 1024 KiB streamed, 400 records`.

## User-mode checks (`userland/init/init.c`, `proc_selftest`)

`pipe` gives two distinct handles at or above 3; `write` 3 bytes then
`fstat(h[0])` is a FIFO of size 3; `read` returns them; `read` on the
write end and `write` on the read end are `EBADF`; `dup` of the write end
keeps the pipe writable after the original is closed; `dup2(d, 40)`
lands at slot 40 and `dup2(d, 64)` is `EINVAL`; after the last write end
closes, `read` returns the two pending bytes and then 0; a fresh pipe
whose read end is closed gives `EPIPE` on write. `spawnvp("echo", ...)`
with the write end mapped to the child's handle 1 produces
`spawned child\n` on the read end and then EOF once the parent has closed
its own copy (the child's copy closed when it exited, before it was
reaped); `spawnvp("cat", ...)` with the read end as the child's handle 0
blocks, `waitpid(WNOHANG)` returns 0, `kill(pid, SIGKILL)` ends it with
status 137.

## Futex (Phase 11)

`futex_wait`/`futex_wake` are exercised through the Linux `futex` call
by `tests/linux/lxtest` (`-EAGAIN` on a mismatch, `-ETIMEDOUT` after 20
ms, a wake with no waiter returns 0, an unknown operation `-ENOSYS`);
`docs/compat/linux/testing.md`. No two-thread test exists yet.

## Gaps and planned tests

- No test kills a writer blocked on a full pipe.
- No test of many pipes at once (handle table pressure) or of a pipe
  handed to two children.
- Atomicity at the system-call boundary is 1024 bytes (`IO_CHUNK`); no
  user-mode test measures interleaving.
