# Kernel Object Model and Handles: Invariants

Rules `kernel/object/` and its callers keep. Each has a **Check** (what
verifies it today) and, where honest, a **Gap**. Changing a rule means
changing this file and the code together.

Several rules about objects and handles were written down in other
subsystems' files before this one existed. They are not restated here
under new numbers:

| Rule | Where | Subject |
|---|---|---|
| Q9, Q9a | `docs/kernel/quiesce/invariants.md` | every type has a release that frees the object; a refused registration leaves no kobject or owner count |
| Q10 | same | a registry's reference is distinct from the creator's |
| Q14 | same | a socket woken after a protocol lock is dropped is held with `kobject_tryget` |
| Q15, Q16 | same | a module outlives every object whose release it holds; `owner` is set at init or `kobject_track_code` and dropped after the release |
| Q17 | same | the handle table takes references under its lock and drops them outside it |
| P16, P17, P18, P24 | `docs/kernel/process/invariants.md` | no kernel pointer reaches user space; rights checked at lookup; handles 0–2 are the console; handles close when the last thread is gone |
| S9 | `docs/kernel/security/invariants.md` | rights only ever shrink; the `exiting` gate for lookup, get and both installs |
| A9 | `docs/kernel/io/invariants.md` | an epoll registration lives exactly as long as some descriptor to its member |
| L17 | `docs/compat/linux/invariants.md` | close-on-exec is set in the installing hold; exec closes exactly the marked slots |
| N12 | `docs/kernel-services/network/invariants.md` | a socket handle honours its rights and its object type |
| I5 | `docs/kernel/ipc/invariants.md` | pipe rights decide direction before the object does |
| U1 | `docs/userland/invariants.md` | a child receives exactly the handles its parent maps |

**OB1. A reference count never rises from zero and never goes below
it.** `kobject_get` and `kobject_get_n` panic when the value they
incremented was 0; `kobject_tryget` refuses (returns false) at 0 by
compare-and-swap instead of incrementing; `kobject_put` and
`kobject_put_and_lock` panic on a drop from 0; `kobject_release_final`
panics on a nonzero count. Every one is a `panic`, present in every
build. Check: the panic paths themselves. `objects` asserts the count
through 1 → 2 → 1 → 0. Gap: no test drives any of the panic paths.

**OB2. `type->release` runs exactly once per object, from the drop that
reached zero, on the dropping thread.** Only `kobject_release_final`
calls it, and only `kobject_put` (on an old value of 1) and a caller of
`kobject_put_and_lock` that got `true` call that. An object that was
never published is the one exception: its creator may undo it with
`kobject_discard` (count 1) and free it, and no release runs. The object
census (`testing.md`) checks that every object initialised is released
or discarded. Check: `objects`
(`released` is 0 after the first of two puts and 1 after the second;
after `handle_table_destroy` and the creator's put, 1 again);
`blk-lifetime`, `net-netif-lifetime` and `module-unload-busy` count
their releases (Q9, Q16).

**OB3. An object that a lookup can find under a table lock reaches
zero either under that lock or behind `kobject_tryget`.** Either the
drop to zero happens under the lock the lookup holds and the object
leaves the table in the same hold (`kobject_put_and_lock`, then
`kobject_release_final` after unlocking: `vnode_put`, the vnode hash
under `mnt->lock`), or the lookup takes its reference with
`kobject_tryget` and treats a refusal as "not found" (`process_lookup`,
TCP `sock_ref`, `udp_input`, `kernel/ipc/unix.c`,
`kernel-services/vfs/pagecache.c`). A plain `kobject_get` on a table
entry is only correct under the first form. Check: `vfs-put-race`
(racers on up to eight CPUs drop the same vnode at once; skipped below
three CPUs); `net-lo-udp`/`net-lo-tcp` exercise the socket paths (Q14).
Gap: the process-table and unix paths are checked by review.

**OB4. `kobject.handles` equals the number of handle-table slots, in
every process, that hold the object.** Raised in `install_slot` and
lowered in `handle_close`, each under the holding table's lock and with
an atomic RMW (acq_rel), since two tables may hold the same object.
The close that takes it to zero is the object's last descriptor, the
only one that tells epoll (A9). Check: `epoll-close` (a `dup` keeps the
registration and the last close removes it; the object's count returns
to the test's own reference). Gap: no test reads `handles` directly.

**OB5. `watched`, once set, is never cleared, and `handle_close` reads
it only after its decrement of `handles` and a `seq_cst` fence.** The
add sets it under epoll's watch lock, fences, and reads `handles`; one
of the two sides sees the other. Check: `make litmus`
(`tests/litmus/epoll/watched.litmus`); `epoll-close`.

**OB6. `count` is the number of non-empty slots and never exceeds
`HANDLE_TABLE_SIZE`; `handle_install*` refuses at `limit`.**
`install_slot` and `handle_close` change `count` with the slot, under
the lock; `handle_table_destroy` asserts it reaches 0 (`KASSERT`).
`limit` is honoured by `handle_install` and `handle_install_flags`
only: `handle_install_at*` (a `dup2`, the standard handles of a new
process) checks the slot, not the limit, and lowering `limit` below
`count` closes nothing. Check: `objects` (63 more installs after the
first fill the table: `count` 64, the next `-EMFILE`; `count` 0 after
destroy); `rlimit` (with `limit` 2 the third install is `-EMFILE` and
a close makes room; at 64 the 65th is `-EMFILE`); `process-rlimit`
(the `rlimit-root` probe: `pipe` is `-EMFILE` at a limit of 4 with
three handles open).

**OB7. After `exiting` is set nothing new enters or leaves the table
except by `handle_close`.** S9 states the rule for lookup, get and both
installs; it also holds for `handle_get_flags` (NULL) and
`handle_set_flags` (`-EBADF`), which test `exiting` the same way.
`handle_close` deliberately does not, because destroy itself closes
through it. Check: `objects` (after destroy: `handle_lookup`,
`handle_get`, `handle_install` and `handle_install_at` all fail).
Gap: no test calls `handle_get_flags` or `handle_set_flags` on an
exiting table.

**OB8. A slot is empty before anything its object does at close runs,
and the handle is closed whatever that returns.** `handle_close` clears
`obj`, `rights` and `flags` and drops `handles` in one hold; the epoll
notice, `flush` and the put follow outside the lock; `flush`'s errno is
`close`'s result. Check: `wb-error-close` (close reports `-EIO` and the
handle is closed); `objects` (a second close is `-EBADF`, not a panic).

**OB9. A per-type right (bits 16..31) is tested only after the object's
type is established.** `sock_of_err` (`kernel/syscall/native.c`),
`vm_of_err` and `vcpu_of_err`
(`kernel-services/virtualization/hvsys.c`) convert the object first and
answer `-EBADF` for another kind, then `-EPERM` for a missing right.
Check: the user-mode suite's `net` section (copies each missing one of
BIND, CONNECT, ACCEPT, SHUTDOWN get `-EPERM` for exactly that
operation); `process-rlimit`'s `rlimit-root` probe (when `/dev/vmm`
opens: a VM handle without `VM_MAP` or `VM_VCPU`, and a vCPU handle
without `VCPU_REGS`/`VCPU_IRQ`, get `-EPERM`); N12 for the type check.

**OB10. A plain kobject (a type without `KOBJECT_TYPE_IO`) is never read
as a `struct kobject_io_type`.** `kobject_io_of` returns NULL for it;
`syscall_obj_read`/`syscall_obj_write` answer `-EBADF`, `kobject_ready`
0, `kobject_poll_wq` NULL, `kobject_set_nonblock` `-EOPNOTSUPP`, and
`handle_close` skips `flush`. Check: review. Gap: no test performs I/O
on a vCPU handle.

**OB11. An object's `read` or `write` never reports more than it was
offered.** `syscall_obj_read` and `syscall_obj_write` bound each object
call by the bounce chunk `n` and `KASSERT(rc <= (int64_t)n)`; the
assertion panics in every build, so an over-reporting object stops the
kernel instead of sizing a copy. Check: the assertion. Gap: no test has
an object that over-reports.

**OB12. The console object is never released.** `g_console` starts with
a reference nobody puts; tables add and drop their own around it;
`console_obj_release` panics. Check: every boot (handles 0–2 of every
kernel-created process are the console, P18, and closing them all at
exit would reach the panic if the static reference were missing);
`rlimit` installs it 64 times and destroys the table.
