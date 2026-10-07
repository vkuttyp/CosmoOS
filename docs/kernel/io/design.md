# Asynchronous I/O: design

Milestone 9 of `docs/audit/2026-09-post-roadmap-audit.md` §19 (Prompt #2
§23, "a unified asynchronous I/O model": submission, completion, buffer
ownership, registered resources, polling, event notification, for files,
sockets, devices, timers, IPC and VM operations). This document is the
decision record; `architecture.md`, `api.md`, `invariants.md` and
`testing.md` describe what was built from it.

## The model in one paragraph

A process creates an *I/O ring*: a kernel object (a handle like any
other) that holds a submission side and a completion queue. It submits
*entries*, each naming an operation, a handle, a buffer and a
`user_data` cookie, and later collects *completions*, each carrying the
cookie and a result. The kernel executes an entry as soon as its object
would not block, and otherwise parks it until the object's readiness
changes; the readiness operation that milestone 8 put on every I/O
object (`kobject_io_type.ready`, `docs/kernel/object/api.md`) is what
decides "would not block", and the wait queues behind those objects are
what wakes a parked entry. Completions are collected with or without
waiting, with a timeout, and the ring itself reports `COSMO_IO_READABLE`
when completions are available, so a ring can be watched through
`ioready` or from another ring. Nothing is copied to or from user memory
except in the submitting process's own threads, inside `aio_submit` and
`aio_wait`.

## Why not io_uring, kqueue, epoll or POSIX AIO

- **io_uring** shares two rings in memory mapped into the process and
  runs blocking work on kernel worker threads. CosmoOS has no user
  mapping of kernel pages yet and its user-memory access is tied to the
  current thread's address space (`copy_from_user` needs the process's
  page tables live on the CPU); a worker thread would need to borrow
  the address space, a mechanism the kernel does not have and this
  milestone does not want to add for its sake. The copy-based ring
  below costs one system call per batch of submissions and one per batch
  of completions, which is the part of io_uring's win that matters at
  CosmoOS's scale; the shared-memory rings are an optimisation the
  interface leaves room for (a `flags` word on `aio_create`).
- **kqueue/epoll** deliver readiness, not completion; the application
  still performs the I/O. Readiness alone is already available through
  `ioready`; the ring adds the completion step on top of the same
  predicate, which is what an event loop wants to be rid of.
- **POSIX AIO** is per-request with signals or threads for completion;
  the ring batches and has no signals.

The result is native: one object, three calls, and every other I/O
object joins by implementing `ready` and `poll_wq`, which the socket,
pipe, console and file types already do or trivially can.

## Data structures

```c
struct cosmo_sqe {            /* one submission, 40 bytes */
    uint8_t  op;              /* COSMO_AIO_NOP, READ, WRITE, PREAD, PWRITE, FSYNC, POLL */
    uint8_t  flags;           /* COSMO_AIO_F_NOWAIT: complete with -EAGAIN instead of parking */
    uint16_t events;          /* POLL: the COSMO_IO_* bits of interest */
    int32_t  handle;
    uint64_t addr;            /* user buffer */
    uint64_t len;
    uint64_t offset;          /* PREAD/PWRITE: file offset */
    uint64_t user_data;       /* returned in the completion */
};
struct cosmo_cqe {            /* one completion, 16 bytes */
    uint64_t user_data;
    int64_t  result;          /* bytes, the ready mask, 0, or -errno */
};

struct aio_req {              /* kernel side of one entry */
    struct cosmo_sqe sqe;
    struct kobject *obj;      /* referenced for the entry's life */
    struct wait_entry wait;   /* on the object's poll_wq while parked */
    struct waitqueue *wq;     /* what the object gave us, or NULL */
    unsigned want;            /* COSMO_IO_READABLE or WRITABLE: what "ready" means for this op */
    struct list_node link;    /* ring->parked */
};
struct aio_ring {
    struct kobject obj;       /* io type "aio": ready = completions or a runnable entry; no read/write */
    struct mutex lock;        /* serialises submit and wait on this ring */
    unsigned entries;         /* capacity: parked + completed <= entries */
    struct list_node parked;  /* requests waiting for readiness */
    struct cosmo_cqe *cq;     /* completed, in order, ring buffer of `entries` */
    unsigned cq_head, cq_len;
    unsigned nr_parked;
    struct waitqueue wait;    /* aio_wait sleeps here; also woken by the objects' queues */
    struct process *owner;    /* the creating process; submit/wait from another process is -EPERM */
    uint64_t submitted, completed, parked_total, executed_at_submit;
};
```

The submission side has no queue of its own: `aio_submit` consumes its
entries at once, executing or parking each. The capacity bounds what
the ring holds for the process (parked requests and uncollected
completions together), so a ring of 64 entries costs at most 64 requests
plus 64 completions of kernel memory whatever the process does.

## Operations

| op | object | what runs | result |
|---|---|---|---|
| `NOP` | none | nothing | 0 |
| `READ` | any readable object | the object's `read`, at its position | bytes, 0 at end of file |
| `WRITE` | any writable object | the object's `write` | bytes |
| `PREAD` / `PWRITE` | a file | `file_pread` / `file_pwrite` at `offset` | bytes |
| `FSYNC` | a file | `file_sync` | 0 |
| `POLL` | any object | nothing | the `COSMO_IO_*` bits currently set among `events` |

Handle rights are checked as the equivalent system call would
(`READ`/`PREAD`/`POLL` need READ, `WRITE`/`PWRITE`/`FSYNC` need WRITE);
a wrong handle, right or object type completes the entry with `-EBADF`
rather than failing the submission, so a batch is never half accepted
for a reason the caller can only learn per entry. The user buffer is
range-checked at submission (`-EFAULT` completion otherwise) and copied
only at execution, through the same bounce buffers as `read` and
`write`.

## Execution and parking

At submission an entry is *runnable* when its object has no `ready`
operation (a file: always) or `ready` reports the bit the operation
needs (`READABLE` for reads and `POLL` with a readable interest,
`WRITABLE` for writes; `POLL` is runnable when any bit of `events` is
set). A runnable entry executes at once in the submitting thread with
the thread's *non-blocking I/O* flag raised (`thread.io_nonblock`, read
by the socket, pipe and tty wait sites next to the object's own
`nonblock` bit), so a readiness that vanished between the check and the
call yields `-EAGAIN`, which parks the entry instead of completing it.
An entry that is not runnable is parked with `COSMO_AIO_F_NOWAIT`
clear, or completed with `-EAGAIN` when the flag is set.

A parked entry needs a wake-up: the io type gains
`struct waitqueue *(*poll_wq)(struct kobject *obj, unsigned events)`,
the queue a waiter for `events` on this object should sleep on (the
socket's `wait`, a pipe end's `rd_wq` or `wr_wq`, the console tty's
`readers`). `aio_wait` sleeps on the ring's own queue *and* on every
parked entry's `poll_wq` at once (one `wait_entry` per parked request,
all prepared before the blocking decision, all finished after; the same
lost-wakeup-free protocol as `wait_event`, extended to many queues).
Whatever wakes it, `aio_wait` re-runs every parked entry whose object is
now ready, in parking order, moves the completions to the completion
queue and returns when at least `min` are available, the timeout passed,
or the process is being killed (`-EINTR`). `min == 0` polls. A timeout is
a one-shot timer that wakes the sleeping thread; the tick's granularity
applies.

Because every execution happens in the process's own thread, `copy_to_user`
and `copy_from_user` behave exactly as in `read` and `write`, faults
included (`-EFAULT` in the completion), and no kernel worker thread ever
holds a user address space.

Regular files execute at submission and may take as long as their disk
I/O takes; the page cache is synchronous today. The ring's contract is
"completion delivered, never blocking on *readiness*"; true asynchrony
for block-backed files is what the block layer changes of this milestone
prepare (`docs/kernel/device/design.md`, "The block layer for NVMe") and
what a later milestone plugs in behind `PREAD`/`PWRITE` without changing
the interface.

A timer is a submittable object of its own (`timer_create`,
`docs/audit/next-subsystem-aio-timer.md`): a kobject with a readiness
operation that becomes readable when it has expired, so it rides `POLL` and
`READ` with no new op and lets a program multiplex "wake me after T" with
I/O in one ring and learn, by `user_data`, which fired. It is the timer the
ring's own `aio_wait` timeout — a bare return, not a completion — could not
be.

## Buffer ownership

From `aio_submit` until the completion is collected, the buffer named by
an entry belongs to the kernel: the process must not free, unmap or
reuse it. The kernel touches it only inside `aio_submit` and `aio_wait`
of the same process, so a violation cannot corrupt the kernel or another
process; it corrupts the violator's own data or completes the entry with
`-EFAULT`. Registered buffers (pinned once, named by index) are the
optimisation `flags` on `aio_create` leaves room for; they are not built
here.

## Lifetime and concurrency

The ring is a kobject; the handle table holds a reference, and every
parked request holds a reference to its object. Closing the last handle
runs the release: parked requests are dropped (their object references
put, nothing executed), completions discarded. A process that exits with
a ring open frees it through `handle_table_destroy` like everything else.
Only the creating process may submit or wait (`-EPERM` otherwise): a ring
handed to a child through spawn is a handle to an object whose buffers
live in another address space, and the kernel refuses rather than copy
the wrong memory. One mutex per ring serialises `aio_submit` and
`aio_wait`; two threads of the same process may share a ring and take
turns. The mutex is never held while sleeping on readiness: `aio_wait`
drops it before blocking and re-takes it to run entries, so a concurrent
`aio_submit` is never blocked by a waiter. Lock order: `ring->lock`
(mutex) → the object's own locks (through its operations) → wait queue
spinlocks (leaves).

## System calls

| Nr | Name | Arguments | Result |
|---|---|---|---|
| 60 | `aio_create` | `unsigned entries (1..1024), unsigned flags (0)` | a ring handle (READ and WRITE) |
| 61 | `aio_submit` | `int ring, const struct cosmo_sqe *sqes, unsigned n` | entries accepted (0..n); `-EBUSY` when the ring is full before the first, `-EFAULT`, `-EBADF`, `-EPERM` |
| 62 | `aio_wait` | `int ring, struct cosmo_cqe *cqes, unsigned n, unsigned min, uint64_t timeout_ns` | completions copied (may be 0 when `min` is 0 or the timeout passed); `-EINTR` when killed, `-EINVAL` (`min > n`), `-EFAULT`, `-EBADF`, `-EPERM` |

`SYS_COUNT` becomes 63. `COSMO_AIO_*` opcodes, `COSMO_AIO_F_NOWAIT`,
`struct cosmo_sqe` and `struct cosmo_cqe` join the UAPI; the libc gains
`cosmo_aio_create/submit/wait`. Linux `io_uring` is not mapped (its
shared rings are a different contract); `poll` and `epoll` remain stage 3
and can be built on `poll_wq` and `ready` alone.

## Polling (milestone 10)

`io_poll(struct io_pollfd *fds, unsigned n, uint64_t timeout_ns)`
(`kernel/io/poll.c`) is the ring's multi-queue wait as a plain call:
each entry names an object and the `COSMO_IO_*` bits of interest; the
call reports `ready & interest` (plus `HANGUP`/`ERROR` whenever set) per
entry, returns the number of entries with something set, and otherwise
sleeps on every object's `poll_wq` at once until one changes, the
timeout passes (0 polls, `COSMO_AIO_WAIT_FOREVER` waits) or the process
is killed or signalled (`-EINTR`). Objects without `ready` are always
readable and writable; objects without `poll_wq` can only be re-checked
by another entry's wake or the timeout. Linux `poll`/`ppoll` translate
onto it; a native `poll` can be added the same way.

## epoll (stage 3; the epoll, EPOLLET and epoll-close units)

`kernel/io/epoll.c` is an interest set: a kobject holding items keyed by
descriptor number, each a referenced member object with a wanted
`COSMO_IO_*` mask, the personality's opaque tokens, and one-shot and
edge state. `epoll_obj_wait` is `io_poll`'s multi-queue sleep over the
set: it snapshots the members under `ep->lock`, pins each with a
reference, arms its own wait entries on their queues and on the set's own
queue, evaluates, and sleeps only when nothing is ready
(`docs/audit/next-subsystem-epoll.md`, `next-subsystem-epollet.md`).

**Lifetime: a registration lives exactly as long as some descriptor to
its member.** Linux keys an epoll registration on the open file
description, not the descriptor: it is removed when the *last* descriptor
referring to that description is closed, in any process; a `dup`'d or
inherited descriptor keeps it; closing one of several does not remove
it. Here the open file description is the kobject in the handle slot (a
`dup` installs the same object; a file's per-open state is its own
kobject), so the rule maps onto a count: `struct kobject` carries
`handles`, the number of handle-table slots holding it across every
process (raised by `handle_install`/`_at`, lowered by `handle_close`), and
when `handle_close` takes it to zero it calls `epoll_last_handle_closed`
before its own put. Every item is linked on its member's `watchers` list
(`kobject.watchers`, a singly linked list through the items), which is
how that call finds the sets to remove it from: each item leaves its
set's list under that set's `ep->lock`, the set's queue is woken, and the
items' references are dropped outside the locks. The registration thus
never outlives the object it holds, and the object's release -- a
socket's FIN, a pipe end's end-of-file -- follows the last descriptor's
close as it does on Linux, where before this unit the registration kept
a closed socket alive and evaluated it forever (invariant A9).

The one lock added is a global mutex, `g_watch_lock` (as Linux's
`epmutex`), guarding every `watchers` list and the handle-count check an
add makes. Order: `g_watch_lock` outside `ep->lock`, always -- the add,
the DEL, the release and the last-close removal all take it first;
`epoll_obj_wait` takes `ep->lock` alone and never the watch lock, so a
waiter is never in the order. Three races are closed by it:

- *An add against the last close.* The door looked the descriptor up, so
  the object had a slot then; another thread's close can empty it before
  the add runs, and that close's removal finds no item. The add therefore
  re-reads `handles` under the watch lock and refuses an object with none
  (`-EBADF`): a registration nothing would ever remove is not made.
- *A reappearing descriptor.* A handle riding in a unix message is
  installed at the receiver, so the count can go 0 -> 1 between
  `handle_close`'s decrement and the removal's lock; the removal re-reads
  the count under the lock and keeps the registrations when a descriptor
  is back. (A description whose *only* remaining reference rides in an
  unread message still loses its registrations at that moment, where
  Linux keeps them until the message is received and that descriptor
  closed: recorded as a gap.)
- *The set closed while it holds entries.* `epoll_release` takes the
  watch lock to unlink its items from their members' lists before freeing
  them, or a member's later last close would walk freed items.

**`epoll_wait` running concurrently with the final close.** A sleeping
waiter holds a snapshot reference to every member and has its own wait
entry parked on the member's queue; the member cannot be freed under it
(the existing rule). The removal wakes the set's queue, so the waiter
finishes its entries, drops its pins and re-snapshots without the member;
the member's release follows that drop -- promptly, not at the member's
next event, which for a quiet connection would be never. A waiter that
collected the member just before the close may still report it; the
program's `close` has not returned at that point, which is a legal order
on Linux too.

**Nesting stays refused** (`-EINVAL` for an epoll as a member, as v1). The
reason is the wait protocol, not the lifetime rule: a member's events
wake the *member's* queue, and a waiter parks its own entries there; the
set's own queue is woken only by `ctl`. An outer set sleeping on an inner
set's queue would therefore sleep through every event of the inner set's
members. Linux's `ep_poll_callback` forwards a member's wake to the set's
queue through an item-owned wait entry, which is also what its loop
detection (`-ELOOP`, depth 5) is built on; that is a redesign of the
wait entry (a callback kind) and of the set's wake path, and the EPOLLET
unit's wake-generation counter does not substitute for it. Until then a
program that nests gets a loud `-EINVAL`, never a silent lost wake.

## What this gives the rest of the system

- **Files, sockets, pipes, the console** work today through the two
  operations every io type has or gains.
- **Devices** joined with the device-readiness unit: a character device's
  file reports readiness through its `chrdev_ops` (`/dev/console`,
  `/dev/tty`, `/dev/net/tap`), so a `READ` or `POLL` on the tap parks
  until a frame is transmitted and completes with it -- the `devices`
  section of `init --selftest` shows it. A block device node would
  implement `ready` as "queue not full" and `read`/`write` as bios; the
  ring needs nothing new.
- **Timers** are a `POLL` with a timeout today; a timer object with
  `ready` = "expired" is the natural addition.
- **IPC** (pipes) works; channels and events join through `ready`.
- **VM operations** (`vcpu_run`) are the one class the model does not
  cover: a vCPU is not an io object and its run is a long computation,
  not a readiness. It stays a synchronous call by design.
