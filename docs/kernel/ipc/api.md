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
uint64_t timeout_ns, bool private)` blocks while the user word equals
`val` (0 woken, `-EAGAIN` differs, `-ETIMEDOUT`, `-EINTR` killed,
`-EFAULT`, `-EINVAL` misaligned); `int futex_wake(struct vm_space *space,
uint64_t uaddr, unsigned n, bool private)` wakes up to `n` waiters and
returns the count. The compare and the enqueue happen under one bucket
lock (64 buckets).

**The key is what the word maps** (the shared-futex unit,
`docs/audit/next-subsystem-shared-futex.md`): `struct futex_key { obj,
off, held }` -- the space and the address for a word in the process's
own memory; the vnode and the file offset for a word in a `MAP_SHARED`
file mapping, with `held` the one vnode reference the waiter holds for
the vnode its current key names. `vm_user_futex_key(space, uaddr,
private, &key)` (`docs/kernel/memory/api.md`) classifies under the
space lock; `private` -- Linux's `FUTEX_PRIVATE_FLAG`, the program's
promise that nobody else can see the word -- and a space with no shared
mapping (`vm_space::shared_maps == 0`) both return the private key
without a walk. The native calls always classify. Two processes sharing
a page therefore share the futex; two mappings of one file in one
process too; a private mapping's word is the process's own (its page is
a copy once written). A waiter's reference is put at dequeue; a wake's
or a requeue's own reference lives for the call.
`int futex_requeue(space, uaddr1, uaddr2, nr_wake, nr_requeue, cmp, cmpval, private)`
wakes up to `nr_wake` and moves up to `nr_requeue` more onto the second
word; both words are classified, and a requeue that changes the key
exchanges the reference -- every waiter on the source word carries its
key, so there is one old vnode and one new however many move: the new
references are taken in one atomic add for every moved waiter
(`vnode_get_n`), under the bucket locks and before they drop, and the
old ones put after the locks are released (a put may block). A word requeued onto itself is counted and left where it is (the
move would push each waiter to the tail of the list being walked, an
unbounded walk with interrupts off — found and fixed by the native
thread door unit), and with nothing to wake it leaves `wake_seq` alone,
so the count does not wake a waiter caught between its compare and its
enqueue. Native: `SYS_futex_wait`/`SYS_futex_wake` (83, 84)
since the threads unit and `SYS_futex_requeue` (94, compare form) since
the native thread door; the Linux `futex` call reaches all three. Full
contract: `docs/compat/linux/api.md`.

## Unix domain sockets (`kernel/include/kernel/unix.h`)

The transport behind `COSMO_AF_UNIX`; `struct socket` owns a
`struct unix_sock` for the family and the socket layer's entry points
dispatch to these (`docs/kernel/ipc/design.md`, "Unix domain sockets").
`struct unix_addr { abstract, len, bytes[108] }` is a name as the doors
parse it (`len == 0`: none); `struct unix_handles { nr, objs[32],
rights[32] }` is what rides in a message.

- `int unix_create(struct socket *s)` / `void unix_release(struct socket *s)`:
  called by `ksock_create` and the socket's release.
- `int unix_bind(s, const struct unix_addr *a)`: `-EINVAL` unnamed or
  already bound; `-EADDRINUSE` if the name exists (path or abstract);
  the filesystem's error for a path (`-EOPNOTSUPP` without `mknod`,
  `-EACCES` without write permission on the directory).
- `int unix_listen(s, backlog)`: a bound stream socket; the backlog is
  clamped to `1..UNIX_BACKLOG_MAX`.
- `int unix_connect(s, a)`: a stream waits for backlog room holding no
  reference (or `-EAGAIN` non-blocking) and returns 0 with the
  server-side socket queued; `-ECONNREFUSED` for a name with no
  listener, a node that is not a socket, or a listener that has gone;
  the lookup's error for a path that does not resolve; `-EPROTOTYPE`
  for the other socket type; `-EISCONN`. A datagram socket records a
  default destination.
- `int unix_accept(s, struct socket **out)`: the queued socket,
  referenced, connected, with the peer's name and credentials;
  `-EAGAIN` non-blocking; `-EINVAL` once the listener is shut down.
- `int64_t unix_send(s, buf, len, to, struct unix_handles *h, dontwait)`:
  bytes sent (a stream may send fewer); `h` consumed on success;
  `-EPIPE`, `-ENOTCONN`, `-EMSGSIZE` (a datagram above
  `UNIX_MSG_MAX`), `-ECONNREFUSED` (the datagram's destination is
  gone), `-EAGAIN`, `-EINVAL` (a unix socket among the handles).
- `int64_t unix_recv(s, buf, len, from, h, flags, dontwait)`: bytes
  received, 0 at end of stream; `h->nr` in: the room, out: delivered;
  `flags` gets `COSMO_MSG_TRUNC` / `COSMO_MSG_HTRUNC`.
- `int unix_shutdown(s, how)` (after the socket's `shut` bits are set),
  `unix_getsockname`, `unix_getpeername`, `unix_peercred` (`-ENOTCONN`
  unless a connected stream socket), `unsigned unix_ready(s)`,
  `int unix_socketpair(type, &a, &b)`.
- `int unix_addr_parse(path, plen, out)` / `size_t unix_addr_pack(a, family, out)`:
  the user shape at both doors (a 16-bit family, then a NUL-terminated
  path, a path exactly as long as the length says, or a leading NUL and
  the bytes of an abstract name).
- `void unix_handles_drop(h)`, `unsigned unix_socket_count(void)`.

## System calls (`kernel/syscall/native.c`)

**`sendmsg(int h, const struct cosmo_msg *m)`** (97) and
**`recvmsg(int h, struct cosmo_msg *m)`** (98): one buffer, a
`struct cosmo_sockaddr_un` (in: a datagram's destination; out: the
sender's name, `addrlen` the full size back), handles with per-handle
rights (`COSMO_RIGHTS_SAME` or a subset), `nr_handles` (in: how many /
the room; out: how many landed) and `flags` (in `COSMO_MSG_DONTWAIT`;
out `TRUNC`, `HTRUNC`). Each handle sent passes
`handle_transfer_check`; `-EPERM` sends nothing. On an inet socket a
message carries no handles (`-EINVAL`). **`socketpair(int family, int
type, int h[2])`** (99): `AF_UNIX` only, two connected sockets
installed with `HANDLE_RIGHT_SOCK_CONNECTED`. `getsockopt(SOL_SOCKET,
SO_PEERCRED)` fills a `struct cosmo_ucred { pid, uid, gid }` on a
connected unix stream socket (`-ENOPROTOOPT` on an inet one). `bind`,
`connect`, `sendto`, `recvfrom`, `accept` and `getsockname` read or
write a `struct cosmo_sockaddr_un` when the family is `AF_UNIX`, the
length passed delimiting the name.


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
| a unix message's handles do not all fit the receiver's table or room | the first ones installed in order, the rest released, `COSMO_MSG_HTRUNC` |
| a unix socket handle in a message | `-EINVAL`, nothing sent (the cycle Linux garbage-collects is refused) |
| a listener released with connections queued | each client reads 0 and writes `-EPIPE` |
| a datagram destination released | `-ECONNREFUSED` to the sender, by name or default destination |
