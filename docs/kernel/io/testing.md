# Asynchronous I/O: testing

## User-mode checks (`userland/init/init.c`, `proc_selftest`)

Run by `init --selftest` on every debug boot (the boot test requires
`USERTEST: PASS`):

- `aio_create(0)` and `aio_create(8, flags 1)` are `-EINVAL`; a ring of
  eight entries is created.
- Four entries in one submission: a `READ` of an empty pipe (parks), a
  `NOP` (completes 0), a `READ` on handle 999 (completes `-EBADF`), a
  `READ` with `NOWAIT` (completes `-EAGAIN`). `ioready(ring)` reports
  `READABLE`; a poll (`min` 0) returns exactly the three completions,
  each `user_data` once; the ring is no longer readable.
- A 20 ms wait with `min` 1 returns 0; after `write(p[1], "ringdata")`
  a wait returns the parked read with 8 bytes and the data in the
  buffer.
- A `POLL` for `READABLE` on the empty pipe parks; a `WRITE` of two
  bytes completes at submission; a wait for both returns the poll with
  `COSMO_IO_READABLE` and the write with 2.
- Files: `PREAD` of four bytes at offset 6, `FSYNC`, `PWRITE` of two
  bytes at offset 0, all complete at submission with the expected
  results; the file reads back changed.
- Capacity: nine parked reads on the eight-entry ring: eight accepted,
  then `-EBUSY`; a poll returns nothing; `min > n` is `-EINVAL`.
- Closing the ring with parked reads, then the pipe, leaves nothing
  behind; `aio_submit` on a non-ring handle or a bad handle is `-EBADF`.

## The timer object (`init --selftest`, the aio-timer unit)

A timer from `timer_create` is a submittable readiness object
(`docs/audit/next-subsystem-aio-timer.md`). A 20 ms one-shot submitted as
`POLL` parks (`aio_wait` with `min` 0 returns nothing) and completes with
its `user_data` and `READABLE` once it fires; a `READ` returns the
expiration count (1) and resets it, so a following `NOWAIT` `READ` is
`-EAGAIN`. A periodic timer's two blocking reads each return at least one
expiry. A timer created with `initial_ns` 0 is `-EINVAL`; a timer created
and closed without submitting cancels at `close`, the handle being its only
reference.

## Syscall fuzzer

`aio_create` and `aio_submit` are in the fuzzer's table (52 of 63 calls
exercised); `aio_wait` is excluded as a blocking call.

## The `devices` section (`init --selftest`, the device-readiness unit)

`/dev/net/tap` opened `O_RDWR|O_NONBLOCK`: `ioready` is `WRITABLE`
alone and a read returns 0; ARP requests for the pool's eight gateways
are written and the tap's own stack answers one, so `ioready` shows
`READABLE` within a bounded wait and the reply is read; a `READ`
submitted to a ring parks (`aio_wait` with `min` 0 returns nothing,
with `min` 1 and 20 ms returns 0) and completes with the reply once the
requests are written; a `POLL` on the tap parks and completes with
`READABLE`; the bench below; a child given the same open file through
the spawn map switches it blocking (`setnonblock` is per open file, so
the switch is the parent's too) and its read ends on the reply to the
parent's request; `setnonblock` on the tap is 0 and on handle 0 still
`EOPNOTSUPP`; an opened `/dev/console` reports what handle 0 reports and
switches both ways.

## Kernel

No kernel-mode self-test drives the ring: its execution paths copy to
and from the caller's user memory, so the user-mode check is the test.
The block-layer half of the milestone has its own tests
(`docs/kernel/device/testing.md`: `blk-segments`, `blk-timeout`, `nvme`).

### `io-poll` (`kernel/io/polltest.c`, milestone 10)

On a pipe: the read end is not ready and the write end is `WRITABLE`
without waiting, an ignored (NULL) entry stays 0; a 20 ms timeout with
nothing ready returns 0 after at least 15 ms; a thread that writes after
20 ms wakes a wait without timeout (`READABLE`, at least 10 ms later);
the bytes read, nothing ready again; the writer dropped: `READABLE |
HANGUP` even though only `READABLE` was asked for. About 50 ms. The Linux
`poll`/`ppoll` checks in `lxtest` cover the translation
(`docs/compat/linux/testing.md`).

### `epoll-close` (`kernel/io/epolltest.c`, the epoll-close unit)

Drives the kernel API with a handle table of its own (invariant A9). A
pipe's read end installed, registered under its handle and closed
without `EPOLL_CTL_DEL`: the end's count is back to the test's creator
reference (the registration left with the descriptor), a zero-timeout
wait reports nothing, and once the test drops that reference the writer's
write returns `-EPIPE`. An eventfd made readable, registered and closed: not reported,
released. A TCP client socket to a loopback server that accepts and
holds, registered and closed: the server's connection reaches
`CLOSE_WAIT` within two seconds (the FIN went), the socket is released,
nothing is reported. Two slots for one eventfd, registered under the
first: closing the first still reports the event, closing the second
removes it. A slot closed and then an add under it: `-EBADF`. A waiter
thread blocked in `epoll_obj_wait(FOREVER)` on a pipe end and an eventfd,
neither ready: the pipe end's slot is closed under it, its count returns
to the test's reference within a second while the waiter still sleeps
(it was woken by the removal and re-slept on the eventfd alone), and a
write to the eventfd ends the wait with that one event. A second set
holding an eventfd, the set's slot closed first (its release unlinks the
entry), then the eventfd's. Finally a registered eventfd in a table that
is destroyed. About 0.3 s; ports 6098. `tools/epoll-close-probe.py --old`
drops the call from `handle_close` and the test fails at its first
check after the baseline close: the registration still holds the end.

### `realtime` (`kernel/io/polltest.c`)

`clock_realtime_ns` is between 2020 and 2100 and advances across a 5 ms
sleep by the same amount as `clock_now_ns`. Each pair of clocks is read
by `clock_pair`, which brackets the wall-clock read with two monotonic
reads and re-reads a pair further apart than 100 µs (a tick, or the host
holding the vCPU, landed between them); the agreement is then exact to
that bracket. It was a 1 ms tolerance on pairs read back to back, which
held only while nothing interrupted the two reads. Proved: a 5 % drift
in `clock_realtime_ns` fails the agreement; a pair interrupted every time
fails the bracket (`docs/testing/flakes.md` for the rule).

## Gaps

- No multi-threaded test (two threads of one process on one ring); the
  `io_poll` wake test uses a kernel thread, the Linux `poll` test a
  clone.
- No test of a kill landing inside `aio_wait`.
- No throughput measurement; the ring is a correctness deliverable in
  this milestone.
