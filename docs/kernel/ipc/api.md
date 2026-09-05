# IPC: API

Every entry follows constitution section 52. The kernel interface is
internal (**ABI stability: internal**); the user ABI is the `pipe`
system call plus `read`, `write`, `close`, `dup` and `fstat` on its
handles, specified in `docs/kernel/syscall/api.md`.

## Shared contracts

- One spinlock per pipe (a leaf: no other lock is taken under it), never
  held while blocking or while copying user memory (the system-call
  layer copies through a kernel bounce buffer first).
- Waits are `wait_event_killable`: a blocked reader or writer whose
  process is killed returns `-EINTR`.
- The two ends are kobjects; handles reference the ends, never the pipe.
  The pipe is freed when both end counters reach zero.

## kernel/pipe.h

### Constants

| Name | Value | Meaning |
|---|---|---|
| `PIPE_SIZE` | 16384 | ring capacity in bytes |
| `PIPE_BUF` | 4096 | a `pipe_write` of at most this many bytes is never interleaved with another writer's |

### `struct pipe_stats { uint64_t created, alive, bytes; }`
Pipes created since boot, pipes not yet freed, bytes moved through
`pipe_read`. `void pipe_get_stats(struct pipe_stats *out)` snapshots
them under the statistics lock.

### `int pipe_create(struct kobject **read_end, struct kobject **write_end)`
- Purpose: allocate a pipe (`struct pipe` plus a `PIPE_SIZE` buffer from
  kmalloc) with its two end objects.
- Outputs: 0 and two referenced kobjects (one reference each, owned by
  the caller: `handle_install` takes its own and the caller puts these);
  `-ENOMEM`.
- Types: the read end is `"pipe-read"` (`read`, `stat`, no `write`); the
  write end is `"pipe-write"` (`write`, `stat`, no `read`). Both embed
  `struct kobject_io_type`.
- Concurrency: thread context (allocates).

### End operations (through `struct kobject_io_type`)

**`read(end, buf, len)`**: `len == 0` returns 0. Otherwise waits until
`used > 0` or `writers == 0`, copies `min(len, used)` bytes out of the
ring (at most two `memcpy`), wakes the writers' queue when it took
anything, and returns the count. 0 means end of file: the ring is empty
and no write end exists. `-EINTR` when killed while waiting.

**`write(end, buf, len)`**: `-EPIPE` at once when `readers == 0`;
`len == 0` returns 0. Otherwise, in pieces: a remaining length of at
most `PIPE_BUF` waits for that much free space and lands whole, a larger
remainder waits for one byte and takes what fits. Each piece wakes the
readers' queue. Returns the bytes written; when the readers vanish or a
kill lands after some bytes were written the partial count is returned,
otherwise `-EPIPE` or `-EINTR`. Note: `sys_write` hands the pipe at most
`IO_CHUNK` (1024) bytes per call, so at the system-call boundary the
atomicity guarantee is 1024 bytes; the `PIPE_BUF` promise holds for
kernel callers (recorded gap, `invariants.md` I3).

**`stat(end, st)`**: `type = COSMO_DT_FIFO`, `mode = 0600`, `nlink = 1`,
`size = used` (bytes in the ring), everything else 0.

**`ready(end)`**: read end `COSMO_IO_READABLE` with `used > 0`,
`READABLE|HANGUP` with `writers == 0`; write end `COSMO_IO_WRITABLE`
with `PIPE_SIZE - used >= PIPE_BUF`, `WRITABLE|ERROR` with `readers ==
0`. **`set_nonblock(end, on)`**: sets the end's mode (0/1; -1 asks) and
returns the previous one. In non-blocking mode `read` is `-EAGAIN`
instead of waiting (still 0 at end of file) and `write` is `-EAGAIN`
when the ring cannot take the current piece and nothing was written
yet (else the partial count); the same holds while the calling thread
executes an I/O ring entry (`io_nonblocking`, `docs/kernel/io/api.md`).
**`poll_wq(end, events)`**: the read end's `rd_wq`, the write end's
`wr_wq`, whatever `events` asks.

**Release of the read end**: `readers--`, wake the writers (they see
`-EPIPE`), free the pipe when both counts are 0. **Release of the write
end**: `writers--`, wake the readers (they see end of file), free
likewise. Releases run from the last `kobject_put` of the end, which
`handle_close` and `handle_table_destroy` perform outside their locks
(they may not block here, but the contract allows it).

## Futex (`kernel/include/kernel/futex.h`, Phase 11)

`int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val,
uint64_t timeout_ns)` blocks while the user word equals `val` (0 woken,
`-EAGAIN` differs, `-ETIMEDOUT`, `-EINTR` killed, `-EFAULT`, `-EINVAL`
misaligned); `int futex_wake(struct vm_space *space, uint64_t uaddr,
unsigned n)` wakes up to `n` waiters and returns the count. The compare
and the enqueue happen under one bucket lock (64 buckets). No native
system call exposes it yet; the Linux `futex` call does. Full contract:
`docs/compat/linux/api.md`.

## System calls (`kernel/syscall/native.c`)

**`pipe(int h[2])`** (35): `-EFAULT` unless `h` names 8 writable user
bytes; `pipe_create`; installs the read end with `HANDLE_RIGHT_READ`
and the write end with `HANDLE_RIGHT_WRITE` in the lowest free slots
(`-EMFILE` when either install fails, nothing installed); copies the two
numbers out (`-EFAULT` closes both). `h[0]` reads, `h[1]` writes.

`read`/`write`/`close`/`dup`/`fstat` need no pipe knowledge: the handle
rights refuse a read on the write end (`-EBADF`) before the object's
missing `read` operation would.

## Failure modes

| Condition | Behaviour |
|---|---|
| write with no read end | `-EPIPE` (no signal; the writer sees the error) |
| read with no write end and an empty ring | 0 (end of file) |
| reader or writer killed while blocked | `-EINTR`, or the partial count for a write that had progressed |
| out of memory | `pipe_create` `-ENOMEM`; `sys_pipe` returns it |
| handle table full | `-EMFILE`, both ends released |
