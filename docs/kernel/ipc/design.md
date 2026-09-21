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

Since the named-pipes unit the ring (`struct pipe`: the lock, the
buffer, the indices, the two counts and the two queues) is split from
the ends: the anonymous pipe is a `struct pipe_pair` -- a pointer to a
ring and the two embedded `struct pipe_end` objects -- and the ring's
four operations (`pipe_ring_read/write/ready_rd/ready_wr`) take the
ring and a non-blocking flag. `readers` and `writers` count live readers
and writers, whatever they are: the ends count themselves (one each
while the end lives), a FIFO counts its opens. The pair is freed when
both counts reach zero: an end's `release` decrements its count under
the ring's lock, wakes the opposite wait queue, and frees the ring and
the pair if it was the last. Handles reference the end objects, never
the ring.

## Algorithms

### pipe_create

Allocate `struct pipe` and its buffer, initialise both ends with a
reference count of 1 each (`readers = writers = 1`), return the two
kobjects. `sys_pipe` installs them with `HANDLE_RIGHT_READ` and
`HANDLE_RIGHT_WRITE` respectively; on a failed second install it closes
the first handle and the objects release normally.

### pipe_ring_read(p, buf, len, nonblock)

`pipe_read(end)` is this with the end's non-blocking bit; a FIFO's
`read_file` is this with the open's.

```text
if len == 0: return 0
if not io_nonblocking(nonblock): wait_event_killable(&p->rd_wq, p->used > 0 || p->writers == 0)   -> -EINTR when killed
lock
n = min(len, used); copy out of the ring (two memcpy at most); head/used update
unlock
if n > 0: wake_all(wr_wq)
return n           (0 only when used == 0 && writers == 0: end of file)
```

A reader returns whatever is available (short reads are normal); it
never waits for `len` bytes.

### pipe_ring_write(p, buf, len, nonblock)

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
end_release(obj, reader): lock ring; reader ? readers-- : writers--; last = readers == 0 && writers == 0; unlock
                          wake_all(reader ? wr_wq : rd_wq); if last: pipe_ring_free(ring); kfree(pair)
```

Waking after the decrement lets a blocked reader see `writers == 0`
(EOF) and a blocked writer see `readers == 0` (`-EPIPE`).

### fstat

`type = COSMO_DT_FIFO`, `size = used`, mode 0600, `nlink` 1.

## Named pipes (kernel/ipc/fifo.c)

A named pipe is the same ring behind a filesystem node: a `VNODE_FIFO`
made by `vfs_mknod` (the `mknod` vnode operation the unix-sockets unit
added; ramfs implements it, cosmofs and procfs have none and refuse
`-EOPNOTSUPP`), owned by the caller, mode as given. The node's ramfs
record holds a `struct fifo` -- a spinlock, a ring pointer (NULL while
nobody has the FIFO open) and an openers' wait queue -- allocated at
`mknod` and freed at evict, which asserts the ring is gone. The ring is
made by the first open and freed by the last release: a FIFO's data
does not outlive its openers. Its `readers` and `writers` are the live
**opens** of each side, counted in the open hook and taken back in the
release hook, and the ring's own rules then give end of file when the
last writer has closed and the ring is empty (bytes written before the
close are read first) and `-EPIPE` when no reader remains. The ring
pointer and the counts change together, under one lock order: the
fifo's lock outside the ring's.

**Open** is POSIX's: `O_RDONLY` waits until a writer has the FIFO open,
`O_WRONLY` until a reader has (each open wakes the other side's openers
on the fifo's queue, and the wait is on that queue with no lock held);
with `O_NONBLOCK` a read-only open returns at once and a write-only one
is `-ENXIO` when no reader is there; `O_RDWR` counts as both sides and
never blocks. The wait is killable. **An open that fails undoes
itself**: the VFS runs the release hook only for an open that
succeeded, so the open hook takes back the count it added -- killed
(`-EINTR`), refused (`-ENXIO`), out of memory -- and frees the ring if
it made it and is the last, waking the other side's openers whose
condition it changed. Per-open state is a `struct fifo_open { ring,
side, nonblock }` in `file->priv`: `read_file`/`write_file` call the
ring with the open's flag, so a FIFO's non-blocking mode is **per
open**, as POSIX has it (the anonymous pipe's shared-per-end bit stays
as documented). `unlink` while open removes the name and nothing else:
the opens keep their ring, a new open finds `-ENOENT`, the last
release frees the ring and the node goes with it.

**Files learned readiness for it.** `struct vnode_ops` has three
optional operations, `ready(vn, f)`, `poll_wq(vn, f, events)` and
`set_nonblock(vn, f, on)`, and the file kobject type delegates to them
when present and answers as before when not (always readable and
writable; NULL, readiness never changes; `-EOPNOTSUPP`). The FIFO
implements all three over the ring's readiness, its two queues and the
open's bit; `chrdev_ops` gained the same three, so a device whose
`read_file` blocks can say so -- no existing device does yet. `open`
takes `COSMO_O_NONBLOCK` (`0x0800`, Linux's value) and keeps it in
`file->flags`, where the FIFO's open reads it.

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

## Non-blocking ends and readiness (milestone 8)

Each end carries a `nonblock` bit (`set_nonblock` through the object
type, `setnonblock`/`fcntl(F_SETFL)` from user mode; shared by every
handle to that end). A non-blocking read of an empty pipe with a writer
alive is `-EAGAIN` (still 0 at end of file); a non-blocking write is
`-EAGAIN` when the ring cannot take the piece (whole for `PIPE_BUF` or
less, one byte otherwise) and nothing was written yet, else the partial
count. `ready` on the read end reports `READABLE` with bytes in the
ring and `READABLE|HANGUP` once no writer remains; on the write end
`WRITABLE` with at least `PIPE_BUF` free and `WRITABLE|ERROR` once no
reader remains (a write fails at once). Both read the pipe under its
lock and never block.

## Unix domain sockets (`kernel/ipc/unix.c`; the unix-sockets unit)

`docs/audit/next-subsystem-unix-sockets.md` is the report. The family is
`COSMO_AF_UNIX`, a third one in `struct socket` (`kernel/socket.h`):
`ksock_create` allocates a `struct unix_sock` for it and every address-
free `ksock_*` entry point (`listen`, `accept`, a connected send or
receive, `shutdown`, `ready`) dispatches on the family, while the doors
call the `unix_*` entry points of `kernel/unix.h` for a unix address,
which an inet `struct netaddr` cannot hold. Nothing above the socket
layer learns a new kind of object: the same kobject type, rights bits,
`fstat` (`DT_SOCK`) and readiness table.

**A send is a message.** `struct umsg`: its bytes, the sender's name (a
datagram), and the handles that rode with it -- referenced kobjects and
the rights to install them with, owned by the message while it is
queued. A `struct uqueue` is a bounded list of them (bytes and count)
with two flags, `wr_closed` (nothing more arrives) and `rd_closed`
(nobody reads).

**A stream connection** is one `struct unix_conn`: a spinlock, two
queues (`q[i]` read by side `i`, written by the other, `UNIX_BUF` =
64 KiB each), the two sockets' wait queues, and a count of ends. The
pipe's rules carry over: the lock is never held while blocking or
touching user memory, waits are killable, a read on an empty queue
whose writer is gone returns 0, a write whose reader is gone returns
`-EPIPE` and no signal. A stream send queues one message of at most
`UNIX_MSG_MAX` bytes and may be partial (the door's loop brings the
rest); a stream read consumes bytes across messages but **stops at the
boundary of a message that carries handles** once it has copied any
byte, so ancillary items stay with the bytes they were sent with and
two sends' handles are never delivered in one read. The conn is freed
when both ends have let go (`ends == 0`), as the pipe is.

**A listener** (`listen` on a bound stream socket) keeps an accept
queue bounded by the backlog (at most `UNIX_BACKLOG_MAX`, 128) and the
credentials of the process that called `listen`. `connect` makes the
server-side socket **then and there**, links the two through a fresh
conn, queues the server side (the creation reference is the queue's)
and returns 0: the client may write before anyone accepts and the bytes
wait. Each side is told the other's name and credentials
(`SO_PEERCRED`). `accept` dequeues; releasing a listener releases what
it never accepted, so those clients read end-of-stream and write
`-EPIPE`.

**A datagram socket** owns one queue (`UNIX_DGRAM_MAX` = 64 messages,
`UNIX_DGRAM_BYTES` = 256 KiB; a message at most `UNIX_MSG_MAX`, else
`-EMSGSIZE`); `sendto` a name or the default destination `connect`
recorded; a receive into a short buffer keeps the prefix and sets
`COSMO_MSG_TRUNC`. The default destination is a **pointer, not a
reference**, kept under the registry lock and cleared by the peer's
release: two datagram sockets connected to each other (a socketpair)
would otherwise hold each other alive with nothing outside to release
either. A sender takes a reference with `kobject_tryget` for the one
send.

**Names.** A bound socket holds one vnode reference -- its node for a
path, the caller's root (`vfs_current_root`) for an abstract name -- and
one entry in the registry (`g_reg`, one spinlock), keyed by that vnode
and, for an abstract name, the bytes; both for exactly as long as the
socket lives, and the entry goes first in the release so nothing finds
a dying socket. `bind` to a path makes a `VNODE_SOCK` node through the
new `vfs_mknod` from the caller's cwd and root, mode 0755 (this system
has no umask; `chmod` widens it), owned by the caller; a name that
exists is `-EADDRINUSE`; a filesystem without `mknod` (cosmofs, procfs)
refuses. `connect` and `sendto` resolve a path with `vfs_lookup` from
the caller's cwd and root -- a jail names nothing outside itself and a
link to a socket works -- require `VNODE_SOCK` (`-ECONNREFUSED`
otherwise, as Linux), write permission on the node (`vfs_permission`),
and a socket behind it (`-ECONNREFUSED` when the socket that made the
node has gone); a path that does not resolve is the lookup's error.
`open()` of a socket node is `-ENXIO`. `unlink` removes the name
through the ordinary path; connections already made are untouched. An
abstract name (a leading NUL) is keyed by the caller's **root**, so a
jailed process sees only the abstract sockets bound under the same
root -- one comparison more than Linux makes, and the one the security
model requires (`docs/kernel/security/design.md`, "Per-process roots").

**A handle in a message** is spawn's rule called again:
`handle_transfer_check` (`kernel/object/handle.c`) -- TRANSFER held, the
rights named `COSMO_RIGHTS_SAME` or a subset -- and the message owns the
references while in flight. At `recvmsg` they are installed in message
order until the first refusal (`-EMFILE`, or the end of the room the
caller gave); those installed stay, the rest are released with
`COSMO_MSG_HTRUNC` (Linux's `MSG_CTRUNC`), no reservation and no
rollback. A receiver that asks for no room gets the bytes and the flag.
**A unix socket does not ride in a message** (`-EINVAL`): two sockets
each queued in the other would hold each other alive with nothing
outside to release either, the cycle Linux collects with a garbage
collector; refused rather than leaked, and the report names the
collector as the unit that would lift it.

**Waiting on another socket.** A connector blocked on a full backlog
and a datagram sender blocked on a full queue hold **no reference** to
the socket they wait on: holding one would keep it alive past its last
handle, and the wait would depend on a release the wait itself
prevents. Both sleep on one global queue (`g_room_wq`) for a change of
generation -- bumped by `accept`, `listen`, a datagram receive and every
unix socket's release -- and resolve the name again when they wake, so
a released listener is simply not found and the connect is refused.
Readers and writers of a stream wait on their own socket's queue; the
peer's release reaches them through the conn.

**Locking.** The socket's mutex for state changes (bind, listen,
connect, accept; a connector takes the listener's mutex nested under
its own, and a listener connects to nobody, so the order is one way),
then the registry spinlock, then a conn's or a datagram queue's
spinlock; never the reverse. A registry lookup takes a reference and
drops the registry lock before touching that socket's mutex. Handles
are installed in the receiver's table after the message has left its
queue and the queue lock has been dropped.

## Future extensibility

- `poll` over the readiness operation once a wait primitive exists.
- The existing devices (`/dev/tty`, `/dev/net/tap`, `/dev/vmm`) can
  now report readiness through `chrdev_ops` and do not yet; each is its
  own small unit (the tap's is the useful one: `select` over the tap
  and a socket).
- Messages with handles exist as unix sockets; events, shared memory
  join `kernel/ipc/` as separate files with their own kobject types; the
  object model needs nothing new for them.
