# Kernel Object Model and Handles: API

All interfaces are kernel-internal (**ABI stability: internal**); user
space sees only handle integers through the system-call layer. Each
entry follows constitution section 52.

## Objects (`kernel/include/kernel/object.h`, `kernel/object/object.c`)

### `struct kobject { const struct kobject_type *type; uint32_t refcount; }`
Embedded as the first or any member of a concrete object; recover the
container with `container_of`. The count is manipulated only through
the functions below.

### `struct kobject_type { const char *name; void (*release)(struct kobject *); }`
Static, immortal type descriptor. `release` runs from the last put,
in the putter's context, and may block; it must free or otherwise
retire the object. Subtypes embed it as `base` and add operations
(`struct kobject_io_type` adds `read`, `write` and, since Phase 9, an
optional `stat`). Contract for `read(obj, buf, len)` / `write(obj, buf,
len)`: return the bytes transferred, `0 <= count <= len`, or a negative
errno; a NULL operation means the object does not support that
direction (`-EBADF` from the system call). `stat(obj, struct cosmo_stat
*st)` fills the record (0 or a negative errno); `sys_fstat` uses it for
every object that is not a `struct file`, and returns `-EBADF` when it
is NULL. I/O kobjects today: the console (`read`/`write`/`stat`),
`struct file` (`kernel-services/vfs/`), `struct socket`
(`read`/`write`, no `stat` yet), the pipe ends `pipe-read`
(`read`/`stat`) and `pipe-write` (`write`/`stat`) (`kernel/ipc/`). The system call
layer bounds every copy by `len`, not by the returned count: a count
above `len` trips a `KASSERT` and fails the call with `-EIO`, so a
buggy object can never make the kernel read past its stack buffer.

### `void kobject_init(struct kobject *obj, const struct kobject_type *type)`
- Purpose: set the type and a reference count of 1 owned by the caller.
- Failure: `KASSERT` on a NULL type or NULL `release`.
- Concurrency: none needed; the object is not yet shared.

### `void kobject_get(struct kobject *obj)`
- Purpose: take a reference; the caller must already hold one (or a
  lock that keeps the object alive, as `handle_lookup` does).
- Concurrency: atomic acq_rel; usable in interrupt context and under
  spinlocks. Panics if the count was 0 (use after release).

### `void kobject_put(struct kobject *obj)`
- Purpose: drop a reference; runs `type->release` when it was the last.
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
