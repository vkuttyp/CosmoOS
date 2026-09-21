# NEXT SUBSYSTEM — unix domain sockets: a name in the filesystem, and a handle that rides in a message

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and the
unit it names is the one the README has called a standing gap since the
service manager was built: "a named pipe and a unix socket — are both
things this kernel does not have" (README, the `svc` entry; the
deferred-work inventory, §1.3). It closes the unix-socket half of that
sentence, and with it three of the Linux personality's missing calls
(`sendmsg`, `recvmsg`, `socketpair`; inventory §2.6). Named pipes stay
open and are named at the end as the next thing this unit makes cheap.

## What is established (before this unit)

This section describes the tree as the report found it, which is the
state the design below starts from.

**Sockets are `AF_INET` and `AF_INET6`, and nothing else.** A socket is
a kobject (`kernel-services/network/socket.c`) with a `struct socket`
around a UDP pcb or a TCP pcb; `ksock_create` refuses every other family
with `-EAFNOSUPPORT`, and both doors say so: the native `SYS_socket`
passes the family through, and the Linux door checks it before calling
(`compat/linux/syscalls.c`, `lx_socket`), which `lxtest` asserts as
`socket(AF_UNIX) == -97`. The socket's identity to the system-call
layer is its kobject; its rights are the generic five plus four of its
own (`COSMO_RIGHT_SOCK_BIND/ACCEPT/CONNECT/SHUTDOWN`), an accepted
connection arriving with the connected set only. An address is one
shape for both families, `struct cosmo_sockaddr` (28 bytes: family,
port, four or sixteen bytes of address), parsed by `addr_from_user`
into the kernel's `struct netaddr`, which has no room for a path.

**Readiness is a property of the object's type.** `struct
kobject_io_type` carries `read`, `write`, `stat`, `ready`,
`set_nonblock`, `flush` and `poll_wq`; `poll`, `select` and the async
ring (`kernel/io/poll.c`, `kernel/io/aio.c`) ask the type, never the
object's kind. A new kind of socket that fills the table is pollable,
selectable and submittable on the day it exists.

**A handle already knows whether it may leave the process.**
`HANDLE_RIGHT_TRANSFER` is "give a handle to it to another process"
(`docs/kernel/object/architecture.md`, "Rights"), and today exactly one
path exercises it: spawn. `process.c`'s handle map installs a parent's
handle in the child only when the parent holds TRANSFER on it, and the
child may be given `COSMO_RIGHTS_SAME` or a subset, never more
(`kernel/process/process.c`, the `attr->handles` loop). That is the
whole rule this unit needs for a handle in a message; it is written
once and will be called from a second place.

**The pipe is the byte queue this unit reuses in spirit.**
`kernel/ipc/pipe.c`: a ring of `PIPE_SIZE` bytes under one spinlock
never held while blocking, two end objects with live counts, killable
waits, EOF when the last writer goes and `-EPIPE` when the last reader
does, no signal. Non-blocking is a property of an end. The shape is
right; it is one-directional and unnamed, which is what a socket is not.

**The filesystem has no node type for a socket.** `enum vnode_type` is
`REG`, `DIR`, `CHR`, `LNK`; `COSMO_DT_SOCK` (5) and `COSMO_DT_FIFO` (4)
exist as dirent types — a socket's `fstat` already reports `DT_SOCK` —
but nothing can create such a node: `struct vnode_ops` has `create`,
`symlink`, `mkdir` and no `mknod` (the `svc` design says so in as many
words). Character nodes are made by the kernel alone through
`ramfs_mkchr`, which builds a `VNODE_CHR` with a `chrdev_ops` and a
private pointer in the `ramfs_node`. The root filesystem is ramfs,
populated from the boot archive, with `/proc` a procfs and cosmofs
mounted where a test or an operator puts a disk; cosmofs's on-disk
inode type is `REG`, `DIR` or `LNK` (`cosmofs_format.h`, `CFS_TYPE_*`),
so a special node on cosmofs would be a format change.

**Credentials are on the process.** `struct credentials { ruid, euid,
suid, rgid, egid, sgid, groups }` (`kernel/include/kernel/cred.h`),
`cred_in_group`, `vfs_permission(vn, mask)` for a node's mode against
the caller; a process has a pid, a cwd and a root, and a path resolved
by a jailed process starts at its root.

## The problem

### A program that needs a local channel has one primitive, and it is unnamed

Two processes that are not parent and child cannot talk. A pipe is
inherited; a TCP socket on loopback is a network connection with a
port to choose, a stack to traverse and no way to say who is on the
other end. The `svc` entry chose "no daemon" partly for good reasons
and partly because the control channel a daemon needs did not exist;
the deferred inventory carries that sentence as a gap, not a decision.
Every Unix program that uses a local service — a display server, a
D-Bus, a database's local connector, `ssh-agent` — expects a socket
with a name, and on this system the name cannot exist.

### A handle cannot move between two running processes

The rights model says a handle may be given away (TRANSFER), and the
only giver is spawn: the handle moves at birth or never. A server that
opens a file on a client's behalf and hands the client the handle, a
supervisor that passes a listening socket to a replacement, a sandboxed
helper that receives exactly one descriptor — each is the pattern the
constitution's capability vocabulary was written for, and none is
expressible. Linux calls it `SCM_RIGHTS`; here it is the second caller
of a rule that already exists.

### The Linux personality answers three common calls with `ENOSYS`

`sendmsg`, `recvmsg` and `socketpair` are missing (inventory §2.6). They
are how musl's `getaddrinfo` talks to a resolver, how any program
passes a descriptor, and how a great many programs make a private
bidirectional channel without a port. The first two are not only unix
calls — `sendmsg` over UDP is ordinary — but their reason to exist on
this system is the unix socket, and this unit builds them with it.

## Design

### One socket object, a third family

`struct socket` gains `COSMO_AF_UNIX` (1, Linux's value, as 2 and 10
are Linux's) and a pointer to `struct unix_sock` alongside the UDP and
TCP members; `ksock_create` allocates it for the new family and every
`ksock_*` entry point dispatches on `s->family` before touching a pcb.
Nothing above the socket layer learns a new kind of object: the same
kobject type, the same rights bits, the same `sock_of_err` at both
doors, the same `fstat` (`DT_SOCK`), the same readiness table. The
transport lives in **`kernel/ipc/unix.c`**, because it is IPC and the
network stack is not involved; `socket.c` calls it and knows nothing of
its queues.

Two types, as Linux: **`SOCK_STREAM`**, a connection with a byte queue
each way, and **`SOCK_DGRAM`**, a queue of messages with a sender. No
`SOCK_SEQPACKET` (`-ESOCKTNOSUPPORT`, which is what Linux answers for a
type a family lacks); it is a stream with record boundaries and can be
added when something needs it.

### A stream is a pair, and the pair is the pipe twice

A connection is one `struct unix_conn`: a spinlock, two rings of
`UNIX_BUF` bytes (64 KiB; the pipe's 16 KiB is sized for `PIPE_BUF`
atomicity, a socket is sized for throughput), two waitqueues per
direction, and two live flags. Each connected `unix_sock` points at the
conn and knows which side it is; a write is a pipe write into the ring
the peer reads, a read is a pipe read from the other one, with the
pipe's rules unchanged: the lock is never held while blocking, waits
are killable, a read on an empty ring whose writer side has closed
returns 0, a write whose reader side has closed returns `-EPIPE` and
no signal (as the pipe and the TCP socket both do on this system;
`MSG_NOSIGNAL` is accepted and means nothing more). `shutdown(WR)`
closes this side's write flag and wakes the peer's readers; `shutdown(RD)`
discards. The conn is freed when both sides have released it, which is
the pipe's `readers == 0 && writers == 0`.

### A listener is a queue of connections that have not been accepted

`listen` on a bound stream socket gives it an accept queue bounded by
the backlog (clamped to `UNIX_BACKLOG_MAX`, 128). `connect` from a
stream socket to a name finds the listener, and if the queue has room
**creates the server-side socket then and there**, links the two
through a fresh conn, queues the server side, wakes the listener, and
returns 0 — the client may write before anyone accepts, and the bytes
wait in the ring, which is Linux's behaviour and what a client that
connects-then-sends expects. A full queue makes a blocking connect wait
(killable) and a non-blocking one return `-EAGAIN`; a name with no
listener behind it is `-ECONNREFUSED`. `accept` dequeues, installs the
queued socket with `HANDLE_RIGHT_SOCK_CONNECTED`, and reports the
peer's name if it has one. Closing a listener refuses its queued,
unaccepted connections: their client sides read EOF and write `-EPIPE`,
exactly as if the server had accepted and closed.

### A datagram socket is its own queue

A `SOCK_DGRAM` unix socket owns a queue of `struct unix_msg` (bytes,
length, the sender's name if bound, and the ancillary items below),
bounded two ways: `UNIX_DGRAM_MAX` messages and `UNIX_DGRAM_BYTES` bytes
in total (64 and 256 KiB). `sendto(name)` resolves the name to a bound
dgram socket and enqueues, blocking (killable) while the receiver's
queue is full unless non-blocking (`-EAGAIN`); `connect` on a dgram
socket only records a default destination, as Linux. A message larger
than `UNIX_MSG_MAX` (the ring size) is `-EMSGSIZE`; a receiver reading
into a smaller buffer gets the prefix and the rest is dropped, with the
truncation flag (below). `recvfrom` reports the sender's name — a
filesystem path or an abstract name, or nothing for an unbound sender.

### The name: a node in the filesystem, or nowhere

**`bind` to a path creates a node.** A new vnode type `VNODE_SOCK`
(`COSMO_DT_SOCK`, so `readdir`, `stat` and `ls` already know its name)
and a new vnode operation, **`mknod(dir, name, len, mode, type, &vn)`**
— the operation the `svc` design named as absent — implemented by ramfs
for `VNODE_SOCK` (and ready for `VNODE_FIFO`, which is the named-pipe
unit's business). `bind` resolves the parent directory from the
caller's cwd and root, as `open(O_CREAT)` does, so a jailed process
binds inside its jail; it needs write permission on the directory; the
name must not exist (`-EADDRINUSE`, Linux's answer, and a socket that
was there and has since closed is still a name that exists — the
program unlinks it first, as on Linux). The node's mode is 0755 (Linux's
usual 0777 masked by a 022 umask; this system has no umask, so the
number is the rule, and `chmod` widens it), its uid and gid the
caller's. cosmofs has no
inode type for it, so `mknod` on a cosmofs directory answers
`-EOPNOTSUPP` from the filesystem, and `bind` reports it: socket names
live under the ramfs root — `/tmp`, `/run`, wherever the program puts
them — which is where Unix puts them too, and a persistent socket node
on a disk filesystem is a format change this unit does not make.

**The node is a name, not the socket.** `unix.c` keeps a registry of
bound sockets keyed by the vnode pointer (a vnode is one object for as
long as it is referenced, and the bound socket holds a reference), under
one spinlock. `connect(path)` and `sendto(path)` resolve the path from
the caller's cwd and root (`vfs_lookup`, so a jail cannot name a socket
outside it, and a symlink to one works), require `VNODE_SOCK`
(`-ECONNREFUSED` for anything else, as Linux), require write permission
on the node (`vfs_permission`, Linux's rule: the node's mode is the
access control), then look the vnode up in the registry: a listening
stream socket or a dgram socket, or `-ECONNREFUSED` when the socket that
made the node has closed. `unlink` removes the name from its directory
through the ordinary path, so a later `connect` is `-ENOENT`, and the
bound socket keeps its vnode reference until it closes — connections
already made are untouched, as on Linux. `open()` of a socket node is
`-ENXIO`. The registry entry is removed when the socket is released,
which is the only place a socket leaves it; a node whose socket is gone
is an ordinary dead name that `rm` removes.

**Abstract names are scoped by the root.** A path whose first byte is
NUL names an abstract socket, Linux's namespace that is not in the
filesystem (the rest of the address, length-delimited, is the name).
They are common enough — D-Bus, systemd's notify socket, Chromium's
sandbox — that a Linux program will use one. This system's jails are
roots, and a global abstract namespace would be a hole through every
one of them, so the registry keys an abstract name by **(the caller's
root vnode, the bytes)**: a jailed process sees only the abstract
sockets bound by processes with the same root. The bound socket holds a
reference to that root vnode from bind to release, exactly as a
path-bound socket holds its node's, so the pointer the key uses cannot
be freed and reused for another root while it is a key; a jail whose
root is torn down after its last process leaves has, by then, no bound
abstract sockets left to key on it. That is one comparison more than
Linux does, and it is the comparison the security model requires.

**`getsockname` and `getpeername`** return the bound name (the path as
given at bind, abstract names as given) in a `struct
cosmo_sockaddr_un`; an unbound socket reports the family alone.

### The message: bytes, a name, handles

Two new native calls, **`SYS_sendmsg`** and **`SYS_recvmsg`**, take a
`struct cosmo_msg`:

```c
struct cosmo_msg {
    void *buf;                          /* the bytes */
    size_t len;
    struct cosmo_sockaddr_un *addr;     /* sendmsg: read as the destination (dgram), NULL when connected;
                                           recvmsg: written with the sender's name, NULL to skip -- one
                                           field read by one call and written by the other, so not const,
                                           as recvfrom's and accept's address pointers are not */
    size_t addrlen;                     /* in: the buffer's size; recvmsg out: the name's size */
    int *handles;                       /* sendmsg: handles to send; recvmsg: where received ones land */
    const unsigned *rights;             /* sendmsg: per handle, COSMO_RIGHTS_SAME or a subset; NULL = SAME */
    unsigned nr_handles;                /* in: how many / room for how many; recvmsg out: how many landed */
    unsigned flags;                     /* in: COSMO_MSG_DONTWAIT; recvmsg out: COSMO_MSG_TRUNC, COSMO_MSG_HTRUNC */
};
```

One buffer, not a vector: this ABI has no `iovec` and no `readv`, and
gathering is the library's business. The Linux door's `msghdr` has a
vector, and `lx_sendmsg` walks it into the socket one element at a
time (a stream accepts a partial write and the loop continues; a
datagram is gathered into one kernel buffer bounded by
`UNIX_MSG_MAX`), and `lx_recvmsg` scatters one message into the vector.

**Where the handles go on a stream, at the Linux door.** `lx_sendmsg`
walks `msg_iov` one element at a time into `unix_send`, and the
`SCM_RIGHTS` set rides with the **first** call only -- the one that
writes the first byte of the send, which is where Linux places
ancillary data -- with every later element a plain write; an empty
first element is skipped so the handles ride with a byte. A stream
send that blocks or returns short after that first call has already
delivered its handles with the bytes it wrote, which is Linux's
behaviour for a partial `sendmsg`; the count returned is the bytes
written. A datagram is gathered into one kernel buffer and sent once,
handles and all.

**A handle in a message is spawn's rule, called again.** For each
handle named, the sender must hold `HANDLE_RIGHT_TRANSFER` on it, and
the rights it names must be `COSMO_RIGHTS_SAME` or a subset of what it
holds — else `-EPERM` and nothing is sent. The kernel takes one
reference per handle, stores the kobject pointer and the rights in the
message, and the message owns those references while it is in flight:
a socket released with messages queued drops every reference in them.
At `recvmsg` the handles are installed in the receiver's table with the
rights the message carries, and the numbers are written to `handles[]`.
"What fits" is literal and in message order: `handle_install` is
called for each handle in turn, and the first `-EMFILE` (or the end of
the caller's `handles[]` room) ends the installing -- the handles
installed so far stay installed, they are the receiver's now; the rest
are dropped (their references put), `nr_handles` reports how many
landed, and `COSMO_MSG_HTRUNC` is set. No reservation and no rollback:
Linux's `MSG_CTRUNC` behaviour is exactly this (`scm_detach_fds`
installs until `get_unused_fd` fails and closes the remainder), and a
receiver that wants all-or-nothing checks the count before trusting
the set. A `recvmsg` that names no room
(`nr_handles == 0`) on a message that carries handles gets the bytes
and the flag and the handles are dropped: a program that does not ask
for descriptors is not made to hold them. On a stream, ancillary items
belong to the bytes they were sent with: a message's handles are
delivered with the first byte of that send and never with an earlier
one, so a read stops at the boundary of a send that carried handles,
and `recv`/`read` (no message structure to receive into) delivering
those bytes drop the handles with the same truncation rule.

**A unix socket cannot ride in a message.** Sending a handle to a unix
socket over a unix socket is `-EINVAL`. This is the one place the unit
says less than Linux, on purpose: two sockets each in flight inside the
other's queue reference each other with nothing holding either from
outside, which Linux resolves with a garbage collector over in-flight
descriptors (`net/unix/garbage.c`), a cycle detector run at close. This
system counts its objects and asserts the counts at exit; a leak by
construction is not an option, and a collector is a unit of its own if
a program turns up that needs to pass a socket rather than a pipe, a
file or an inet socket. Every other kind of handle rides. The
restriction is on the object's kind, not the handle's rights, and is
recorded as a risk below with the escape hatch named.

**`SO_PEERCRED`.** A connected stream socket (either side of an accept,
either end of a socketpair) records the peer's pid, effective uid and
effective gid at the moment the connection was made, and
`getsockopt(SO_PEERCRED)` returns them (`struct cosmo_ucred`; Linux's
`struct ucred` at the Linux door). This is what a local server uses to
decide who is talking to it, cheaper and more trustworthy than a
protocol handshake, and it is the credential the constitution's
security section wants a service to have.

### `socketpair`

**`SYS_socketpair(family, type, handles[2])`** makes a connected pair
of stream sockets (or dgram, each connected to the other) with no name:
a conn with two ends, both installed with `HANDLE_RIGHT_SOCK_CONNECTED`.
It is the pipe that goes both ways and carries handles, and with spawn's
handle map it is how a parent gives a child a private channel. The
Linux door maps `socketpair` (x86-64 53, AArch64 199) onto it.

### The Linux door

`lx_socket` accepts `AF_UNIX` (1) with `SOCK_STREAM`/`SOCK_DGRAM` and
the two flags; `struct lx_sockaddr_un` is Linux's (`sa_family` and 108
bytes of path, the path NUL-terminated unless the length says otherwise
— an unterminated path of exactly the length given is valid, as Linux
treats it, and a leading NUL is abstract with the length as the name's
end). `bind`, `connect`, `sendto`, `recvfrom`, `getsockname`,
`getpeername` and `accept4` read and write it by the length the caller
passes. **`sendmsg`** (x86-64 46, AArch64 211) and **`recvmsg`** (47,
212) translate `struct msghdr` and its control buffer: `SCM_RIGHTS`
(`SOL_SOCKET`, 1) carries `int` descriptors in and out, at most
`UNIX_HANDLES_MAX` (32) per message, rights `COSMO_RIGHTS_SAME` always
(Linux has no narrower notion; a Linux program that wants to give less
is a native program); `MSG_CTRUNC` (8) and `MSG_TRUNC` (0x20) come back
in `msg_flags`; `MSG_DONTWAIT` (0x40) and `MSG_NOSIGNAL` (0x4000) are
honoured on the way in; any other control type is `-EINVAL`.
`getsockopt(SO_PEERCRED)` (17) fills `struct ucred { pid, uid, gid }`.
Errors are Linux's numbers: `EAFNOSUPPORT` becomes a thing of the past
for `AF_UNIX`, `ECONNREFUSED` (111), `EADDRINUSE` (98), `ENOENT`,
`ENXIO` (6) on `open`, `EMSGSIZE` (90), `ESOCKTNOSUPPORT` (94).

### Lifetime, in one paragraph

A `unix_sock` is freed with its `struct socket` (the kobject's release).
A bound socket holds one vnode reference from bind to release, and its
registry entry lives exactly that long. A conn is held by its two ends
and by nobody else. A queued, unaccepted connection is held by the
listener's queue (one reference to the server-side socket) and by the
client (the conn); closing the listener drops the queue's references
and marks each conn's server side dead. A message holds one reference
per handle it carries and is freed with its bytes when received or when
its queue's socket is released. Nothing takes a reference to a process:
`SO_PEERCRED` copies three integers. Invariant **I8** states it: *every
reference a unix socket, a connection or a message holds is dropped by
the release of the object that holds it, and `socket_count` returns to
its value after every test that creates one.*

### Locking

One spinlock per conn (the pipe's rule: never held while blocking or
touching user memory), one per dgram socket's queue, one for the
registry, and the socket's existing mutex around connect/accept/listen
state changes as the inet path uses it. Lock order: socket mutex →
registry lock → conn/queue spinlock, never the reverse; a registry
lookup takes a reference to the listener and drops the registry lock
before touching the listener's mutex. Handle installation at `recvmsg`
happens after the message has been dequeued and the queue lock dropped:
the handle table's lock is never nested under a socket lock.

## Affected files

| file | change |
| --- | --- |
| `kernel/ipc/unix.c`, `kernel/include/kernel/unix.h` (new) | the transport: `struct unix_sock`, `unix_conn`, `unix_msg`; bind/listen/accept/connect/send/recv/shutdown/peercred; the name registry; the handle-passing rule |
| `kernel/ipc/unixtest.c` (new) | the kernel self-tests below |
| `kernel-services/network/socket.c`, `kernel/include/kernel/socket.h` | `COSMO_AF_UNIX` accepted; `un` member; every `ksock_*` dispatches on the family; `ksock_sendmsg`/`ksock_recvmsg`/`ksock_socketpair`/`ksock_peercred` |
| `kernel/include/kernel/vfs.h`, `kernel-services/vfs/vfs.c`, `kernel-services/vfs/ramfs.c` | `VNODE_SOCK`; `vnode_ops.mknod`; `vfs_mknod(start, path, mode, type, &vn)`; ramfs implements it for `VNODE_SOCK`; `open` of a `VNODE_SOCK` is `-ENXIO`; cosmofs leaves `mknod` NULL (`-EOPNOTSUPP`) |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_AF_UNIX`, `struct cosmo_sockaddr_un`, `struct cosmo_msg`, `struct cosmo_ucred`, `COSMO_MSG_*`, `COSMO_SO_PEERCRED`; `SYS_sendmsg` 97, `SYS_recvmsg` 98, `SYS_socketpair` 99; `SYS_COUNT` 100 |
| `kernel/syscall/native.c` | `addr_from_user` reads the family first and the unix shape when it says so; `sys_sendmsg`, `sys_recvmsg`, `sys_socketpair`; `getsockopt(SO_PEERCRED)` |
| `compat/linux/syscalls.c`, `compat/linux/linux_abi.h` | `AF_UNIX` at `lx_socket`; `lx_sockaddr_un` both ways; `lx_sendmsg`, `lx_recvmsg` (msghdr, cmsghdr, `SCM_RIGHTS`), `lx_socketpair`; `SO_PEERCRED`; the three table entries |
| `libc/include/sys/socket.h`, `libc/include/sys/un.h` (new), `libc/include/cosmo/syscall.h`, `libc/src/unistd.c` | `AF_UNIX`, `sockaddr_un`, `socketpair`, `cosmo_sendmsg`/`cosmo_recvmsg`/`cosmo_socketpair`, `SO_PEERCRED` |
| `userland/init/init.c` | a `unix` section of `init --selftest`; probe modes for the two-process cases; `USERBENCH: unix` |
| `tests/boot/run_boot_test.py` | `"unix"` in `USERTEST_SECTIONS` |
| `tests/linux/lxtest.c` | the Linux rows below |
| `kernel/core/selftest.c` | the new self-tests registered |
| `docs/kernel/ipc/{design,api,invariants,testing}.md`, `docs/kernel-services/vfs/{design,api}.md`, `docs/kernel/object/architecture.md` (TRANSFER's second caller), `docs/compat/linux/{api,testing}.md`, `docs/syscall/api.md`, `docs/libc/api.md`, `docs/userland/{design,testing}.md` (the `svc` sentence becomes history), README Status, the inventory (§1.3's unix half and §2.6's three calls struck) | as built |

## New APIs

```c
/* kernel/include/kernel/unix.h */
int  unix_create(struct socket *s);                         /* called by ksock_create for COSMO_AF_UNIX */
void unix_release(struct socket *s);                        /* drops the name, the conn, the queue, every reference */
int  unix_bind(struct socket *s, const char *name, size_t len, bool abstract);
int  unix_listen(struct socket *s, int backlog);
int  unix_accept(struct socket *s, struct socket **out);
int  unix_connect(struct socket *s, const char *name, size_t len, bool abstract);
int64_t unix_send(struct socket *s, const void *buf, size_t len, const char *to, size_t tolen, bool abstract,
                  struct kobject *const *objs, const unsigned *rights, unsigned nr, unsigned flags);
int64_t unix_recv(struct socket *s, void *buf, size_t len, struct unix_name *from,
                  struct kobject **objs, unsigned *rights, unsigned *nr, unsigned *flags);
int  unix_shutdown(struct socket *s, int how);
int  unix_peercred(struct socket *s, struct cosmo_ucred *out);
unsigned unix_ready(struct socket *s);
int  unix_socketpair(int type, struct socket **a, struct socket **b);

/* kernel/include/kernel/vfs.h */
int vfs_mknod(struct vnode *start, const char *path, uint32_t mode, enum vnode_type type, struct vnode **out);
/* struct vnode_ops */ int (*mknod)(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                                    enum vnode_type type, struct vnode **out);

/* uapi */
#define COSMO_AF_UNIX 1
struct cosmo_sockaddr_un { uint16_t family; char path[110]; };   /* NUL-terminated, or length-delimited; path[0] == 0: abstract */
struct cosmo_ucred { int32_t pid; uint32_t uid, gid; };
#define COSMO_SO_PEERCRED 17
#define COSMO_MSG_DONTWAIT 0x40
#define COSMO_MSG_TRUNC    0x20
#define COSMO_MSG_HTRUNC   0x08   /* handles dropped for want of room (Linux MSG_CTRUNC) */
#define SYS_sendmsg 97 /* (int h, const struct cosmo_msg *) -> bytes */
#define SYS_recvmsg 98 /* (int h, struct cosmo_msg *) -> bytes */
#define SYS_socketpair 99 /* (int family, int type, int h[2]) -> 0 */
```

The handle-passing rule is one function, **`handle_transfer_check(table,
h, give, &obj, &rights)`** in `kernel/object/handle.c`, which spawn's
loop and `unix_send` both call: TRANSFER held, `give` SAME or a subset,
the object referenced and returned with the rights to install. Spawn
loses its inline copy of the rule.

## Migration plan

Nothing existing changes meaning. `AF_UNIX` was an error and becomes a
family; `sendmsg`, `recvmsg` and `socketpair` were `ENOSYS` at the Linux
door and absent natively. `lxtest`'s `socket(AF_UNIX) == -97` row is
replaced by the rows below. The inet paths are untouched apart from the
family dispatch at the top of each `ksock_*` entry, which is one
comparison. `SYS_COUNT` moves from 97 to 100; libc and the syscall
filter's table follow (the filter is by number: a confined process that
was never granted the new numbers cannot use them, which is the right
default).

## Tests

| test | what it proves |
| --- | --- |
| `unix-stream` (kernel) | bind/listen/connect/accept on a path; bytes both ways; a client's write before accept is read after; EOF on peer close; `-EPIPE` on write to a closed peer, no signal; `shutdown` each way; the backlog: a full queue blocks a connect and a non-blocking one gets `-EAGAIN`; closing the listener refuses the queued connections (EOF, `-EPIPE`); `-ECONNREFUSED` for a name whose socket is gone and for a node that is not a socket |
| `unix-dgram` (kernel) | sendto by name, recvfrom with the sender's name; a connected dgram socket's default peer; the queue's two bounds (a full queue blocks, `-EAGAIN` non-blocking); `-EMSGSIZE`; truncation with the flag |
| `unix-name` (kernel) | `bind` makes a `VNODE_SOCK` node with the caller's uid/gid and mode 0755; a second bind is `-EADDRINUSE`; `open` is `-ENXIO`; `unlink` then `connect` is `-ENOENT` while the existing connection lives on; a node the caller cannot write refuses `connect` (`-EACCES`); `mknod` on cosmofs is `-EOPNOTSUPP`; an abstract name bound under one root is invisible from a process with another root |
| `unix-handles` (kernel) | a pipe end sent and received with SAME rights; sent with a subset, received with the subset; sent without TRANSFER: `-EPERM` and nothing queued; a unix socket handle: `-EINVAL`; a receiver with room for one of three: one installed, `HTRUNC`, and `kobject` counts show the other two released; a socket released with queued handles releases them; ancillary items stay with their bytes on a stream (a read stops at the boundary) |
| `unix-lifetime` (kernel) | `socket_count` and the pipe/vnode counts before and after every case above; the leak test I8 rests on |
| `unix-close-race` (kernel, two CPUs) | a peer closed under a blocked reader, a blocked writer, a blocked connector and a blocked accepter, each on the other CPU: every wait returns with the right answer and nothing is used after release (the poisoner is on) |
| `unix-poll` (kernel) | readiness through the type: a listener is readable when a connection is queued; a stream is readable/writable by its rings and HANGUP when the peer closes; the async ring submits a read on a unix socket and completes it |
| `init --selftest`, section `unix` (native) | `socketpair` between parent and a spawned child through the handle map; `sendmsg` with a file handle to the child, the child reads the file; `SO_PEERCRED` says the parent's pid/uid/gid; a jailed child (`cosmo_spawn` with a root) cannot connect to the parent's abstract name or its path outside the jail; a `poll` on a unix socket |
| `lxtest` rows | `socket(AF_UNIX, STREAM)` and `DGRAM`; bind/listen/connect/accept with `sockaddr_un` (terminated, unterminated, abstract); `socketpair`; `sendmsg`/`recvmsg` with `SCM_RIGHTS` (a pipe fd arrives and works; `MSG_CTRUNC` when `msg_controllen` is short); `SO_PEERCRED`; `SOCK_SEQPACKET` is `-94`; `open` of a socket node is `-6` |

**Bug-proofs, to run.** Each mutation alone on x86-64, the debug suite
booted, the file restored:

- the rights not narrowed at send (`give` ignored) → `unix-handles`: the
  handle arrives with the sender's full rights.
- TRANSFER not checked → `unix-handles`: the send succeeds where
  `-EPERM` is expected.
- the registry entry not removed at release → `unix-stream`: a connect
  to a dead name finds a freed listener; the poisoner or a use-after-
  free report (it may instead be `-ECONNREFUSED` by luck, which the
  report says in advance; the `unix-lifetime` count still fails because
  the vnode reference is never dropped).
- abstract names not keyed by the root → `unix-name`: the other root
  connects.
- the unix-socket-in-flight refusal removed → `unix-lifetime`: two
  sockets sent into each other and closed never return `socket_count`.
- queued handles not released at socket release → `unix-lifetime`: the
  pipe's object count does not return.
- the backlog not bounded → `unix-stream`: the 129th connect succeeds.
- the ancillary boundary ignored on a stream → `unix-handles`: a read
  crosses into a send that carried handles and the handles are lost
  without `HTRUNC`.

## Benchmarks

`USERBENCH: unix`, both architectures, recorded in the as-built banner:
(a) the round trip of one byte over a unix stream socketpair, against
the same over a pipe pair and over a TCP loopback connection — the
socket should sit between the two, nearer the pipe; (b) 4 KiB messages
over a dgram socket, messages per second; (c) `sendmsg` carrying one
handle, round trips per second, against (a) — the cost of a reference,
a table install and a message structure.

## Risks

**No unix socket in flight.** A Linux program that passes a unix socket
over a unix socket gets `-EINVAL` where Linux says yes. Known users:
systemd's descriptor store (not run here), some D-Bus service
activation, `ssh` connection sharing. The escape hatch is a cycle
collector over in-flight messages, which is a unit of its own with its
own report; this unit refuses rather than leaks, and says so in the
Linux compat docs' list of differences.

**Names on cosmofs.** `bind` to a path on a cosmofs mount is
`-EOPNOTSUPP`. Nothing on this system puts a socket on a disk
filesystem — the root is ramfs — and a Linux program that binds under
`/var/run` on a persistent volume would be the first; adding
`CFS_TYPE_SOCK` is a format bump for another unit.

**Handle-table pressure.** `HANDLE_TABLE_SIZE` is 64. A message
carrying up to 32 handles into a table with 40 in use truncates; the
receiver is told (`HTRUNC`) and the dropped handles are released, but a
program that assumed Linux's larger tables will see it. The table's
size is the constraint, not this unit's; the truncation rule is
Linux's.

**The pair created at connect.** The server-side socket exists before
`accept`, so a client can fill its ring before the server has seen the
connection; that is bounded by `UNIX_BUF` per direction and per
connection and by the backlog per listener — at most 8 MiB parked
behind one listener that never accepts. Linux behaves the same, with
the same bound shape.

## Alternatives considered

**A new kobject kind rather than a third family in `struct socket`.**
Cleaner in isolation, but every door (`sys_bind`, `lx_connect`, …)
already converts a handle to a `struct socket` and checks socket
rights; a second kind means every door grows a branch or the rights
vocabulary a second socket type. The family dispatch inside `ksock_*`
is one comparison per call and keeps the doors as they are.

**Names only in the abstract namespace, no filesystem node.** It would
avoid `mknod` and `VNODE_SOCK`. It would also leave every Linux program
that binds a path — nearly all of them — without a socket, and leave
the filesystem's permission model out of who may connect. The node is
the access control; the abstract namespace is the exception, and it is
scoped by the root for the same reason.

**Passing handles by a separate call (`SYS_handle_send`) instead of in
the message.** Simpler ABI, but a handle that arrives detached from the
bytes that explain it is a race the receiver has to resolve; Unix put
the descriptor in the message forty years ago because that is where
the protocol wants it, and the Linux door needs `SCM_RIGHTS` regardless.

**A garbage collector for sockets in flight, now.** It is the only way
to say yes to a socket in a message, and it is a cycle detector over
every queued message in every unix socket, run at close, with the
in-flight count per socket Linux keeps (`unix_tot_inflight`,
`gc_candidates`). It is real complexity for a case no program on this
system has yet asked for; the refusal is explicit, tested and recorded,
and the collector is named as the unit that lifts it.

**Named pipes in the same unit.** `mknod` and `VNODE_FIFO` are half of
it, and the pipe's queue is the other half; but a FIFO's open semantics
(a blocking open until the other side arrives, `O_NONBLOCK` rules,
`ENXIO` for a writer with no reader) are their own page of rules and
their own tests, and this report is long enough. The next report can
say "a FIFO is a pipe with a `VNODE_FIFO` name, opened through `mknod`"
and be short.
