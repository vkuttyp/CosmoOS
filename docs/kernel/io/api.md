# Asynchronous I/O: API

## Kernel (`kernel/include/kernel/aio.h`, `kernel/io/aio.c`)

### `struct aio_ring`
`obj` (io type `aio`: `ready` and `poll_wq` only, no `read`/`write`,
`fstat` is `-EBADF`), `lock` (a mutex over every field, never held
while sleeping for readiness), `entries`, `parked` (`struct aio_req`,
oldest first) and `nr_parked`, `cq`/`cq_head`/`cq_len` (a ring of
`entries` completions), `wait` (woken on every completion), `owner`
(the creating pid), counters `submitted`, `completed`, `parked_total`,
`executed_at_submit`.

### `int aio_ring_create(unsigned entries, unsigned flags, struct aio_ring **out)`
A ring with one reference for the caller. `-EINVAL` (`entries` 0 or
above `COSMO_AIO_MAX_ENTRIES` 1024, `flags` non-zero), `-ENOMEM`.
Thread context of the owning process (`owner` is `process_current()`).

### `struct aio_ring *aio_ring_from_kobject(struct kobject *obj)`
The ring, or NULL for another type.

### `int64_t aio_submit(struct aio_ring *r, uint64_t usqes, unsigned n)`
Copies `n` entries from user memory and runs or parks each (design.md,
"Execution and parking"). Returns the number accepted, which is less
than `n` when the ring is full (parked plus uncollected completions equal
`entries`) or memory ran out; `-EBUSY` when it could take none;
`-EFAULT` when the first entry cannot be read (a later unreadable entry
ends the batch at the accepted count); `-EPERM` from another process.
Per-entry failures are completions: `-EINVAL` (opcode, flags, a `POLL`
with no events),
`-EFAULT` (buffer range), `-EBADF` (handle, right, or an object without
the operation), `-ESPIPE` (`PREAD`/`PWRITE`/`FSYNC` on a non-file),
`-EAGAIN` (`COSMO_AIO_F_NOWAIT` and not ready). Thread context; may
block on the objects' own locks and on disk I/O for files, never on
readiness.

### `int64_t aio_wait(struct aio_ring *r, uint64_t ucqes, unsigned n, unsigned min, uint64_t timeout_ns)`
Runs every parked entry whose object is ready, then returns up to `n`
completions (oldest first) once at least `min` are available; `min == 0`
polls; `timeout_ns` bounds the wait (0 polls too;
`COSMO_AIO_WAIT_FOREVER` waits without bound). Returns the number copied
(0 after a timeout or a poll with nothing), `-EINTR` when the process is
killed with nothing to deliver, `-EINVAL` (`min > n`, `n > 1024`),
`-EFAULT`, `-EPERM`. Sleeps on the ring's queue and on every parked
entry's `poll_wq`; a one-shot timer wakes it at the deadline. Thread
context of the owning process.

### The entry operations (`struct cosmo_sqe.op`)

| op | handle right | runnable when | result |
|---|---|---|---|
| `COSMO_AIO_NOP` 0 | none | always | 0 |
| `COSMO_AIO_READ` 1 | READ | `ready` has READABLE, HANGUP or ERROR, or the object has no `ready` | bytes, 0 at end of file |
| `COSMO_AIO_WRITE` 2 | WRITE | `ready` has WRITABLE, HANGUP or ERROR, or no `ready` | bytes |
| `COSMO_AIO_PREAD` 3 | READ | a file: always | bytes at `offset` |
| `COSMO_AIO_PWRITE` 4 | WRITE | a file: always | bytes at `offset` |
| `COSMO_AIO_FSYNC` 5 | WRITE | always | 0 |
| `COSMO_AIO_POLL` 6 | READ | any bit of `events` set | the set bits |

`COSMO_AIO_F_NOWAIT` completes a not-runnable entry with `-EAGAIN`
instead of parking it. An entry that becomes runnable and then finds the
object no longer ready (its operation returns `-EAGAIN` under the
thread's non-blocking flag) is parked again, unless `NOWAIT`.

## System calls (`kernel/include/uapi/cosmo/syscall.h`)

| Nr | Name | Arguments | Result | Errors |
|---|---|---|---|---|
| 60 | `aio_create` | `unsigned entries, unsigned flags` | a ring handle (READ and WRITE) | `EINVAL`, `ENOMEM`, `EMFILE` |
| 61 | `aio_submit` | `int ring, const struct cosmo_sqe *sqes, unsigned n` | entries accepted | `EBADF` (not a ring, or lacking a right), `EPERM`, `EFAULT`, `EBUSY` |
| 62 | `aio_wait` | `int ring, struct cosmo_cqe *cqes, unsigned n, unsigned min, uint64_t timeout_ns` | completions copied | `EBADF`, `EPERM`, `EINVAL`, `EFAULT`, `EINTR` |

`struct cosmo_sqe` (40 bytes: `op`, `flags`, `events`, `handle`,
`addr`, `len`, `offset`, `user_data`) and `struct cosmo_cqe` (16 bytes:
`user_data`, `result`) are stable UAPI, as are the opcode and flag
values above and `COSMO_AIO_MAX_ENTRIES`, `COSMO_AIO_WAIT_FOREVER`. The
ring handle also answers `ioready` (58: `COSMO_IO_READABLE` with
completions waiting) and `close`. libc: `cosmo_aio_create(entries,
flags)`, `cosmo_aio_submit(ring, sqes, n)`, `cosmo_aio_wait(ring, cqes,
n, min, timeout_ns)` (`libc/include/cosmo/syscall.h`).

## The object side

### `struct waitqueue *(*poll_wq)(struct kobject *obj, unsigned events)` (`kernel/include/kernel/object.h`)
The queue a waiter for `events` sleeps on; it must be woken whenever
`ready` may have changed for those bits. NULL means readiness never
changes (files). Implementations: sockets (`socket.wait`), pipe ends
(`rd_wq`/`wr_wq`), the console (`tty.readers` for `READABLE`, NULL for
`WRITABLE`), the ring itself (`aio_ring.wait`). `kobject_poll_wq(obj,
events)` applies the defaults (NULL for a plain object or a type without
the operation).

## Polling (`kernel/include/kernel/poll.h`, `kernel/io/poll.c`, milestone 10)

### `int io_poll(struct io_pollfd *fds, unsigned n, uint64_t timeout_ns)`
- `struct io_pollfd { struct kobject *obj; unsigned events; unsigned
  revents; }`: `obj` NULL means the entry is ignored; `events` are
  `COSMO_IO_*` bits of interest; `revents` receives `ready & events`
  plus `HANGUP`/`ERROR` whenever set.
- Returns the number of entries with any bit set; when none: sleeps on
  every object's `poll_wq` (armed with `waitqueue_prepare` before
  readiness is re-read, the AIO ring's protocol) until one may have
  changed, `timeout_ns` passes (0: return at once; `IO_POLL_FOREVER`: no
  timeout) or the process is killed or has a signal to take
  (`-EINTR`). Objects without a `poll_wq` are re-checked only when
  another entry wakes the caller or the timeout ends. `-ENOMEM` for the
  wait entries. Thread context; may block. The caller holds every
  object reference.
- Users: Linux `poll`/`ppoll` (`compat/linux/syscalls.c`); a native
  `poll` can be added the same way.

### `bool io_nonblocking(bool object_nonblock)` (`kernel/include/kernel/thread.h`)
True when the object is non-blocking or the current thread is executing
a ring entry (`thread.io_nonblock`). Every wait site that honours a
socket's or pipe end's `nonblock` bit goes through it; `tty_read`
returns `-EAGAIN` under it when no line waits.

### `int64_t syscall_obj_read(struct kobject *obj, uint64_t ubuf, size_t len)`, `syscall_obj_write(...)` (`kernel/include/kernel/syscall.h`)
The bodies of `read`/`write` on an object the caller already holds: the
object's operation through the 1024-byte bounce buffer, `-EBADF` for an
object without it. `syscall_handle_read/write` look the handle up and
call them.
