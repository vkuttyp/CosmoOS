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

The root is tested before `..` crosses a mount, not after: a process
rooted at a mounted filesystem stands on that filesystem's root vnode,
and the crossing replaces it with the covered vnode beneath, which is
not equal to it. A rooted child starts *at* its root rather than
inheriting the caller's
working directory, because a child standing outside its own root escapes
through any relative path without trying; a root and an explicit working
directory are refused together, since the cwd would have to be resolved
in the child's namespace to know it is inside.

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
before touching its working directory at all, and again after `cd ..`, writes through an absolute path into that directory as
seen from outside, and exits nonzero when it tries to reach a directory
that exists only outside; and a child rooted at a *mounted* filesystem
cannot climb out through it -- checked by trying to enter a directory
that exists only outside the mount, since `pwd` alone cannot tell an
escape from confinement when both print `/`).

**S11. A process sees and signals only its own domain, unless it is in
the one the system booted in.** Every process belongs to a domain,
inherited from its parent; a spawn may start a new one, and nothing
leaves one. `procinfo` skips other domains entirely — invisible rather
than merely unreadable — and a signal to another domain is `-ESRCH`
rather than `-EPERM`, because refusing with "not permitted" would
confirm the pid exists, which is what the domain is meant not to tell.
Domain 0 sees and signals everything, subject to the credential rules
that already applied: a host must be able to manage what it started, and
what it started must not reach back. The process that starts a domain
reports no parent, since its real parent is outside it.

Domain identifiers are allocated from 1, monotonically, and are never
reused: reusing one would give a second set of processes the identity of
a live domain. Exhaustion is `-ENOSPC` rather than a wrap, because
wrapping would eventually hand out 0 — the domain the system boots in,
which sees everything. Four billion domains is not a number this will
reach, which is exactly the reasoning that produces such bugs.

This is deliberately *not* a pid namespace: pids are not renumbered, and
the doc says so rather than implying otherwise. Nothing outside the
domain is nameable, so the number tells a confined process nothing.
Check: the user-mode self-test runs `ps` inside a domain and requires
its own shell to be listed and `init` — pid 1, which certainly exists —
not to be; and runs `kill 1` inside a domain and requires it to fail.
The visibility check was confirmed against the bug: with the filter
removed, `ps` in the domain lists `init` and the assertion fails.

**S12. A process sees the mounts of its namespace, and mounts into no
other.** A process belongs to a mount namespace; a spawn may start a new
one, which begins as a copy of the parent's view and diverges. A mount
made afterwards is visible only where it was made, and an unmount only
removes it from the namespace that asked; the last namespace to see a
mount is the one that unmounts it, so nothing is released while anyone
can still reach it and nothing is left behind that nobody can.

This is the other half of the root (S10). A root says which subtree a
process may name; a namespace says what is attached inside it. A
confined process without one still shares the mount table, so what it
mounts is system-wide and what is mounted system-wide appears under its
root — the confinement holds for names and leaks through mounts.
Starting a namespace is privileged, like a root and a domain: privilege
flows down.

Check: the user-mode self-test puts a file in a directory and then reads
that directory from both sides of a split, since whether the file is
listed says which side the listing came from. A child spawned into a new
namespace mounts a ramfs over the directory and lists only its own empty
mount, while this side still finds the file and can mount over the
directory itself; and a child that waits until this side has mounted
still lists the file, because the mount came after its namespace was
copied. An unprivileged spawn asking for a namespace is refused.

Confirmed against the bug: with the visibility filter defeated, the
waiting child lists an empty directory and the assertion fails.

**S13. A process reads the name of its own container, and only a
privileged one writes it.** The hostname lives in a uts namespace; a
spawn may start a new one holding a copy of the caller's name, and
setting it there is invisible outside. `gethostname`, `uname` and
`sysctl kernel.hostname` all answer from the caller's namespace, so
there is no path by which one of them reports the host's name to a
process that the others tell otherwise. Setting requires privilege:
a name is not a secret, but a process that could rename the machine
could make another one's logs say whatever it liked.

The name is bounded (`COSMO_HOST_NAME_MAX`, 64 including the
terminator) and copied under the namespace's lock into a local buffer
before it reaches user space, so a concurrent `sethostname` cannot be
observed half-written.

Check: the user-mode self-test has a child in a new namespace rename
itself and requires the parent's name to be unchanged afterwards, and
requires the child to read back its own new name rather than the
parent's; the kernel self-test requires a namespace made *after* a
rename to carry the name current at that moment and to be unaffected by
later ones; and the unprivileged-process test requires `sethostname` to
be refused. Confirmed against the bug: with the namespace ignored so that every
process reads the initial one, the child's rename reaches this side and
two assertions fail — the name here changed, and a child *without* a
namespace no longer reports the name this side still believes it has.

**S14. A process's system calls only ever get fewer.** A filter is a
bitmap of allowed system-call numbers; installing one intersects with
what is already in force, so no sequence of calls widens what a process
may do. Children inherit it, because a child that could shed its
parent's filter would make the filter one spawn away from meaningless.
A denied call kills the process with `SIGSYS` rather than returning an
error: a filter says what the program will ever need, so a call outside
it is a bug or an exploit, and neither should be allowed to continue
into a state the author never tested.

Installing one needs no privilege -- it is the one primitive here that
only ever takes authority from the caller, and a process that could not
restrict itself would be unable to do the one safe thing it can do
without asking. Three things stay allowed whatever the mask says:
`exit`, and for the Linux personality `exit_group` and `rt_sigreturn`,
without which a clean shutdown or a signal handler's return would itself
be fatal. Bits beyond the supplied mask are clear, so a program built
against a smaller system-call count denies what it has not heard of.

Check: the user-mode self-test spawns children that filter themselves
and then make a denied call, requiring each to die with status 159
(128 + `SIGSYS`); requires a child that stays inside its mask to exit
0; requires `exit` to work from a mask that does not name it; requires
a second, wider mask not to restore what the first removed; and
requires a spawned child to be killed by the filter its parent
installed. Confirmed against the bug: with the intersection replaced by
an assignment, the widening test's child survives a call its first mask
had removed.

**S15. An operation that is neither reading nor writing has a right of
its own.** A handle's upper sixteen bits name the operations its type
offers that the generic vocabulary cannot describe: for a socket
`BIND`, `ACCEPT`, `CONNECT` and `SHUTDOWN`; for a VM `MAP` and `VCPU`;
for a vCPU `RUN`, `REGS` and `IRQ`. Each is required on its own rather
than on top of a generic right, because the bit already names the
operation and demanding MANAGE as well would make "may accept
connections" inseparable from "may reconfigure the socket".

Before this, `bind`, `listen`, `connect` and `shutdown` required no
right at all: a socket passed to another process as read-only could be
pointed at a different peer or shut down by the receiver. A vCPU had the
reverse fault -- one WRITE covered running a guest, rewriting its
registers and injecting interrupts.

The same bit means different things on different types, which is safe
because a per-type right is only ever tested by code that has already
established the type: the accessors convert the object first and refuse
another kind, so a bit cannot be spent on the wrong object. What
`accept` returns carries READ, WRITE, the owner rights and SHUTDOWN, and
not the three that name things an established connection cannot do.

Check: the user-mode self-test reduces a socket handle to each of these
in turn and requires the operation it dropped to fail with `EPERM` while
the ones it kept still work, and does the same for a vCPU handle without
`REGS` and without `IRQ`. Confirmed against the bug: with the check
removed from `shutdown`, a handle explicitly stripped of `SHUTDOWN`
closes the connection and the assertion fails.
