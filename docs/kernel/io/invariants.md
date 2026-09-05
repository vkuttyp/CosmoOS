# Asynchronous I/O: invariants

**A1. User memory is touched only by the submitting process's own
threads, inside `aio_submit` and `aio_wait`.** No kernel thread executes
an entry; a parked entry runs when the owner calls `aio_wait`. Another
process's `aio_submit`/`aio_wait` on the ring is `-EPERM`. Check:
review (`run` is called from the two entry points only); `init --selftest`
(a ring handle is only useful to its creator: a parked read completes on
the owner's next wait).

**A2. An entry never waits for readiness; it runs when ready or parks.**
Every executable entry runs with `thread.io_nonblock` set, so the socket,
pipe and tty wait sites return `-EAGAIN`, which parks the entry again
(or completes it with `NOWAIT`). Files may wait on disk I/O, never on a
reader or writer. Check: `init --selftest` (a read of an empty pipe
parks, a `NOWAIT` read completes `-EAGAIN`, a write to a pipe with room
completes at submission); review of the wait sites (`io_nonblocking`).

**A3. The ring holds at most `entries` requests and completions
together.** `aio_submit` stops accepting at the bound (`-EBUSY` when it
took none); `cq_push` asserts the bound. Check: `init --selftest` (nine
parked reads on an eight-entry ring: eight accepted, the ninth
`-EBUSY`).

**A4. Every accepted entry completes exactly once, or is dropped with
the ring.** A per-entry failure is a completion with the error; a
parked entry completes on a later wait; the release drops parked entries
(object references put) without running them. Check: `init --selftest`
(each `user_data` seen once; the ring is closed with parked reads and
nothing leaks: the pipe ends close cleanly afterwards). Gap: no count of
live requests is compared across the test.

**A5. A parked entry holds a reference to its object for its life.**
The handle may be closed while the entry is parked; the object survives
until the entry completes or the ring is released. Check: review
(`handle_lookup` at submission, `kobject_put` in `req_free`). Gap: no
test closes a handle with a parked entry on it.

**A6. `aio_wait` has no lost wake-up.** Every parked entry's `poll_wq`
and the ring's own queue are prepared (the thread marked BLOCKED) before
the conditions are evaluated under the ring mutex, and the thread blocks
only when none holds; a wake between the evaluation and the block makes
`sched_block_current` return at once (`docs/kernel/scheduler/design.md`,
the `wait_event` protocol). A concurrent `aio_submit` that parks an entry
wakes the ring's queue, so a sleeping waiter re-arms with the new
entry's queue included. Check: `init --selftest` (a 20 ms wait times
out; the next wait returns the completion the write made runnable). Gap:
the two-thread case is not tested.

**A7. Handle rights apply per entry as for the equivalent system call.**
`READ`/`PREAD`/`POLL` need READ, `WRITE`/`PWRITE`/`FSYNC` need WRITE; a
missing right is `-EBADF` in the completion. Check: `init --selftest`
(handle 999 completes `-EBADF`). Gap: no test of a right actually
dropped from a valid handle.

## Gaps (documented, not invariants)

- No cancellation of a single parked entry; closing the ring is the only
  way out.
- No registered buffers, no shared-memory rings, no kernel worker
  threads: a file entry's disk I/O runs in the submitter.
- A ring polled by another ring becomes readable only when the owner
  collects completions into the inner ring's queue, since parked entries
  run only inside `aio_wait`.
