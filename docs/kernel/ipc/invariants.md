# IPC: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**I1. Every end object is one reference on the pipe's lifetime; the pipe
is freed exactly when both end counters are zero.** `readers` and
`writers` count live end *objects*, not handles: `dup` and `spawn` add
kobject references to an end, and only the end's release (last
reference) decrements the counter. `pipe_ring_free` (and the pair's
free) runs from whichever release sees both counters at zero; since the
named-pipes unit the ring is shared with FIFOs, whose counts are opens
(I9), and the rule here is the anonymous pipe's. Check: `ipc-pipe` compares
`pipe_stats.alive` before and after four pipes; `init --selftest` closes
ends in every order. Gap: no debug poisoning of freed pipes.

**I2. End of file and broken pipe follow the counters, and every counter
change wakes the other side.** A read returns 0 only when the ring is
empty and `writers == 0`; a write fails with `-EPIPE` only when
`readers == 0`; both releases wake the opposite queue after decrementing
under the lock, so a waiter re-evaluating its condition sees the new
count. Check: `ipc-pipe` (EOF after the writer thread's put, `-EPIPE`
after the read end's put); `init --selftest` (EOF after the last write
end, including a `dup` of it, is closed; `-EPIPE` after the read end is
closed); the shell's pipelines depend on it. Gap: none known.

**I3. A write of at most `PIPE_BUF` bytes handed to `pipe_write` is
never interleaved.** `pipe_write` waits for the whole remainder to fit
when it is at most `PIPE_BUF`, then copies it in one locked section.
Check: `ipc-pipe` (two writers, 200 records of 1000 bytes each, the
reader finds every record uniform). Gap: `sys_write` splits user writes
into `IO_CHUNK` (1024) pieces, so the promise a user program actually
gets is 1024 bytes; raising the chunk for pipes, or letting `sys_write`
hand an object the whole length, is the recorded follow-up.

**I4. The pipe lock is a leaf and is never held across a copy to or
from user memory or across a wait.** `pipe_read`/`pipe_write` take the
lock only around ring arithmetic and `memcpy` to kernel buffers; waits
happen before the lock; wake-ups happen after it. Check: review; the
loopback of a pipe between two kernel threads in `ipc-pipe` would
deadlock under a held lock. Gap: no lock-order checker.

**I5. Rights decide direction before the object does.** The read end
is installed with `HANDLE_RIGHT_READ` only and the write end with
`HANDLE_RIGHT_WRITE` only, so `sys_write` on `h[0]` and `sys_read` on
`h[1]` fail in `handle_lookup` (`-EBADF`); the end types also lack the
other operation, so a `dup` cannot widen a handle beyond what the object
supports. Check: `init --selftest` (`read` on the write end and `write`
on the read end are `-EBADF`). Gap: none.

**I6. A blocked reader or writer dies with its process.** Both waits are
`wait_event_killable`; `process_kill` wakes the thread and the wait
returns `-EINTR` (or the partial count). Check: `init --selftest`
spawns `cat` reading a pipe, kills it, and reaps status 137; the shell
test's `kill` of a non-existent pid covers the error path only. Gap: no
test kills a blocked *writer*.

## Gaps (documented, not invariants)

- No `splice`, message boundaries or priorities (non-blocking mode and
  readiness exist since milestone 8: `docs/kernel-services/network/invariants.md`
  N19; named pipes since the named-pipes unit, I9).
- No global limit on pipes; a process is bounded by its 64-slot handle
  table (32 pipes, about 520 KiB of rings).
- Channels, events and shared memory are not written; the futex
  (`futex.c`) exists since Phase 11 with its invariant L4 in
  `docs/compat/linux/invariants.md` (no lost wake between compare and
  sleep) and, since the shared-futex unit, I7 below (the key is what the
  word maps); native calls since the threads unit (wait, wake) and the
  native thread door (requeue). A requeue of a word onto itself moves
  nothing and counts: the move would walk the list it is on without
  bound.

**I7. A futex's identity is what the word maps, and a waiter holds the
one reference its key names.** Since the shared-futex unit
(`docs/audit/next-subsystem-shared-futex.md`) a word in a `MAP_SHARED`
file mapping is keyed by `(vnode, file offset)` and every other word by
`(space, uaddr)`; the classification runs under the space lock
(`vm_user_futex_key`) and is skipped -- the private key -- for Linux's
`FUTEX_PRIVATE_FLAG` and in a space with no shared mapping
(`shared_maps == 0`, kept by `vm_user_map_file` and the mapping record's
release, checked zero at `vm_space_destroy`). A shared key carries a
vnode reference: a waiter's from classification to dequeue, exchanged
by a requeue that changes its key (one add of as many references as
waiters moved, under the bucket locks, the old ones put after them, one
vnode each way since every waiter on a word carries that word's key); a wake's or a
requeue's own for the call. So a wake from another process finds the
sleeper, a wake on a private mapping's word does not find a shared
one's, a waiter whose mapping is unmapped or whose word is requeued
never holds a dangling vnode, and a vnode a waiter no longer keys is
released. L4 (no lost wake) is unchanged: the sequences are per bucket
and the key only chooses the bucket. **Checked by** the `mmap` section
of `init --selftest` (the two-process wait and its sleeper count across
the boundary, the wake before the sleep, private stays private, unmap
under a waiter, requeue across kinds, requeue onto a shared word then
the mapping gone, the double requeue through two files with the first
released between), `lxtest` (the flag both ways on a shared page), and
every process exit.

**I8. Every reference a unix socket, a connection or a message holds
is dropped by the release of the object that holds it, and nothing
waits on another socket while holding it alive.** A bound socket holds
its node (or, for an abstract name, its root) and its registry entry
from bind to release; a connection is held by its two ends and by
nobody else; a listener's queue holds the server-side sockets it has
not handed out; a message holds one reference per handle it carries
and is freed, references and all, when received or when its queue's
socket is released; a datagram's default destination is a pointer
cleared by the peer's release, never a reference. A connector waiting
for backlog room and a sender waiting for queue room hold no reference
to the socket they wait on: they wait for a generation change and
resolve the name again. So closing a socket's last handle always
releases it, `unix_socket_count` returns to its value after every case,
and a unix socket cannot ride in a message (the one reference cycle
this rule cannot break without a collector is refused). **Checked by**
every `unix-*` self-test's count before and after, `unix-handles` (the
reference a message holds, seen and returned), `unix-close-race` (the
connector released by the listener's close), and the userland cases.

**I9. A FIFO's ring exists exactly while an open of it does, its
reader and writer counts equal the live opens of each side, and the
pipe's end-of-file and `-EPIPE` rules follow from those counts alone.**
The first open makes the ring, the last release frees it; the open
hook adds the open's count(s) and the release hook takes them back,
both under the fifo's lock outside the ring's; and an open that fails
-- killed while it waits, refused with `-ENXIO`, out of memory -- takes
back its count and any ring it made before it returns, because the VFS
runs the release hook only for an open that succeeded. So `fifo_count`
returns to its value after every case, an unlinked FIFO's opens keep
their ring until the last of them closes, and a node is evicted with no
ring (asserted in `fifo_free`). **Checked by** `ipc-fifo`: the count
before and after every case, the killed process's open, the refused
non-blocking writer, the two-reader/two-writer sequence; and the
`fifo` section's cases through the doors.
