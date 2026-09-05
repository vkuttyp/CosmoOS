# Asynchronous I/O: architecture

Milestone 9 of the post-roadmap plan (`docs/audit/2026-09-post-roadmap-audit.md`
§19; Prompt #2 §23). Constitution sections 10 and 11 (kernel objects
behind handles), invariant 11 (explicit ownership), invariant 14
(validate untrusted input).

## Where it sits

```text
   user process    aio_create(entries) -> ring handle
                   aio_submit(ring, sqes[], n)      aio_wait(ring, cqes[], n, min, timeout)
        │
        ▼
   kernel/io/aio.c   struct aio_ring (kobject, io type "aio"): parked requests + completion queue
        │              runs an entry when its object is ready; parks it otherwise
        │              sleeps on the ring's queue and every parked object's poll_wq at once
        ▼
   kernel/object/    kobject_io_type.ready / poll_wq / read / write   (milestone 8 + 9)
   kernel/syscall/   syscall_obj_read/write (the bounce-buffer copies)
   kernel-services/vfs   file_pread/pwrite/sync
   sockets, pipes, console   their ready predicates and wait queues
```

## Purpose

Let a process issue many I/O operations without a thread per operation
and collect their results in batches, over every kind of I/O object the
kernel has, without a second I/O path: the ring drives the same `read`,
`write`, `file_pread`, `file_pwrite`, `file_sync` and `ready` operations
the synchronous system calls use.

## Responsibilities

- The ring object: creation, capacity, ownership by the creating
  process, release with parked entries dropped.
- Submission: validation of every entry (opcode, flags, buffer range,
  handle, rights), immediate execution of entries whose object is ready,
  parking of the rest, per-entry error completions.
- Waiting: running parked entries as their objects become ready,
  delivering completions in order, `min`, timeout and kill handling, the
  multi-queue wait protocol.
- Readiness of the ring itself (`COSMO_IO_READABLE` with completions
  waiting) so it composes with `ioready` and other rings.
- The per-thread non-blocking flag (`thread.io_nonblock`) that turns an
  object's wait into `-EAGAIN` while an entry executes.

## Non-responsibilities

- Shared-memory rings mapped into the process, registered buffers,
  kernel worker threads: possible later behind `aio_create`'s `flags`
  (design.md).
- Asynchronous completion of block I/O behind files: the page cache is
  synchronous; a file entry completes at submission after its disk I/O.
- `poll`/`select`/`epoll` for the Linux personality (stage 3; they can
  be built on `ready` and `poll_wq`).
- Cancellation of a parked entry other than by closing the ring.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `aio_ring_create`, `aio_submit`, `aio_wait`, `aio_ring_from_kobject` | `kernel/aio.h` | `kernel/syscall/native.c` |
| `SYS_aio_create` 60, `SYS_aio_submit` 61, `SYS_aio_wait` 62, `struct cosmo_sqe/cqe`, `COSMO_AIO_*` | `uapi/cosmo/syscall.h` | libc (`cosmo_aio_*`), programs |
| `kobject_io_type.poll_wq`, `kobject_poll_wq` | `kernel/object.h` | the ring; sockets, pipes, console implement it |
| `io_nonblocking(bool)` | `kernel/thread.h` | the wait sites of sockets, pipes, the tty |
| `syscall_obj_read/write` | `kernel/syscall.h` | the ring's READ/WRITE entries |
