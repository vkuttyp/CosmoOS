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

## Syscall fuzzer

`aio_create` and `aio_submit` are in the fuzzer's table (52 of 63 calls
exercised); `aio_wait` is excluded as a blocking call.

## Kernel

No kernel-mode self-test drives the ring: its execution paths copy to
and from the caller's user memory, so the user-mode check is the test.
The block-layer half of the milestone has its own tests
(`docs/kernel/device/testing.md`: `blk-segments`, `blk-timeout`, `nvme`).

## Gaps

- No multi-threaded test (two threads of one process on one ring).
- No test of a kill landing inside `aio_wait`.
- No throughput measurement; the ring is a correctness deliverable in
  this milestone.
