# Kernel Object Model and Handles: API

All interfaces are kernel-internal (**ABI stability: internal**); user
space sees only handle integers through the system-call layer. Each
entry follows constitution section 52.

## Objects (`kernel/include/kernel/object.h`, `kernel/object/object.c`)

### `struct kobject { const struct kobject_type *type; uint32_t refcount; struct module *owner; }`
Embedded as the first or any member of a concrete object; recover the
container with `container_of`. The count is manipulated only through
the functions below. `owner` is the module whose code the release lives
in (NULL for the kernel), recorded so the module cannot be unloaded
while the object exists (`docs/kernel/quiesce/design.md`, "Module
unload"). Adding the field changed the layout of every exported
structure that embeds a kobject (`struct device`, `struct blkdev`,
`struct netif`): module ABI v2.

Lifetime rules every type follows (`docs/kernel/quiesce/invariants.md`
Q9–Q10): the release frees the memory and nothing else does; the creator
holds reference 1; a registry that hands the object out by lookup holds
its own reference and drops it on unregister; lookups return referenced
pointers.

### `struct kobject_type { const char *name; void (*release)(struct kobject *); unsigned flags; }`
Static, immortal type descriptor. `release` runs from the last put,
in the putter's context, and may block; it must free or otherwise
retire the object. `flags` carries `KOBJECT_TYPE_IO` when the
descriptor is really a `struct kobject_io_type`; a type without it (a
vcpu) is a plain object and every I/O path answers `-EBADF` for it
instead of reading operations past the end of the descriptor. Subtypes
embed it as `base` and add operations (`struct kobject_io_type` adds
`read`, `write`, since Phase 9 an optional `stat`, since milestone 8 the
optional `ready` and `set_nonblock`, and since milestone 9 the optional
`poll_wq`). Contract for `read(obj,
buf, len)` / `write(obj, buf, len)`: return the bytes transferred,
`0 <= count <= len`, or a negative errno; a NULL operation means the
object does not support that direction (`-EBADF` from the system
call). `stat(obj, struct cosmo_stat *st)` fills the record (0 or a
negative errno); `sys_fstat` uses it for every object that is not a
`struct file`, and returns `-EBADF` when it is NULL. `unsigned
ready(obj)` returns the `COSMO_IO_READABLE` (1) / `WRITABLE` (2) /
`HANGUP` (4) / `ERROR` (8) bits describing what would not block now,
from any thread context without blocking; NULL means always readable
and writable. `int set_nonblock(obj, int on)` switches the object's
non-blocking mode (0 or 1; -1 only asks) and returns the previous mode;
NULL means the object never blocks and the switch is `-EOPNOTSUPP`. The
mode is a property of the object, shared by every handle to it (there
is no open-file-description layer between a handle and its object).
`struct waitqueue *poll_wq(obj, unsigned events)` returns the queue a
waiter for `events` sleeps on, woken whenever `ready` may have changed
for those bits; NULL means readiness never changes (a file). The
asynchronous I/O ring (`docs/kernel/io/`) is built on `ready` and
`poll_wq` alone.
I/O kobjects today: the console (`read`/`write`/`stat`/`ready`: readable
with a complete tty line, always writable), `struct file`
(`kernel-services/vfs/`; no `ready`: always ready), `struct socket`
(`read`/`write`/`stat`/`ready`/`set_nonblock`), the pipe ends
`pipe-read` (`read`/`stat`/`ready`/`set_nonblock`) and `pipe-write`
(`write`/`stat`/`ready`/`set_nonblock`) (`kernel/ipc/`), all four with
`poll_wq` since milestone 9, the I/O ring `aio` (`ready`/`poll_wq` only;
`kernel/io/`), and
since Phase 12 `struct vm` (`read` drains the guest's debug console,
`write` `-ENOTSUP`, `stat` `COSMO_DT_CHR`; `kernel-services/virtualization/`),
whose companion `struct vcpu` is a plain kobject. The system call
layer bounds every copy by `len`, not by the returned count: a count
above `len` trips a `KASSERT` and fails the call with `-EIO`, so a
buggy object can never make the kernel read past its stack buffer.

### `const struct kobject_io_type *kobject_io_of(const struct kobject *obj)`
The object's io type, or NULL for a plain object (or NULL `obj`).
`syscall_handle_read/write/stat`, `sys_ioready` and `sys_setnonblock`
go through it.

### `struct waitqueue *kobject_poll_wq(struct kobject *obj, unsigned events)`
The `poll_wq` operation with its default: NULL for a plain object or a
type without it.

### `unsigned kobject_ready(struct kobject *obj)`, `int kobject_set_nonblock(struct kobject *obj, int on)`
The `ready` and `set_nonblock` operations with their defaults: a plain
object is never ready and cannot be switched (`-EOPNOTSUPP`); an io type
without `ready` is `READABLE|WRITABLE`; one without `set_nonblock` is
`-EOPNOTSUPP`. System calls `ioready` (58) and `setnonblock` (59)
expose them (`docs/kernel/syscall/api.md`).

### `void kobject_init(struct kobject *obj, const struct kobject_type *type)`
- Purpose: set the type and a reference count of 1 owned by the caller;
  record the owner module of `type->release` (`module_owner_of`, which
  raises that module's live-object count inside a read-side section).
- Failure: `KASSERT` on a NULL type or NULL `release`.
- Concurrency: none needed for the object; the owner lookup is a
  `quiesce_read_lock` section, any context.

### `void kobject_track_code(struct kobject *obj, uintptr_t code)`
- Purpose: for types whose release trampolines to a per-object callback
  (`struct device.release`, `struct blkdev_ops.release`,
  `struct netif_ops.release`), record the callback's owner module once
  it is known (`device_register`, `blk_register`, `netif_register`).
  Releases the owner recorded by `kobject_init` if it differs.

### `bool kobject_tryget(struct kobject *obj)`
- Purpose: take a reference unless the count is already zero; the form
  for a table that a release path clears under a lock the looker holds
  (TCP `sock_ref`, `udp_input`), where the object may be mid-release.
- Concurrency: CAS loop, acq_rel; any context.

### `void kobject_get(struct kobject *obj)`
- Purpose: take a reference; the caller must already hold one (or a
  lock that keeps the object alive, as `handle_lookup` does).
- Concurrency: atomic acq_rel; usable in interrupt context and under
  spinlocks. Panics if the count was 0 (use after release).

### `void kobject_put(struct kobject *obj)`
- Purpose: drop a reference; runs `type->release` when it was the last,
  then drops the owner module's live-object count (so the release runs
  with its module still mapped).
- Concurrency: atomic; the drop itself is interrupt-safe, but because
  `release` may block, a put that might be the last one must not be
  made under a spinlock or in interrupt context. `handle_close` and
  `thread_put` obey this by unlocking first. Panics on underflow.

### `uint32_t kobject_refcount(const struct kobject *obj)`
Acquire load; for tests and diagnostics only.

### `struct kobject *console_object(void)`
- Purpose: the single console object (`kernel/object/console_obj.c`),
  type name `"console"`, statically allocated with refcount 1 that is
  never dropped; its `release` panics.
- Ownership: the caller must `kobject_get` before installing it in a
  table (`handle_install` does this itself).
- Operations: `write(obj, buf, len)` calls `console_write` and returns
  `len` (takes the console sink lock, may spin, never blocks); `read`
  calls `tty_read(tty_console(), buf, len)` and blocks until a line has
  been typed (`docs/kernel/tty/api.md`; `-EINTR` when the process is
  killed); `stat` reports `COSMO_DT_CHR`, mode 0620, one link. `buf` is
  kernel memory (the syscall layer bounces user data).

## Handle tables (`kernel/include/kernel/handle.h`, `kernel/object/handle.c`)

`struct handle_table`: spinlock, `HANDLE_TABLE_SIZE` (64) entries of
`{ struct kobject *obj; unsigned rights; }` with `NULL` meaning free,
and a live count. Embedded in `struct process`. Rights:
`HANDLE_RIGHT_READ` (1), `HANDLE_RIGHT_WRITE` (2), `HANDLE_RIGHT_ALL`.

Common properties: the spinlock is taken `irqsave`, so every function
is safe with interrupts disabled; none allocates; only
`handle_close`/`handle_table_destroy` can block (last put).

### `void handle_table_init(struct handle_table *t)`
Zero the slots, count 0, initialise the lock. Once, before any use.

### `int handle_install(struct handle_table *t, struct kobject *obj, unsigned rights)`
- Purpose: take a reference on `obj` and store it in the lowest free
  slot.
- Outputs: the handle (0..63) or `-EMFILE` (reference dropped again).
- Concurrency: reference taken before the lock, so `obj` must be alive
  on entry (`KASSERT obj != NULL`).

### `int handle_install_at(struct handle_table *t, int h, struct kobject *obj, unsigned rights)`
- Purpose: store at slot `h` (used for handles 0–2 of a new process).
- Outputs: `h`; `-EBADF` if out of range; `-EBUSY` if occupied.

### `struct kobject *handle_get(struct handle_table *t, int h, unsigned *rights_out)`
- Purpose: translate a handle to a referenced object *and* its rights,
  without demanding any right; for `dup` and for copying a parent's
  handles into a child at `spawn`.
- Outputs: the object with one new reference and `*rights_out` set, or
  NULL when `h` is out of range or free.
- Concurrency: non-blocking, interrupt-safe.

### `struct kobject *handle_lookup(struct handle_table *t, int h, unsigned rights_needed)`
- Purpose: translate a handle to a referenced object.
- Outputs: the object with one new reference, or NULL when `h` is out
  of range, free, or lacks any bit of `rights_needed`. The caller must
  `kobject_put` when done; the reference keeps the object valid even if
  the slot is closed concurrently.
- Concurrency: non-blocking, interrupt-safe.

### `int handle_close(struct handle_table *t, int h)`
- Purpose: free the slot and drop the table's reference.
- Outputs: 0; `-EBADF` if out of range or free (double close is an
  error, never a panic).
- Concurrency: the put happens after the lock is released, so the
  caller must be in a context where a release may block (thread
  context, no spinlock held).

### `void handle_table_destroy(struct handle_table *t)`
Close every slot; asserts the count reaches 0. Called from
`process_last_thread_gone` (reaper context) as soon as the last thread
of the process is gone, so handles close at exit rather than when the
zombie is reaped (a pipe's reader sees end of file before the parent
waits); `process_release` calls it again on the empty table. The table
must not be in use by any thread of the process. May block (the last
put of a file, socket or pipe end).

### `unsigned handle_table_count(struct handle_table *t)`
Live handle count under the lock; for tests and `process_dump_all`.

## Adding an object kind

1. Define a `struct my_type { struct kobject_type base; ... ops }` with
   an immortal descriptor and a `release` that frees the container.
2. Allocate the container, `kobject_init(&c->obj, &my_type.base)`; the
   creator holds the first reference.
3. Hand it to a process with `handle_install` (which takes its own
   reference) and `kobject_put` the creator's reference when it no
   longer needs direct access. Give it a `stat` operation if `fstat`
   should describe it.
4. In a syscall, `handle_lookup` with the rights the operation needs,
   downcast via `obj->type`, call the op, `kobject_put`.
