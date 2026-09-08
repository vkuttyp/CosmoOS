# Security: design (access control and resource limits)

Audit milestone 6 (`docs/audit/2026-09-post-roadmap-audit.md` §19;
findings #6, #24 and the `klog`/`procinfo` part of #31). The Prompt #3
fix pass gave the kernel POSIX-shaped credentials (`kernel/cred.h`),
one privilege predicate, discretionary file permissions, creator
ownership of new files, `setres*` and privilege gates on `mount`,
`umount`, `klog`, `kill` and reserved ports (`docs/kernel/process/design.md`
"Credentials", `docs/kernel-services/vfs/design.md` "Permissions").
This milestone records the decision the audit asked for, adds the
missing primitive, and bounds what an unprivileged process can consume.

## 1. The privilege model: privilege flows down

**Decision.** There are no setuid executables and no capability set in
this milestone. A process is privileged if and only if its effective uid
is 0 (`cred_privileged`), and a process becomes privileged only by being
created by a privileged process. The transition primitive is `spawn`:
`COSMO_SPAWN_SETCRED` names the credentials the child starts with. A
privileged caller may name any uid and gid; an unprivileged caller only
ids it already holds (the `setresuid` rule). The child starts with real,
effective and saved ids equal to the named ones and **no supplementary
groups**. The `S_ISUID`/`S_ISGID` mode bits of an executable are stored
and ignored: `spawn` never raises the child's ids above the caller's.

Why not setuid binaries: they make every setuid program's argument
parsing, environment handling and file access part of the trusted
computing base, and the tree has one root-owned service manager (`init`)
whose job is exactly to start services with the right identity. Why not
capabilities yet: the boundary is one predicate consulted in nine
places; a capability set replaces the body of `cred_privileged` and adds
a mask to `struct credentials` without changing any call site, and the
decision of which capabilities exist should follow the container work
(§53) that needs them. Recorded as future work, not done here.

Consequences the code enforces:

- `spawn` with `SETCRED` from an unprivileged caller naming a uid or gid
  outside its real, effective and saved set is `-EPERM`.
- A child created without `SETCRED` inherits its parent's credentials
  by copy, as before.
- `procinfo` shows an unprivileged caller only the processes whose real
  uid is its own; a privileged caller sees every process. `klog` stays
  privileged. `log` (the user-to-kernel-log call) is rate limited per
  process for unprivileged callers: a token bucket of 64 lines that
  refills at 16 lines per second; over it, `-EAGAIN`.

## 1b. Per-process roots

A process has a root. Every absolute path starts there and `..` stops
there — the only two ways a path can climb out of a directory, both
closed — so a process given a root below the global one cannot name
anything outside it. This is the filesystem half of what a container
needs, and it is a primitive rather than a container: nothing here knows
what a container is.

The root test happens **before** the step that leaves a mount through
its mountpoint, and the order is the whole of it. A process rooted at a
mounted filesystem stands on that filesystem's root vnode; crossing
replaces it with the covered vnode underneath, which is a different
vnode and no longer equal to the root, so a check made afterwards never
matches and the walk climbs out of the very mount it was confined to.

A rooted child also **starts at its root**: it does not inherit the
caller's working directory. Inheriting it would leave the child standing
outside its own root, where every relative path reaches outside and
`..` climbs to the global root instead of stopping — the confinement
bypassed by doing nothing at all. For the same reason a root and an
explicit working directory are not offered together (`-EINVAL`): the cwd
would have to be resolved in the child's namespace to know it lies
inside the root, and `spawn` resolves paths in the caller's.

It is set at `spawn` alone, never on a running process, and the path is
resolved in the caller's namespace. Two consequences that are the whole
security argument:

- **Confinement only tightens.** A confined caller can only name
  directories inside its own root, so a child is confined at least as
  much as its parent, and no operation widens a root.
- **Setting one is privileged**, like setting credentials: a process
  that could root itself anywhere could root itself at a directory whose
  contents it chose. Privilege flows down and never up (§1).

The executable is found in the caller's namespace before the child
exists, so a confined child needs no copy of its own program inside its
root. What it runs *afterwards* it must find in there — a shell in a
jail has its builtins and not `/bin/echo` — which is worth knowing
before it looks like a bug.

A handle cannot be used as a path base — nothing in the system opens
relative to a directory handle — so passing a child a handle to a
directory outside its root gives it no way to name anything through it.
That is a property of the current syscall surface rather than a defence,
and an `openat` would have to be written with this in mind.

Not done here: mount namespaces (a confined process still sees the same
mount table), pid and uts namespaces, a working directory for a rooted
child, and any way to give a running process a new root.

## 1c. Process domains

A process belongs to a domain. The system boots in domain 0; a spawn may
start a new one, which the child and its descendants belong to and which
nothing leaves. A process outside domain 0 sees only its own domain in
`procinfo` and may signal only its own domain; domain 0 sees and signals
all of them.

That asymmetry is the point: a host has to be able to manage what it
started, and what it started must not be able to reach back. It is the
same shape as the root — privilege flows down — and starting a domain is
privileged for the same reason.

Two details that are choices rather than accidents:

- A signal to another domain is `-ESRCH`, not `-EPERM`. Refusing with
  "not permitted" would confirm that the pid exists, which is the one
  thing the domain is meant not to tell.
- The process that starts a domain reports **no parent**: its real
  parent is outside, and naming that pid would leak one number out of
  the thing the domain hides. That is also what a process at the top of
  a tree conventionally reports.

**This is not a pid namespace.** Pids are not renumbered: a process in a
domain sees its own real pid, not a private 1. Renumbering means a
translation at every boundary that takes or returns a pid, and the
isolation here does not need it — nothing outside the domain is
nameable, so nothing is learned from the number. Saying which of the two
this is matters more than the word.

Identifiers come from a counter that starts at 1 and never wraps or
repeats: a reused identifier would merge two sets of processes into one
domain, and a wrapped one would eventually be 0, which is the host's.
Exhaustion refuses the spawn.

Not done here: pid renumbering, a domain-scoped `/proc` (there is no
`/proc` yet), and any accounting per domain.

## 1d. Mount namespaces

A process belongs to a mount namespace: the set of mounts it can see. A
spawn may start a new one, which begins as a copy of the parent's view
and diverges from there. A mount made afterwards is visible only in the
namespace that made it, and an unmount only removes it from the
namespace that asked.

This completes the pair the root started. A root says which subtree a
process may name; a namespace says what is attached inside it. Without
the namespace a confined process still shares the mount table with
everything else, so anything it mounts appears system-wide and anything
mounted system-wide appears under its root. Starting one is privileged,
like the root and the domain, and for the same reason: privilege flows
down.

**A namespace copies the view, not the filesystems.** A mount is one
filesystem instance -- a vnode cache, an open transaction, a device --
and duplicating it would give two namespaces two views of one disk with
two sets of dirty state, which is not isolation but corruption. So a
mount carries the set of namespaces that can see it, a new namespace
adds itself to every mount its parent could see, and the filesystem is
mounted exactly once no matter how many namespaces show it.

Three details that are choices rather than accidents:

- **Unlink and rename stay conservative.** A directory that is a
  mountpoint in *any* namespace refuses to be removed or renamed, even
  from a namespace that cannot see that mount. A mount is attached to
  the vnode, not to the path, so a namespace that cannot see it is
  exactly the one with no basis to decide its fate.
- **The last namespace out unmounts.** A mount that no namespace can
  see is unreachable, so a namespace that goes away takes with it every
  mount only it could see -- children before parents, since a nested
  mount holds a vnode of the one below it. This is the same rule as
  everywhere else in this kernel: nothing is released while its fate is
  unknown, and a mount nobody can reach whose data was never committed
  is exactly that.
- **The root filesystem is visible everywhere.** It is not on the
  visibility machinery at all: every namespace needs a root, no
  namespace may unmount it, and a set that always has the same one
  member is a fact better stated than stored.

Not done here: moving or rebinding a mount between namespaces, mount
propagation (a shared mount whose children appear in peers), and any
way to enter an existing namespace -- a namespace is joined by being
spawned into it and in no other way.

## 1e. The uts namespace, and a hostname to put in it

The machine had no name. `uname` reported the constant "cosmo" to a
Linux binary and nothing else asked, so the first half of this is
introducing a hostname at all: a name a privileged process sets
(`sethostname`), anyone reads (`gethostname`), `uname` reports and
`sysctl kernel.hostname` shows.

The second half is that a name a contained process reads should be the
name of its container, not of the machine underneath it -- which is the
whole reason the thing exists. So the hostname lives in a **uts
namespace**: a spawn may start a new one holding a copy of the caller's
name, and setting it there changes nothing outside.

It is by far the smallest of these primitives -- one string, no
lifetimes to get right, nothing to unmount -- and it is here because
software asks the machine its name and believes the answer. A contained
process that reports the host's name is telling every log line and every
peer something false about where it is running.

Two details that are choices rather than accidents:

- **The domain name is not namespaced, and is not settable.** `uname`
  reports the constant "(none)". Linux carries `domainname` in this
  namespace for NIS, which nothing here has ever used; a second string
  with a second syscall and no reader would be scaffolding, not a
  feature. The namespace holds what something actually reads.
- **Reading is unprivileged, setting is not.** A name is not a secret
  -- every process that logs anything wants it -- but a process that
  could rename the machine could make another one's logs and its peers'
  records say whatever it liked.

Not done here: any relationship between a hostname and the network
stack, which does not consult one; and entering an existing namespace,
which no namespace here offers.

## 1f. The syscall filter

A process may narrow the set of system calls it is allowed to make. The
filter is a bitmap indexed by system-call number in the process's own
personality: bit set, the call is allowed; bit clear, the process is
killed. It is installed with `syscall_filter`, inherited by children,
and can only ever be narrowed -- installing intersects with what is
already in force.

**It is the one primitive here that is unprivileged**, and the reason is
the whole shape of it: every other one decides what a subtree of
processes may see or reach, so starting one has to be privileged.
This one only ever takes authority away from the caller and its
children. A process that could not restrict itself would be unable to
do the one safe thing it can do without asking anyone.

Inheritance follows for the same reason confinement does elsewhere: a
child that could shed its parent's filter would make the filter one
spawn away from meaningless.

**A denied call kills the process** (`SIGSYS`, 31, so the exit status is
159 and says which of the ways to die this was). The alternative --
returning `ENOSYS` or `EPERM` -- was considered and rejected: a filter is
a statement about what this program will ever need, so a call outside it
means the program is not doing what it was confined to do. That is
either a bug or an exploit, and both are better stopped than handed an
error code and allowed to continue into a state the author never tested.

**Some calls cannot be denied.** `exit` always works: a process must be
able to stop, and a filter that kills a process for exiting is a filter
that turns every clean shutdown into a signal death. `sigreturn` is
likewise always allowed in the native personality, and `exit_group` and
`rt_sigreturn` in the Linux one -- a signal handler must be able to
return, or the first signal after a filter is installed is fatal for a
reason that has nothing to do with the filter. The set is named by the
personality, which owns the numbering.

**The filter reads the number and nothing else.** It does not inspect
arguments, and that is a decision rather than a missing feature.
Arguments live in user memory, so a filter that read one would be
checking a value the process can change between the check and the call
-- the classic way argument-inspecting filters are defeated. A system
call's *number* is fixed by the time the kernel has it. A filter that
can only say things that stay true is worth more than one that can say
more.

Bits beyond the mask the caller supplies are treated as **clear**, so a
program built against a smaller system-call count denies the calls it
has never heard of rather than allowing them. Unknown means denied, in
the direction that fails safe.

The filter is asked only of calls that **exist**. A number the kernel
does not implement answers `ENOSYS` whether or not a filter is
installed: a filter takes away authority the process would otherwise
have, and installing nothing must change nothing. Turning a nonexistent
call into a signal death would also make the filter's presence
detectable by a program that never tripped it.

Not done here: any filtering on arguments or on the caller's state, a
way to read the current mask back, and any notification to another
process (seccomp's user notification), which needs a supervisor this
system does not have.

## 2. Resource limits

`struct rlimits` is one 64-bit value per resource, inherited by copy at
spawn (after `SETCRED`, the limits still come from the parent) and owned
by the process like its credentials:

| Resource | Bounds | Enforced where | Default |
|---|---|---|---|
| `COSMO_RLIMIT_AS` | bytes of user address space mapped by regions (segments, stack, `mmap`, `brk`) | `vm_user_map_anon`: `-ENOMEM` | 2 GiB |
| `COSMO_RLIMIT_MEM` | bytes of anonymous memory populated (resident frames) | demand-zero fault: over the limit is "no memory" (fatal to the process for a user touch, `-EFAULT` inside a copy); populated maps (ELF load): `-ENOMEM` | 128 MiB |
| `COSMO_RLIMIT_NOFILE` | open handles | `handle_install`: `-EMFILE` | 64 (the table size; cannot exceed it) |
| `COSMO_RLIMIT_NPROC` | processes with the child's real uid, counting the child | `spawn`: `-EAGAIN` | 128 |
| `COSMO_RLIMIT_VMEM` | guest memory per VM created by the process | `vm_mem`: `-ENOMEM` (the VM records the cap at creation) | 64 MiB |

`COSMO_RLIM_INFINITY` (`~0`) disables a limit. `setrlimit` may lower a
limit freely and raise one only with privilege (`-EPERM` otherwise);
there is no separate soft and hard value: the Linux personality reports
`rlim_max == rlim_cur` and refuses a `rlim_max` above the current value
from an unprivileged caller. Limits apply to privileged processes too;
root raises its own when it needs to. Native calls `SYS_getrlimit` (56)
and `SYS_setrlimit` (57); Linux `getrlimit`, `setrlimit` and
`prlimit64` (self only) map `RLIMIT_AS`, `RLIMIT_RSS` (→ `MEM`),
`RLIMIT_NOFILE` and `RLIMIT_NPROC`; every other Linux resource reads as
infinity and accepts only infinity.

Where the numbers live: the address-space limits are copied into the
process's `vm_space` (`limit_mapped_pages`, `limit_anon_pages`) when the
space is created and whenever `setrlimit` changes them, so the VMM
enforces them without knowing about processes; `mapped_pages` counts the
pages of every region (split and merge leave it unchanged). The handle
limit is copied into the handle table (`limit`). The VM limit is read
at `vm_create` into `vm->mem_limit`. `NPROC` is counted at spawn under
the process-table lock: processes whose `cred.ruid` equals the child's,
plus one.

What the memory limit does to a running process: a demand-zero fault
that would exceed `MEM` is treated exactly like an allocation failure,
which milestone 5 made a clean outcome (the process dies with the fault
status; a system call returns `-EFAULT`). The ELF loader's populated
segments count against `MEM` page by page, so an executable whose
`p_memsz` sum exceeds the limit fails to load with `-ENOMEM` after
unwinding what it populated (finding #12's "unbounded `p_memsz`", the
allocation part).

## 3. ramfs and page-cache caps

**ramfs** (`/`, `/tmp`, `/dev`): every mount has a page budget
(`mount.cache_limit_pages`, `RAMFS_MAX_PAGES` = 16 384 pages = 64 MiB for
ramfs; 0 for filesystems with a backing store); the page cache counts
the pages it holds per mount (`mount.cache_pages`) and a miss that would
exceed the budget is `-ENOSPC`. A ramfs page is the file's only copy, so
ramfs mounts are marked `MOUNT_CACHE_IS_STORE` and their pages are never
reclaimed. The per-file cap (`RAMFS_MAX_FILE`, 64 MiB) stays.

**The page cache as a whole**: a global limit (`pagecache_limit_pages`,
a quarter of the buddy's pages at boot; `sysctl vm.cache_limit`,
`vm.cache_pages`) and a reclaim path. Clean pages of reclaimable mounts
sit on one global LRU (most recently inserted or written back at the
head); a page leaves the LRU when it is dirtied or freed and returns
when `pagecache_sync` cleans it. Before a read, write or page get/put
takes a vnode's cache lock, `pagecache_reclaim_if_needed` checks the
global count and, while it is at or above the limit, evicts from the
tail: take a reference on the victim's vnode (`kobject_tryget`; a vnode
already on its way out is skipped), lock its cache with `mutex_trylock`
(a busy vnode is skipped; nothing waits on a lock while holding another
of the same class), re-check that the entry is still clean and on the
LRU, remove it. Up to 32 pages per call, or until the count is under
the limit. Dirty pages are never reclaimed here: the filesystem
transaction engine (milestone 7) brings writeback and dirty thresholds;
until then a cache full of dirty pages may exceed the limit (a soft cap,
recorded in `invariants.md`). Reading a large cosmofs file no longer pins
every page until the last close (finding #24).

## 4. What this is not

No namespaces, no per-handle rights beyond READ/WRITE, no rights
reduction on `dup`, no audit log, no per-uid aggregate memory
accounting (limits are per process; a user with many processes is
bounded by `NPROC × MEM`). Each is a future subsystem with this
milestone's `struct rlimits`, `SETCRED` and the single privilege
predicate as its seams.
