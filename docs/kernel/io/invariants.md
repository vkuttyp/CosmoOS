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

**A8. `io_poll` loses no wake (milestone 10).** Every entry's `poll_wq`
is prepared (the thread marked BLOCKED) before readiness is evaluated,
the thread blocks only when nothing is ready, the timer has not fired and
no kill or signal is pending, and every entry is finished afterwards; a
wake between the evaluation and the block returns from
`sched_block_current` at once. Check: `io-poll` (a write from another
thread wakes a wait without timeout), `lxtest` (a clone's write wakes
`poll(-1)`). Gap: the two-waiter and the storm cases are not tested.

**A9. An epoll registration lives exactly as long as some descriptor to
its member.** `kobject.handles` counts the handle-table slots holding an
object across every process; when `handle_close` takes it to zero it calls
`epoll_last_handle_closed`, which removes every registration on the
object (the items leave their sets under each set's lock, the sets' queues
are woken, the references are put) before the slot's own reference goes.
`EPOLL_CTL_DEL` is never required; a `dup`'d or inherited descriptor
keeps the registration; closing one of several does not remove it. An add
whose object has no slot left is refused (`-EBADF`); a set released with
entries unlinks them from their members first. Check: `epoll-close` (a
pipe end, an eventfd and a TCP socket closed while registered: the writer
gets `-EPIPE`, the peer sees the FIN, nothing is reported, the object's
count returns to the test's own reference; a dup keeps the entry and the
last close removes it; the add after the last close is refused; a close
under a blocked `epoll_obj_wait` releases the member while the waiter
still sleeps; the set closed first, then the member), `lxtest`
(`LXEPOLLCLOSE`), `epoll_musl` (a static musl program relying on the
removal); `tools/epoll-close-probe.py --old` restores v1 and the test
fails at its baseline: the registration still holds the closed pipe end.

**A10. A wait costs the ready members, and a member's callback never
outlives its item.** Every epoll item owns a callback wait entry on each
queue its member's requested directions wake, for the item's whole life;
a wake links the item onto the set's ready list and wakes the set's own
queue, and `epoll_obj_wait` sleeps on that queue alone and walks only the
ready list -- O(ready), not O(registered). The entries leave their queues
under those queues' locks before the item is freed (DEL, release, the
last-close removal), so a callback in flight has finished and none can
start; a queue not owned by the member (a process's signalfd queue)
detaches every callback entry and waits a grace period before it is
freed, and the unhook reads the queue pointer inside a read-side section.
Check: `epoll-scale` (1, 16, 256 and 1024 members with one ready: the
1024-member non-blocking wait within 8x the 1-member one plus 20 us, the
woken blocking wait within 4x plus 200 us; the figures are printed),
`epoll-wake-race` (a writer on another CPU against ADD, wait, DEL, the
last close and re-install, two thousand rounds; then a write after DEL
puts nothing in the set), `lxtest` (`LXEPOLLET`: the edge semantics the
callback carries); `tools/epoll-callback-probe.py --old no-unhook`
leaves the entry on the queue at DEL and `epoll-wake-race` fails at that
last check. Gap: no test frees a process's signalfd queue under a
registration held by another process.

**A11. A set in a set forwards its events, and a chain of sets is bounded
and acyclic.** An inner set's wake runs the outer set's callback (the
outer's item hooks the inner's queue), taking the outer's ready-list and
queue locks with a lockdep subclass equal to the depth below it; a set
added to itself is `-EINVAL`, a set that reaches the outer or that would
make a chain of more than `EPOLL_MAX_NESTS` (4) sets is `-ELOOP`, decided
under the watch lock with no set lock held. A set's readiness as a member
is "its ready list has entries". Check: `epoll-nest` (an eventfd written
from a thread while the outer blocks: the outer returns the inner's
descriptor, the inner the eventfd's, drained neither reports; a loop
`-ELOOP`, four sets accepted, the fifth `-ELOOP`, an event at the bottom
of the chain reaching the top), `lxtest` (`LXEPOLLNEST`), `epoll_musl`
(a set in a set, `ELOOP`); `tools/epoll-callback-probe.py --old
no-loop-check` skips the check and `epoll-nest` fails at its first
`-ELOOP`. Lockdep, in every debug boot, checks the subclassed order.

## Gaps (documented, not invariants)

- No cancellation of a single parked entry; closing the ring is the only
  way out.
- No registered buffers, no shared-memory rings, no kernel worker
  threads: a file entry's disk I/O runs in the submitter.
- A ring polled by another ring becomes readable only when the owner
  collects completions into the inner ring's queue, since parked entries
  run only inside `aio_wait`.
- A chain of nested epoll sets is bounded at four (A11); Linux allows
  five. The bound is lockdep's subclass count, which sizes its order graph
  (`design.md`, "epoll").
- A signalfd's registration hears the signals of the process that made
  it: the hook is on that process's `signalfd_wqh`, while the signalfd's
  readiness is the current process's. Another process that inherits both
  the set and the signalfd and waits on the set is not woken by its own
  signals (Linux's epitem sits on the adding task's `sighand` queue the
  same way). A signalfd in a set is a one-process arrangement.
- A set's readiness as a member of a set, or to `poll()`, is its ready
  list's emptiness: a wake that turns out not to have made the member
  ready (a drain) reads as readable until the next walk drops it, so a
  `poll()` on an epoll descriptor can return readable and the following
  `epoll_wait` nothing. Linux's epoll descriptor has the same property.
- A handle riding in an unread unix message is not a descriptor for A9: an
  object whose only remaining reference is such a handle loses its epoll
  registrations when its last slot closes, where Linux keeps them until
  the message is received and that descriptor is closed too.
