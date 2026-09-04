# IPC: design

## Data structures

```c
#define PIPE_SIZE 16384u     /* ring capacity */
#define PIPE_BUF  4096u      /* writes up to this size are atomic */

struct pipe {
    spinlock_t lock;
    uint8_t *buf;                     /* PIPE_SIZE, kmalloc'd */
    unsigned head, tail, used;
    unsigned readers, writers;        /* live end objects, not handles */
    struct waitqueue rd_wq, wr_wq;
    struct pipe_end rd, wr;           /* the two kobjects, embedded */
};

struct pipe_end {
    struct kobject obj;               /* type pipe_read_type or pipe_write_type */
    struct pipe *pipe;
};
```

The two ends are embedded in the pipe; the pipe itself is freed when
both ends have been released (`readers == 0 && writers == 0`). An end's
`release` decrements its counter under the lock, wakes the opposite wait
queue, and frees the pipe if it was the last end. Handles reference the
end objects, never the pipe.

## Algorithms

### pipe_create

Allocate `struct pipe` and its buffer, initialise both ends with a
reference count of 1 each (`readers = writers = 1`), return the two
kobjects. `sys_pipe` installs them with `HANDLE_RIGHT_READ` and
`HANDLE_RIGHT_WRITE` respectively; on a failed second install it closes
the first handle and the objects release normally.

### pipe_read(end, buf, len)

```text
if len == 0: return 0
wait_event_killable(&p->rd_wq, p->used > 0 || p->writers == 0)   -> -EINTR when killed
lock
n = min(len, used); copy out of the ring (two memcpy at most); head/used update
unlock
if n > 0: wake_all(wr_wq)
return n           (0 only when used == 0 && writers == 0: end of file)
```

A reader returns whatever is available (short reads are normal); it
never waits for `len` bytes.

### pipe_write(end, buf, len)

```text
if p->readers == 0: return -EPIPE
if len == 0: return 0
done = 0
while done < len:
    need = (len - done <= PIPE_BUF) ? len - done : 1        /* atomicity for small writes */
    wait_event_killable(&p->wr_wq, PIPE_SIZE - p->used >= need || p->readers == 0)
    if killed: return done ? done : -EINTR
    lock
    if p->readers == 0: unlock; return done ? done : -EPIPE
    n = min(len - done, PIPE_SIZE - used); copy in; tail/used update
    unlock
    wake_all(rd_wq)
    done += n
return done
```

A write no larger than `PIPE_BUF` waits until it fits entirely and then
copies it in one locked section, so two writers' small writes never
interleave. Larger writes proceed in pieces as space appears. All
callers (`sys_write`) already loop and copy through a bounded kernel
buffer (`IO_CHUNK` 1024), so a single call to `pipe_write` sees at most
1024 bytes and the atomicity guarantee at the system-call level is the
`IO_CHUNK` one; libc's `write` is what applications see, and it passes
the buffer straight through. **Consequence documented in api.md**:
atomic pipe writes are guaranteed for `len <= 1024` at the system-call
boundary in this phase; raising `IO_CHUNK` for pipes is the recorded
follow-up (the pipe layer itself honours `PIPE_BUF`).

### Release

```text
pipe_read_release(obj):  lock; readers--; wake_all(wr_wq); last = readers == 0 && writers == 0; unlock; if last free
pipe_write_release(obj): lock; writers--; wake_all(rd_wq); last = ...; unlock; if last free
```

Waking after the decrement lets a blocked reader see `writers == 0`
(EOF) and a blocked writer see `readers == 0` (`-EPIPE`).

### fstat

`type = COSMO_DT_FIFO`, `size = used`, mode 0600, `nlink` 1.

## Ownership and lifetime

The handle table owns references to the ends. `spawn` copies handles
(and references) into the child. `dup` adds a reference. The pipe is
owned jointly by its ends and dies with the last one. A blocked reader
or writer holds a reference to its end (the system call took it from
`handle_lookup`) so the pipe cannot vanish under a waiter.

## Concurrency

One spinlock per pipe, never held across a copy to or from user memory
(the system-call layer copies through a kernel buffer first) and never
held while blocking. Wakers use `waitqueue_wake_all` (Mesa semantics:
every waiter re-checks). Lock order: none with other subsystems (the
pipe lock is a leaf).

## Memory

`PIPE_SIZE + sizeof(struct pipe)` per pipe, ≈ 16.3 KiB, from kmalloc; a
process is limited by its 64-entry handle table, so at most 32 pipes per
process can be held open, ≈ 520 KiB. No global limit in this phase
(recorded).

## Error handling

`-ENOMEM` from `pipe_create`; `-EPIPE`, `-EINTR` as above; `-EBADF` from
the handle layer when the wrong end or missing rights are used (a read
on the write end fails in `handle_lookup` since the write end's handle
lacks READ, and the write end type also has no `read` operation).

## Performance

Two memcpy per transfer at most; wake-ups are per operation (no
batching). Adequate for shell pipelines.

## Security

Pipes are anonymous: only handle inheritance or `dup` can share one. The
buffer is bounded; a writer cannot exhaust memory beyond `PIPE_SIZE` per
pipe. Lengths come from the system-call layer already validated.

## Future extensibility

- Non-blocking ends and `poll` readiness: a `flags` word per end and
  readiness callbacks on the two wait queues.
- Named pipes: a `VNODE_FIFO` whose open returns the ends.
- Channels (messages with handles), events, shared memory and futexes
  join `kernel/ipc/` as separate files with their own kobject types; the
  object model needs nothing new for them.
