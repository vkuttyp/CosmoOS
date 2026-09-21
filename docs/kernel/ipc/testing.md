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
`docs/compat/linux/testing.md`. The native door's tests are
`userland/tests/thrtest.c` steps 23–27 (`docs/libc/testing.md`, "The
native thread door"): eight waiters requeued by one broadcast, the
handoff chain across every interleaving libc's probe can reach, a
requeued waiter timing out on the word it was moved to, and a concurrent
requeue answered `-EAGAIN`. The same-address walk is proved by putting
the move back: the boot stops at step 23's sleeper count, which requeues
the condition's word onto itself.

## Non-blocking mode and readiness (milestone 8)

`net-nonblock` (`kernel-services/network/nettest.c`) drives the pipe ends
through the object operations: `-EAGAIN` on an empty read, exactly
`PIPE_SIZE` bytes written before `-EAGAIN`, `WRITABLE` clear when full
and back after a read, `HANGUP` on the read end after the write end is
released. `init --selftest` repeats it through `setnonblock`/`ioready`
(`read` `-EAGAIN`, the read end not ready and the write end `WRITABLE`,
`READABLE` after one byte, `HANGUP` then 0 after the writer closes);
`lxtest` covers `pipe2(O_NONBLOCK)`, `fcntl(F_GETFL/F_SETFL)` and the
same fill-to-`EAGAIN`.

## Unix domain sockets (the unix-sockets unit)

Six self-tests in `kernel/ipc/unixtest.c`, each counting live unix
sockets (and pipes, where it makes any) before and after -- the leak
half of invariant I8 asserted, not hoped for:

- `unix-stream`: a name bound and listened on; connect completes
  before accept and the bytes wait; both ways; `shutdown(WR)` reads 0
  on the other side while the reverse still flows; the client gone,
  the server's write is `-EPIPE` and its read 0; a backlog of two,
  the third connect `-EAGAIN` non-blocking and admitted after an
  accept; a released listener refuses its queue (0, `-EPIPE`) while the
  accepted connection lives; a dead name `-ECONNREFUSED`, a regular
  file `-ECONNREFUSED`, no such path `-ENOENT`, a datagram socket's
  name `-EPROTOTYPE`.
- `unix-dgram`: `sendto` by name, the sender's name back (none, then an
  abstract one), a default destination and a reply to the name that
  came with it, truncation flagged and the remainder dropped,
  `-EMSGSIZE`, the queue's message bound then `-EAGAIN` and room again,
  a released receiver refused by name and by default destination.
- `unix-name`: `bind` makes a `DT_SOCK` node of mode 0755, `open` is
  `-ENXIO`, a second bind of the path and of an abstract name
  `-EADDRINUSE`, binding twice `-EINVAL`, `unlink` then connect `-ENOENT`
  with the existing connection alive, a filesystem without `mknod`
  (`/proc`) refused.
- `unix-handles`: a pipe end rides with the rights the sender named and
  the message's reference is seen and returned; a unix socket is
  `-EINVAL` and the caller keeps its reference; room for one of three
  installs the first and releases two with `HTRUNC`; no room asked for
  releases all; ancillary items stay with their bytes (a read stops at
  the boundary of a send with handles and never crosses into it); a
  socket released with a message queued releases the handles in it;
  `handle_transfer_check` on a table (SAME, a subset, more than held
  `-EPERM`, no TRANSFER `-EPERM`, no such handle `-EBADF`).
- `unix-close-race` (two CPUs): a blocked reader (0), writer (`-EPIPE`),
  connector on a full backlog (`-ECONNREFUSED`) and accepter (`-EINVAL`
  after `shutdown(RD)`) each released from the other CPU. The connector
  case is the one the build found: a connector that held a reference
  to the listener could never be released by the listener's close.
- `unix-poll`: readiness through the object's type -- a listener
  readable with a connection queued, a stream readable by its bytes,
  writable by its space (a full queue is not) and hung up by its peer,
  a datagram socket writable always.

`init --selftest`'s `unix` section (`docs/userland/testing.md`) drives
the doors: a pair with `SO_PEERCRED`, a child echoing over its end from
the spawn map and returning a file's bytes from a handle it received in
a message, `-EPERM` without TRANSFER, `-EINVAL` for a unix socket, a
child connecting to a path with the accepted socket naming the child's
pid, `EADDRINUSE`, a uid-1000 child refused `EACCES` by the node's
mode, a jailed child that reaches neither the abstract name nor the
path, `ECONNREFUSED` then `ENOENT` after `unlink`, datagrams by name
with the sender's abstract name back and `MSG_TRUNC`,
`ESOCKTNOSUPPORT`, and the bench. `lxtest` covers the Linux door
(`docs/compat/linux/testing.md`). The mutations run are in the report's
as-built banner.

## Gaps and planned tests

- No test kills a writer blocked on a full pipe.
- No test of many pipes at once (handle table pressure) or of a pipe
  handed to two children.
- Atomicity at the system-call boundary is 1024 bytes (`IO_CHUNK`); no
  user-mode test measures interleaving.
