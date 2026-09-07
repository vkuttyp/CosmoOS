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

Not done here: pid renumbering, a domain-scoped `/proc` (there is no
`/proc` yet), and any accounting per domain.

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
