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
- `struct kobject_type`: name and `release`; subtypes extend it (the
  console type, since Phase 7 `struct file`, since Phase 8
  `struct socket`, and since Phase 9 the two pipe ends add `read`/`write`
  and an optional `stat` through `kobject_io_type`). Other kobjects
  today: `struct vnode` and `struct mount` (`kernel-services/vfs/`),
  `struct device` (`kernel/device/`), `struct blkdev` (`kernel/block/`),
  `struct process`.
- `struct handle_table`: fixed 64 slots, spinlock, install/lookup/close,
  `handle_get` (object plus rights, for `dup` and `spawn`), destroy when
  the process's last thread is gone.
- Rights: READ and WRITE now; the set grows with the object kinds.
  Since Phase 9 handles cross process boundaries: `spawn` copies chosen
  handles with their rights into the child (a capability-style map), and
  `dup` copies within a process.

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
