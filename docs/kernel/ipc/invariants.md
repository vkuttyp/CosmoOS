# IPC: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**I1. Every end object is one reference on the pipe's lifetime; the pipe
is freed exactly when both end counters are zero.** `readers` and
`writers` count live end *objects*, not handles: `dup` and `spawn` add
kobject references to an end, and only the end's release (last
reference) decrements the counter. `pipe_free` runs from whichever
release sees both counters at zero. Check: `ipc-pipe` compares
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

- No named pipes, non-blocking mode, `poll`, `splice`, message
  boundaries or priorities.
- No global limit on pipes; a process is bounded by its 64-slot handle
  table (32 pipes, about 520 KiB of rings).
- Channels, events and shared memory are not written; the futex
  (`futex.c`) exists since Phase 11 with its invariant L4 in
  `docs/compat/linux/invariants.md` (no lost wake between compare and
  sleep) and no native system call yet.
