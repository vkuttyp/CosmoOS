# Security: invariants (access control and resource limits)

Rules milestone 6 adds on top of the credential and permission rules
recorded in `docs/kernel/process/invariants.md` (P26a, P28) and
`docs/kernel-services/vfs/invariants.md` (V14). Each names its check.

**S1. Privilege flows down.** A process's effective uid becomes 0 only by
inheritance from a privileged parent, either by copy or through
`COSMO_SPAWN_SETCRED` from a privileged caller. `setresuid` refuses ids
the caller does not hold (P26a); `SETCRED` from an unprivileged caller
refuses a uid or gid outside its real, effective and saved set
(`may_set_cred`, `spawn.c`); the executable's setuid and setgid mode
bits are never consulted. Check: `process-rlimit` (`rlimit-unpriv`:
`spawnve_as(…, 0, 0)` and `(1000, 0)` are `-EPERM`, `(1000, 1000)`
succeeds and the child has no groups), `init --unpriv-test`, review of
`process_create_from_elf` (the only writer of a new process's ids).

**S2. A `SETCRED` child starts clean.** Its real, effective and saved
ids are the named ones and `ngroups` is 0, whatever the caller held.
Check: `init --probe uid-is:N` (`getuid`, `geteuid`, `getgid` equal `N`,
`getgroups(0)` is 0) run by `process-rlimit`.

**S3. Limits are inherited, lowered by anyone, raised only with
privilege.** `struct rlimits` is copied at spawn (also with `SETCRED`);
`process_setrlimit` compares against the current value under
`process.lock` and returns `-EPERM` for a raise by an unprivileged
caller; `NOFILE` above the table size is `-EINVAL`. Check:
`process-rlimit` (both probes), `lxtest` (`setrlimit` with `cur > max`
is `-EINVAL`, `prlimit64` on another pid is `-EPERM`).

**S4. Every limit binds where the resource is granted, with a clean
error.** `AS`: `vm_user_map_anon` `-ENOMEM` before any change; `MEM`:
a populated map `-ENOMEM` after unwinding, a demand-zero fault treated as
an allocation failure (fatal to a user touch, `-EFAULT` in a copy,
docs/kernel/memory/design.md §6.1); `NOFILE`: `handle_install` `-EMFILE`
(`handle_install_at`, used for a new process's 0–2, is not bounded);
`NPROC`: `process_create_from_elf` `-EAGAIN`, decided under the
process-table lock in the same critical section that publishes the
process, so two spawns near the limit cannot both pass on a stale count; `VMEM`: `vm_mem_add` `-ENOMEM` against `vm->mem_limit`,
recorded from the creator at `vm_create`. Check: `rlimit` (VMM and
handle table on private objects), `process-rlimit` (every resource from
user mode, the memory limit ending the toucher with status 139),
`hv-npt` (a VM created with a 8 KiB cap refuses 12 KiB), `lxtest`
(`EMFILE` at the eighth handle), `process-nproc` (two kernel threads
spawn sixteen children of one uid under a limit of four while a third
samples the count: the peak is exactly four).

**S5. Lowering a limit below the current use changes nothing already
granted.** `vm_space_set_limits` and `handles.limit` only gate growth;
regions, frames and handles stay. Check: `rlimit` (three regions survive
a limit of one page), review.

**S6. A ramfs mount never holds more than its page budget.** `struct
mount.cache_pages` counts every cached page of the mount; a miss
*reserves* its page with one atomic increment and refuses with `-ENOSPC`
when the result exceeds `cache_limit_pages` (the increment is the
admission, so concurrent misses on different vnodes cannot both pass a
stale read); every later failure of the miss returns the reservation,
and `remove_entry`/`pagecache_drop` return it when the page goes. ramfs
mounts get `RAMFS_MAX_PAGES` (16 384). Check: `cache-limits` (a budget
of four pages: the fifth page `-ENOSPC`, a second file refused too,
unlink frees the budget), `cache-budget-race` (two writers fill and free
files on a four-page mount for forty rounds while a sampler watches the
count: the peak is exactly four and the count returns to zero), review
of the counter's paths.

**S7. The global page-cache limit reclaims only what can be rebuilt.**
Only clean pages of mounts without `MOUNT_CACHE_IS_STORE` are ever on
the LRU (`lru_add` checks the flag; dirtying removes the page;
`pagecache_sync` re-adds it); reclaim never frees a dirty page, never
touches a ramfs page, and never waits on a cache lock while another is
held (`mutex_trylock`, `kobject_tryget`; busy or dying vnodes are
skipped). Over the limit with nothing clean, the cache grows: the limit
is soft until writeback exists (milestone 7). Check: `cache-limits`
(a 2 MiB cosmofs file read back under a limit below its size: pages
reclaimed, contents intact, the root ramfs's page count unchanged; with
the limit at one page a dirty write stays cached and reads back),
lockdep (the trylock takes no class edge), review.

**S8. Information gates.** `klog` is privileged; `procinfo` returns to an
unprivileged caller only processes whose real uid is its own; `log`
from an unprivileged process is limited to a bucket of 64 lines
refilled at 16 per second (`-EAGAIN` beyond). Check: `init --unpriv-test`
(`klog` `-EPERM`), `process-rlimit` (`rlimit-unpriv`: every `procinfo`
record has uid 1000; of 80 quick `log` calls at least 16 succeed and at
least one is `-EAGAIN`). Gap: `sysctl` values are world-readable by
design (none carries a kernel address); the kernel's fault log line for
a process names the user address only.

**S9. A handle says what may be done with it, and only ever says less.**
Rights live on the handle, not the object: READ, WRITE, DUP, TRANSFER
and MANAGE in the generic vocabulary, with bits 16..31 belonging to each
object's type (docs/kernel/object/architecture.md, "Rights"). `dup`
needs DUP, the `spawn` map needs TRANSFER, and both may hand over a
subset of what the caller holds — never more, and there is no operation
anywhere that adds a right to a handle that already exists. Creating an
object grants its creator the access rights that suit it plus
`HANDLE_RIGHT_OWNER`, so giving something away is always a deliberate
act. Administering an object is separate from using it: `setnonblock`
needs MANAGE.

Two answers are deliberately different. The capability operations say
`-EPERM` when a handle exists and does not carry the right — that is
what `handle_lookup_rights` reports — while `read` and `write` keep
saying `EBADF`, which is what POSIX says of a descriptor that is not
open for that direction. The rights layer adds vocabulary; it does not
change what the calls that predate it answer.

A table stops answering before it is torn down. `handle_table_destroy`
raises an exiting flag under the table lock and only then empties the
slots, so a thread still inside a syscall can neither be handed a
reference the exit is releasing nor put one back for nobody to close.
Check: `objects` (rights tell "no such handle" from "not through this
handle"; after destroy, lookup, get and both installs all fail), and the
user-mode self-test (a copy with only READ cannot be written, copied,
transferred to a child or administered, while a copy that keeps DUP and
TRANSFER can still be passed on and is no wider than its parent).

**S10. A process cannot name anything outside its root.** A process has
a root, inherited from its parent and defaulting to the global one.
Every absolute path starts there, and `..` stops there exactly as it
stops at the global root — those are the only two ways a path can climb,
and both are closed. `vfs_current_root` supplies it to the VFS the way
`cred_current` supplies credentials: the caller's context is asked for,
not threaded through every entry point.

A root is set only at `spawn`, with `COSMO_SPAWN_SETROOT`, and the path
is resolved **in the caller's own namespace** — so a process already
confined can only name a directory inside its own root, and confinement
tightens but never loosens. It is privileged, like setting credentials:
a process that could root itself anywhere could root itself at a
directory whose contents it chose. Privilege flows down and never up.

The executable is found in the *caller's* namespace before the child
exists, so a confined child needs no copy of its own program — but
anything it runs afterwards it must find inside its root, which is why
a shell in a jail can use its builtins and not `/bin/echo`. Check: the
user-mode self-test (a child rooted at a directory reports `/` for `pwd`
after `cd ..`, writes through an absolute path into that directory as
seen from outside, and exits nonzero when it tries to reach a directory
that exists only outside).
