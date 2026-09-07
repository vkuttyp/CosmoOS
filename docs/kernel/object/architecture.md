# Kernel Object Model and Handles: Architecture and Design

## Purpose

Give every important kernel entity one lifetime discipline and one way
for user space to name it. A `kobject` is a reference-counted header
with a type; a handle is a small integer in a per-process table that
holds one reference and a set of rights. User space never sees a kernel
pointer (constitution section 11).

## Position

```text
   process (handle table)  ──►  kobject (console, later file, socket, device, ...)
   syscalls: handle_lookup(table, h, rights) → referenced kobject → type-specific ops
```

## Responsibilities

- `struct kobject`: type pointer and atomic reference count.
- `struct kobject_type`: name, `release` and `flags` (`KOBJECT_TYPE_IO`
  marks an io type, so a plain object is never mistaken for one);
  subtypes extend it (the console type, since Phase 7 `struct file`,
  since Phase 8 `struct socket`, since Phase 9 the two pipe ends, and
  since Phase 12 `struct vm` add `read`/`write`, an optional `stat`,
  and since milestone 8 optional `ready` and `set_nonblock` through
  `kobject_io_type`; `struct vcpu` is a plain kobject). Other kobjects
  today: `struct vnode` and `struct mount` (`kernel-services/vfs/`),
  `struct device` (`kernel/device/`), `struct blkdev` (`kernel/block/`),
  `struct process`.
- `struct handle_table`: fixed 64 slots, spinlock, install/lookup/close,
  `handle_get` (object plus rights, for `dup` and `spawn`), destroy when
  the process's last thread is gone.
- Rights: a capability vocabulary carried by the handle, not by the
  object (see "Rights" below). Since Phase 9 handles cross process
  boundaries: `spawn` copies chosen handles into the child (a
  capability-style map), and `dup` copies within a process; both are now
  gated by rights and both can hand over *less* than they hold.

## Rights

A handle is a capability: what a process may do with an object is what
its handle says, not what the object is or who the process runs as. The
constitution's aim for section 54 is that privileged operations stop
depending on being uid 0, and that begins with a vocabulary wide enough
to say something other than "read" and "write".

```text
  bits 0..15   generic, meaning the same for every object kind
    READ       take data out of it
    WRITE      put data into it
    DUP        make another handle to it, in this process
    TRANSFER   give a handle to it to another process
    MANAGE     change how it behaves, as opposed to using it
  bits 16..31  per type, meaning whatever that object kind says
```

Three rules make the vocabulary worth having:

- **Rights only ever shrink.** `dup` and `spawn` may hand over a subset
  of what the caller holds and nothing more, so a process can pass a
  read-only view of something it can write, and cannot recover what it
  gave up. There is no operation that adds a right to an existing
  handle.
- **Holding a handle is not permission to pass it on.** DUP and TRANSFER
  are separate rights and separate from READ and WRITE, because "you may
  read this" and "you may give this to anyone" are different statements.
- **Using an object and administering it are different.** MANAGE covers
  the operations that change an object's behaviour rather than its
  contents -- making it non-blocking today, and whatever each type
  decides its upper sixteen bits mean.

A creator gets every right its type defines: reducing rights is
something a process does deliberately, on the way to somebody else.

### The upper sixteen bits

The generic five say what a process may do with an object's *contents*.
They say nothing about the operations that are neither reading nor
writing: naming a socket, listening on it, connecting it somewhere,
running a vCPU, injecting an interrupt into it. Those are what the
type's own bits are for.

```text
  socket   BIND      give it a local address, and listen on it
           ACCEPT    take a connection from its queue
           CONNECT   reach out to a peer
           SHUTDOWN  end a direction of an established connection
  vm       MAP       give the guest memory
           VCPU      make a vCPU in it
  vcpu     RUN       enter the guest on it
           REGS      set its registers (reading them is READ)
           IRQ       make a vector pending in it
```

Sockets are where this was actually missing rather than merely coarse.
`bind`, `listen`, `connect` and `shutdown` required **no right at all**:
a socket handed to another process as read-only could still be pointed
at a different peer or shut down by the receiver, which is most of what
holding a socket is worth. The vCPU objects had the opposite problem --
every mutating call needed WRITE and nothing finer, so "may run this
guest" could not be given without also giving "may rewrite its
registers".

**A type right is checked on its own**, not on top of a generic one.
The type bit already names the exact operation, and requiring MANAGE as
well would bundle authorities that have nothing to do with each other:
there would be no way to say "may accept connections" without also
saying "may reconfigure the socket". Data movement keeps its generic
meaning -- `vm_mem_rw` is READ and WRITE because guest memory is
contents, and reading a vCPU's registers is READ for the same reason.

**The same bit means different things on different types, and that is
safe because a per-type right is only ever tested by code that has
already established the type.** `sock_of`, `vm_of` and `vcpu_of` each
convert the object and refuse anything of another kind, so bit 18
(CONNECT on a socket, IRQ on a vCPU) cannot be spent on the wrong
object: the conversion fails before the operation runs.

**What `accept` returns carries what a connection can use.** The new
handle gets READ, WRITE, the owner rights and SHUTDOWN -- not BIND,
ACCEPT or CONNECT, which are meaningless on an established connection.
Granting rights that name impossible operations costs nothing and says
something false.

### Leaving a table

An object that lives in a lookup table (the vnode hash, the process
table) must leave it in the same act that takes its count to zero, under
the lock a lookup holds -- or the table will, for a moment, hold a dead
object, and a lookup in that moment either takes a reference on it (a
panic in `kobject_get`) or hands out something whose release is already
running. `kobject_put_and_lock` is that act: it drops the count without
the lock while more than one reference remains and takes the lock only
for the drop that reaches zero, returning with the lock held so the
caller can unlink; `kobject_release_final` then runs the release. The
alternative -- read the count, decide, then drop -- is three steps, and
two holders dropping from 2 both read 2. The vnode cache did exactly
that until `vfs-put-race` reproduced it (`docs/kernel-services/vfs/design.md`,
"Vnode cache").

## Non-responsibilities

- Global object namespace, object naming, capability transfer over IPC
  messages, per-object permissions beyond handle rights (Phase 5
  security work builds on this). Transfer at creation time exists
  (`spawn`'s handle map).
- Dynamic growth of the handle table.

## Data structures and rules

- Ownership: whoever creates an object holds the initial reference;
  installing it in a table takes another; lookup takes one for the
  caller; close drops the table's.
- Lifetime: `release` runs exactly once, from the last `kobject_put`,
  in the context of whoever dropped the last reference. Releases must
  be callable from any thread context; they may block (they run outside
  spinlocks).
- Synchronisation: refcounts are atomic; the handle table lock protects
  slot contents only; object-internal state is the object's own
  concern.
- Destruction: `handle_table_destroy` closes every slot as soon as the
  process's last thread is gone (`process_last_thread_gone`), before the
  zombie is reaped; the process release finds the table empty.
- Handle representation: `int`, 0 to 63, `-EBADF` for anything else.

## Error handling

`handle_install` returns `-EMFILE` when full. `handle_lookup` returns
NULL for empty slots and for insufficient rights (the caller maps both
to `-EBADF` or `-EPERM` as it sees fit; the native syscalls use
`-EBADF`).

## Security

Rights are checked at lookup, not at use; an object reached through a
read-only handle cannot be written even if the object supports writing.
The table is per process; a handle number means nothing in another
process.

## Testing

Self-test `objects`: refcount get/put with a counting release, install
until full (`-EMFILE`), lookup with and without rights, close, destroy
releasing everything. See `docs/kernel/process/testing.md`.

## Future

Devices, timers, shared memory, IPC channels, VMs and vCPUs join files,
sockets and pipe ends as kobjects with handle rights; capability passing
over IPC transfers references the way `spawn` does at creation.
