# NEXT SUBSYSTEM — the pass nobody can run

Date: 2026-09-15. Tree: `main` at 4891254 (after PR #144, the
structural check). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: an operator interface to a filesystem's maintenance passes
— a stable name for a mount, a privileged channel that takes a command
against one, and the rule that keeps the mount alive while the pass
runs.**
**Built: PR #146.** The design below is as proposed; "As built" and "As
run" record what the build changed and measured. This report closes the
inventory's §3 row "No operator interface for the filesystem's
maintenance passes".

cosmofs has two passes over a mounted filesystem. `cosmofs_scrub`
(`cosmofs_scrub.c:265-295`) reads every block the filesystem reaches and
repairs a mirrored member where a copy has rotted. `cosmofs_check`
(`cosmofs_check.c:691-794`) walks the live tree, every snapshot, both
allocation maps and the inode map, and reports ten classes of
structural disagreement, repairing four of them.

Both were built last week and the week before. Neither can be run.

## Problem

**There is no way to ask.** Every caller of either pass is a self-test:
`cosmofs_scrub` from `cosmofstest.c`, `cosmofs_check` from
`cosmofstest.c` and the crash suite. The two are unreachable in
different ways, and the second is the worse one:

- `cosmofs_check` is compiled only under `CONFIG_DEBUG`
  (`cosmofs_check.c:31`). A release kernel does not contain it — not
  because a release kernel should not check its filesystems, but because
  nothing in one could call it if it did.
- `cosmofs_scrub` has **no such gate**. It is compiled into every build,
  release included, and has no caller outside the tests. A release
  kernel today ships the code that repairs a rotted mirror and no way to
  start it.

**A mount has no name.** `struct mount` (`vfs.h:134-163`) carries a
kobject, a filesystem, a root vnode, a mountpoint, a parent, a block
device, per-mount locks and two list links. It carries no identifier of
any kind. The table is an unbounded linked list (`g_mounts`,
`vfs.c:27`) under a mutex (`g_mounts_lock`, `:25`), and the only lookup
in the tree resolves a *path* and then compares vnodes (`vfs_umount2`,
`vfs.c:470-483`).

A path is not a name for a mount, for two reasons the tree already
demonstrates:

- **The same mount is at different paths.** A mount namespace holds a
  list of (mount, namespace) pairs (`mountns.h:29-43`); `mountns_create`
  copies the *view* and not the filesystems (`mountns.c:103-140`). One
  `struct mount` therefore appears in several namespaces, at whatever
  path each one mounted it, and in a namespace that never mounted it, at
  no path at all.
- **A path names different mounts over time.** Between an operator
  reading a list and issuing a command, the filesystem at `/mnt` can be
  unmounted and another mounted in its place. A command against a path
  is a command against whatever is there when it arrives.

**Nothing pins a mount.** `vfs_umount` decides a mount is busy by
scanning its vnode hash for references beyond the ones the filesystem
itself holds (`vfs.c:529-546`). An open file does not pin its mount;
busy-ness is inferred from the vnodes, after the fact. A pass that ran
for a minute against a mount nobody had pinned would be walking freed
memory the moment someone unmounted it.

**The obvious place is the wrong place.** The fsck report named "a
procfs node that holds a reference to its target mount" as the shape
this unit would need. Building it that way is now the thing this report
rejects, and the argument is in the tree rather than in taste:

- procfs's own header states the rule (`procfs.c:1-14`): machine-wide
  values belong in `sysctl`, and procfs is the per-process hierarchy.
  A mounts directory is machine-wide by construction.
- A procfs vnode is **never hashed** (`procfs.c:63-78`): every lookup
  builds a fresh one. There is no object for a reference to persist in.
- `proc_file_ops` (`procfs.c:177-180`) has a `readpage` and nothing
  else: no `open`, no `release`, no write. Every procfs file is a string
  rendered on demand. A command is not a string rendered on demand, and
  a reference held across an open needs an open to hold it across.

Making procfs writable, hashed and open-aware to reach a filesystem pass
is three changes to procfs for one feature that is not procfs's.

## Current implementation

- **The passes.** `cosmofs_scrub(struct mount *, struct cosmofs_scrub_stats *)`
  and `cosmofs_check(struct mount *, struct cosmofs_check_report *, unsigned)`
  (`kernel/include/kernel/cosmofs.h:70,118`). Both take the mount's own
  lock for the whole walk, sleep, and return their answer in an
  out-struct. `cosmofs_check` takes `COSMOFS_CHECK_REPAIR` and reports
  `repair_refused` when the walk was not sure enough to act on.
- **The mount table.** `g_mounts` (`vfs.c:27`), a linked list with a
  count in `g_nr_mounts` (`:29`), under the `g_mounts_lock` mutex
  (`:25`). `vfs_sync` (`vfs.c:612-639`) is the precedent for iterating
  it safely, and its shape is worth copying exactly: it walks by
  *ordinal*, retaking the lock for each k, finding the k-th entry,
  taking `kobject_get` and dropping the lock before it does any work.
  That is what makes it safe against a list that changes under it. It is
  the only code in the tree that holds a mount across an operation it
  did not resolve by path.
- **Mount namespaces.** `struct mount_ns` holds `struct mount_ns_ref`
  entries (`mountns.h:29-43`); the root mount carries none
  (`mountns.c:61`). `covering_mount_ns` (`vfs.c:303`) is how a walk
  decides which mount a namespace sees at a point.
- **The control-plane precedent.** `/dev/net/tapctl`, mode 0600
  (`tap.c:438`), takes one fixed-layout `struct cosmo_netctl` written
  whole at its exact size and returns a versioned snapshot on read
  (`uapi/cosmo/netctl.h`). It is at version 5, each version adding
  commands and growing records, each documented in the header itself.
  `chrdev_ops` has `open` and `release` (`vfs.h:309-319`) and a file
  carries per-open private state (`vfs.h:171-172`, `file_run_open`
  `vfs.c:1103-1116`); `tap_chr_open`/`tap_chr_release`
  (`tap.c:168-229`) are the reference.
- **Privilege.** One predicate: `cred_privileged(cred_current())`
  (`cred.h:47-54`), which `sys_mount` (`native.c:664`) and `sys_umount`
  (`:693`) call. Device modes are enforced by `vfs_permission`
  (`vfs.c:677-697`) at open, with root bypassing.
- **sysctl.** Read-only and get-only (`native.c:1661-1765`,
  `libc/include/cosmo/sysctl.h:5`), with no `fs.*` names. It does have
  one precedent for a privileged read with a side effect:
  `debug.preempt_probe` gated by `cred_privileged` at `native.c:1723`.

## Why it matters

**A check nobody runs is a check nobody has.** The fsck unit measured
that every crash strands blocks: 162 of 199 replayed prefixes, 1912
blocks in all. It can find them and give them back. On a real machine,
after a real power loss, there is no way to ask it to. The unit's own
inventory row says so, and this is the row.

**The scrub has been unreachable for longer.** It predates the check and
has the same problem. Mirrored members exist so that a rotted copy can
be repaired from a good one; the code that does it has been in the tree
for weeks with no caller outside a test.

**The release build is the honest measure.** A release kernel today
carries the scrub as dead code and does not carry the check at all.
Neither is a decision anyone made about maintenance; both are what
happens when the only caller is a test. A release kernel that ships a
filesystem with snapshots, compression, encryption and mirrored members,
and no way to check any of it, is shipping the feature and not the
operations.

**And the seam is reusable.** A name for a mount is not a filesystem
feature. Anything that will later act on one mount rather than on a path
— a per-mount statistic, a quota, a freeze, a resize — needs the same
name, the same visibility rule and the same pin. This unit builds that
once.

## Design

### A mount is a number

`struct mount` gains `uint64_t id`, assigned in `mount_alloc`
(`vfs.c:235-253`) from a monotonically increasing counter under
`g_mounts_lock`, starting at 1. **Never reused**: a counter and not an
index, because an index is reused the moment a slot is freed and an
operator holding a stale id would then command a filesystem they never
listed. At one mount per microsecond a 64-bit counter lasts longer than
the hardware.

The id is the name. It is stable for the life of the mount, identical in
every namespace that can see the mount, and meaningless once the mount
is gone — which is the property a name should have.

### What a caller may see

**The listing is the caller's own namespace.** For each mount the
caller's `struct mount_ns` holds, the snapshot carries: the id, the
filesystem type's name, the mount flags, whether the filesystem offers
each pass, and the path *in this namespace*. A mount the namespace does
not hold is not listed, and a command naming its id is `-ENOENT`.

**Where the path comes from, because the tree cannot derive one.** There
is no vnode-to-path machinery here and this unit is not the place to
invent it: `getcwd` returns a string the process *remembers*
(`native.c:1562-1573`, `p->cwd_path_locked`), not a path walked back up
from a vnode. So the path is recorded rather than reconstructed.
`struct mount_ns_ref` (`mountns.h:29-35`) is the (mount, namespace) pair
and is exactly the right place: it gains the path at which *this*
namespace holds *this* mount, copied from the argument `vfs_mount` was
given, and copied again when `mountns_create` clones the view.

The honest caveat, stated here rather than discovered later: **it is the
path as of the moment the namespace gained the mount.** Renaming an
ancestor directory does not update it, exactly as it does not update a
process's remembered cwd — the same defect, with the same open report
against it (`docs/audit/next-subsystem-cwd-ref.md`). The id is what a
command is issued against; the path is a label for a human, and the
report says so rather than implying the path is authoritative.

**The root mount is a special case, by the namespace code's own design.**
`mountns.c:61` skips it: the root carries no `mount_ns_ref` in any
namespace, because it is visible in all of them. A listing built only
from a namespace's `mounts` list would therefore omit the filesystem
most worth checking. The snapshot emits the root mount first, at `/`,
before walking the namespace's list.

**A process with a narrowed root sees paths it cannot reach.** Per-
process roots exist (`vfs_current_root`), so a path reported here is the
*namespace's* path and not the caller's. That is the right answer for a
listing of mounts, and it is another reason the id and not the path is
what a command names.

**The snapshot protocol.** A path is `VFS_PATH_MAX` bytes and a mount
list cannot be copied out under a lock, so: take `g_mounts_lock`, count
the namespace's mounts plus one for the root, drop the lock, allocate;
take it again, fill until the buffer is full, and record how many there
were. `count` is what fits, `total` is what there was. A namespace that
gained a mount between the two passes produces `count < total`, which
the reader can see, rather than a listing that silently omits one.

`-ENOENT` and not `-EPERM`, deliberately: the two answers differ in what
they tell a caller about a filesystem they cannot reach, and "there is
no such mount here" is the true one. A mount in another namespace is not
hidden from this caller for safety; it is *not theirs*.

This holds even though the device is root-only. The namespace rule is
the tree's rule (`covering_mount_ns`, `vfs.c:303`), and an interface
that ignored it because its caller happens to be privileged would be the
first thing in the tree to treat a namespace as advisory.

### The channel

`/dev/fsctl`, mode 0600, root-owned: the same shape as `/dev/net/tapctl`
and for the same reasons. A caller writes one fixed-layout command whole,
at its exact size, and reads the result back from the same open file.

```
write(fd, &cmd, sizeof cmd)   -> the pass runs, or the command is refused
read(fd, buf, len)            -> the result of this file's last command
```

**The result belongs to the open file**, held in the per-open private
state the chrdev layer already provides (`vfs.h:171-172`). Two operators
with two open files do not see each other's answers, and there is no
global "last result" to race over. An open file whose last command has
not completed, or which has issued none, reads zero bytes; a completed
command's result is readable until the next write replaces it.

Commands, each its own fixed-layout struct sharing a `{version, op}`
first four bytes. **Every read is preceded by a write, `LIST` included**:

| command | what it does |
| --- | --- |
| `LIST` | build the snapshot of mounts this namespace holds |
| `CHECK` | run `cosmofs_check` against one id; `flags` carries `REPAIR` |
| `SCRUB` | run `cosmofs_scrub` against one id |

`LIST` is a written command and not "a read with no prior command",
because the two rules would otherwise contradict each other: a read with
nothing pending returns **zero bytes**, always, and that is the only
thing it can mean. An interface where the empty state and the listing
are the same request has no way to say "nothing yet".

A version, in the header, with the netctl header's discipline: every
version documented where the constant is defined, and a writer of the
wrong size refused rather than reinterpreted.

### What the VFS knows about a pass

Two optional entries on `struct fs_type` (`vfs.h:123-129`):

```c
int (*check)(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);
int (*scrub)(struct mount *mnt, struct cosmofs_scrub_stats *out);
```

The VFS does not learn what a check is; it learns that a filesystem has
one. ramfs and procfs leave both null, the listing says so per mount,
and a command against such a mount is `-EOPNOTSUPP` before anything is
locked. This is the seam that keeps `fsctl.c` in `kernel-services/vfs/`
from including `kernel/cosmofs.h`, which would make the VFS depend on
one of its filesystems and break the constitution's invariant 4.

The two out-structs are cosmofs's, which is the one wrinkle: the
prototypes name types the VFS does not otherwise know. They are declared
in `kernel/cosmofs.h` and the VFS needs only the forward declarations,
because it never dereferences either — it hands the pointer to the
filesystem and copies the bytes out by size. The report stating this
plainly is better than inventing a filesystem-neutral result type that
has exactly one implementation.

### Keeping the mount alive

Three rules, and the third is the one with teeth.

1. **Resolve the id under the table lock and take a reference.** As
   `vfs_sync` does (`vfs.c:622`): hold `g_mounts_lock`, find the id,
   `kobject_get`, drop the lock. The mount cannot be freed while the
   pass runs.
2. **Refuse a mount that is on its way out.** `struct mount` already has
   `unmounting` (`vfs.h:161`), set first thing by `vfs_umount`
   (`vfs.c:521-527`) to turn new walkers away. A command that finds it
   set is `-EBUSY`, and that refusal is also what makes rule 3's drain
   terminate.
3. **Unmount waits for a pass, and no new pass starts once it begins.**
   A reference alone is not enough: `vfs_umount` decides busy-ness by
   scanning the vnode hash (`vfs.c:529-546`), which a pass holding a
   kobject reference does not appear in. So the mount gains `unsigned
   passes_running`, incremented under `g_mounts_lock` around the call,
   and `vfs_umount` **drains it** before its busy scan.

The drain is a handshake with the flag unmount already sets:

- `vfs_umount` sets `mnt->unmounting` before anything else it decides
  (`vfs.c:521-527`), to turn new walkers away. Rule 2 makes a new pass
  one of the things it turns away: `vfs_mount_acquire` refuses a mount
  with the flag set.
- So once unmount has begun, `passes_running` can only fall. Unmount
  then waits for it to reach zero and proceeds.

**One unmount at a time, because the drain is what makes that a
question.** Today `vfs_umount2` holds `g_mounts_lock` unbroken from
`vfs.c:487` to `:568`, so two unmounts of one mount serialise on the
lock and the code never has to ask. It does not ask: `unmounting` is
tested only by `follow_mount` (`vfs.c:657`), never by unmount itself.
The drain below opens a gap in that lock, and **the gap is this unit's
doing, so the guard is this unit's to add**: under `g_mounts_lock` and
`mp->lock` — the same pair that sets the flag, so there is one order —
an unmount that finds `unmounting` already set returns `-EBUSY` before
it changes anything. A second caller arriving during a drain is told
somebody is already unmounting this, which is true.

The `restore` path (`vfs.c:593-599`) already clears the flag when an
unmount is refused or fails, so a mount that survives a failed unmount
is claimable again.

**The wait, in full, because a drain without a protocol is a deadlock.**
`vfs_umount` holds `g_mounts_lock` from `vfs.c:487` until `:568`, across
the whole region the drain sits in, and `vfs_mount_release` needs that
same lock to decrement. Waiting there as written would be unmount
holding the lock that the only thing able to release it must take. So:

```c
mnt->unmounting = true;          /* under mp->lock, g_mounts_lock held */
mutex_unlock(&mp->lock);
while (atomic_load(&mnt->passes_running) != 0) {
    mutex_unlock(&g_mounts_lock);
    wait_event(&mnt->passes_quiet, atomic_load(&mnt->passes_running) == 0);
    mutex_lock(&g_mounts_lock);
}
/* zero, and it stays zero: `unmounting` is set and acquire refuses */
```

- **The count is written under `g_mounts_lock` and read atomically.**
  The lock orders it against the mount being published or removed; the
  atomic load is what lets the wait condition read it with the lock
  dropped.
- **No wakeup is lost**, and this is the one place the report leans on a
  property of an existing primitive rather than on its own argument:
  `wait_event` enqueues the waiter and marks it BLOCKED *before* it
  evaluates the condition (`wait.h:1-12`), so a release that lands
  between the unlock and the wait wakes a waiter that is already queued.
  Mesa semantics re-evaluate after every wake, so a spurious one costs a
  loop iteration.
- **`vfs_mount_release` wakes on the falling edge only**: it decrements
  under `g_mounts_lock` and wakes `passes_quiet` when the count reaches
  zero. Waking under the mutex is fine — the waitqueue's own lock is a
  spinlock taken beneath it, never above.
- **The outer loop re-checks under the lock**, which is belt and braces
  rather than necessity: nothing can raise the count once `unmounting`
  is set. It costs one comparison and removes the need to prove that.

**A pass must never take `g_mounts_lock`.** It is acquired with the lock
dropped and released the same way, and a filesystem that reached back
into the mount table mid-walk would deadlock against a concurrent
unmount's drain. The two passes do not, and the rule is written here so
that the third one does not either.

**The wait is bounded, and that is why waiting is the right answer
rather than `-EBUSY`.** A pass count is held across a call the kernel
makes and returns from, never across a userland round trip, so the
longest wait is one pass over one filesystem — long, but finite and not
in an operator's hands. An unmount that failed instead would push the
retry loop into userland for no gain.

**A forced unmount waits too.** `VFS_UMOUNT_FORCE` cannot skip this: the
pass is reading the filesystem's buffers, and tearing them down under it
is a crash rather than a wrong answer. Force is about references held by
files, not about a kernel call in flight.

An earlier draft of this report claimed instead that shutdown would
never meet a busy mount because `vfs_sync` would block behind the pass.
That was wrong twice over — `vfs_sync` takes `mnt->sync_lock`
(`vfs.c:630`) while the passes take the filesystem's own lock, and a new
pass could start between the sync and the unmount anyway. The drain
makes the question moot, which is a better answer than a correct claim
about lock ordering would have been.

### What a caller may do to a filesystem

**Repair is reachable, behind an explicit flag.** `COSMOFS_CHECK_REPAIR`
frees blocks, clears inode slots and rewrites link counts on a *mounted*
filesystem. The case against exposing it is obvious. It loses for two
reasons:

- A repair nobody can invoke is the same defect as a check nobody can
  run, one layer in. The unit would close its row and leave the row's
  reason standing.
- The fsck unit's rule already makes it safe in the way that matters:
  repair acts only on a walk sure of its reachability, and reports
  `repair_refused` otherwise. The dangerous repair — the one that frees
  a live file's blocks because a directory block was unreadable — is the
  one the checker already refuses.

So repair is a flag on the `CHECK` command, off by default, and the
result carries `repair_refused` for the caller to read. It is not a
separate command, because "check" and "check and fix" differ by a
promise about mutation and not by an operation.

### The §70 gate

*Ownership and lifetime.* The id is assigned at `mount_alloc` and dies
with the mount; no lookup structure owns it, because a lookup is a walk
of a list whose length is the number of mounted filesystems, and a hash
for that is machinery without a reason. The per-open result buffer is owned by the
open file and freed in `release`. The reference taken in rule 1 is
released on every path out, including the error paths, which is the
thing to get wrong.

*Concurrency.* Two commands on two open files against the same mount
serialise on the filesystem's own lock, which each pass already takes.
`passes_running` is touched only under `g_mounts_lock`, the same lock
that publishes and removes a mount, so "is this mount going away" and
"is a pass running on it" are answered under one lock and cannot
disagree — which is the whole of rule 3's correctness, and the reason
the drain does not need the passes and the sync to share a lock. The
drain itself drops `g_mounts_lock` across its wait, because the thread
that will decrement the count needs it; the lock order is
`g_mounts_lock` then the waitqueue's spinlock, never the reverse, and a
pass holds neither while it walks. The pass itself runs with no VFS lock held: the table lock is
dropped before the call, or a check on a large filesystem would block
every mount and unmount on the machine rather than only its own.

**The mount's own lock is held for the whole walk, and that is the cost
this unit makes visible.** Every file operation on the checked mount
waits. On the test disks it is milliseconds; on a filesystem worth
checking it is the length of the walk. A pass that dropped the lock
would be answering about a filesystem that changed underneath it, which
is not an answer — so the lock is not the bug, the absence of a way to
see it coming is. This unit reports `elapsed_ns` (the check already
does) and the report's benchmark section measures it against filesystem
size so an operator can predict it. A progress and cancel interface is
named as deferred, with the design question stated: a cancellable walk
must define what a half-walk may claim, and today the honest answer is
nothing.

*Memory.* One counter and one `unsigned` per mount. One result buffer
per open file, sized by the largest result struct, allocated on first
use and freed at release. A listing snapshot cannot be built with the
lock held (it copies a path per mount) and cannot be sized without it,
so it is sized from `g_nr_mounts` under the lock, allocated after the
lock is dropped, and filled under the lock again with the count
rechecked — short by a mount that appeared in between, which the header
reports honestly rather than silently truncating.

*Error handling.* `-EPERM` if not privileged, `-EINVAL` for a version or
size the channel does not know, `-ENOENT` for an id this namespace does
not hold, `-EOPNOTSUPP` for a filesystem with no such pass, `-EBUSY` for
a mount being unmounted, and the pass's own return otherwise. A pass
that finds faults is **not** an error: the report says what they are and
the write succeeds. This matters because the shell idiom for "did it
work" is the exit status, and "the filesystem has 3 leaked blocks" is a
successful check.

*Security.* Mode 0600 plus `cred_privileged`, which is belt and braces
on purpose: the mode is discretionary and an operator can change it, the
credential check is not. The interface exposes block and inode numbers
of a mounted filesystem, which is information about its layout and not
about its contents — no path, no name, no byte of a file is returned by
either pass. The listing does return paths, and they are the caller's
own namespace's paths, which the caller could read by walking. Repair is
the one mutation and it is flagged.

*Performance.* The listing is one walk of the mount list. The list is
unbounded in principle and a handful in practice, which is the reason
the snapshot is sized from `g_nr_mounts` under the lock and the count
rechecked rather than assumed. The commands cost what the passes cost,
which is the point of measuring them.

*Future extensibility.* The version and the fixed-size commands are the
extension seam, and netctl has taken it five times. The two `fs_type`
entries are the other: a filesystem that grows a pass implements one and
appears in the listing as having it, with no change here. A freeze, a
per-mount statistic or a quota is a third command against the same name.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/fsctl.h` | new: the versioned command and result ABI |
| `kernel-services/vfs/fsctl.c` | new: the chrdev, the command dispatch, the listing, the id lookup |
| `kernel/include/kernel/vfs.h` | `struct mount` gains `id`, `passes_running` and a `struct waitqueue passes_quiet`; `struct fs_type` gains `check` and `scrub`; the lookup helper's prototype |
| `kernel/include/kernel/mountns.h` | `struct mount_ns_ref` gains the path this namespace holds the mount at |
| `kernel-services/vfs/vfs.c` | the id counter in `mount_alloc`; the drain and the `unmounting` guard in `vfs_umount2`; the id lookup that takes a reference and the release that wakes the drain |
| `kernel-services/vfs/mountns.c` | `struct mount_ns_ref` gains the path; `mountns_create` copies it; the visibility predicate the listing uses |
| `kernel-services/filesystem/cosmofs/cosmofs.c` | `cosmofs_fs_type` gains `check` and `scrub` |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | the `CONFIG_DEBUG` gate comes off (`:31`); `cosmofs_scrub.c` has none and needs no change |
| `kernel/core/main.c` | create the device at boot |
| `userland/system/fsctl.c` | new: the operator's tool |
| `kernel-services/vfs/vfstest.c`, `kernel-services/filesystem/cosmofs/cosmofstest.c` | the tests |
| docs | `docs/kernel-services/vfs/{api,design,invariants,testing}.md`, cosmofs `design.md` and `architecture.md`, README Status, `docs/README.md`, the inventory |

## New APIs

```c
/* kernel/include/uapi/cosmo/fsctl.h */
#define COSMO_FSCTL_VERSION 1

#define COSMO_FSCTL_LIST  1
#define COSMO_FSCTL_CHECK 2
#define COSMO_FSCTL_SCRUB 3

#define COSMO_FSCTL_F_REPAIR (1u << 0)   /* CHECK only: fix what has one right answer */

#define COSMO_FSCTL_CAP_CHECK (1u << 0)  /* this filesystem offers a structural check */
#define COSMO_FSCTL_CAP_SCRUB (1u << 1)  /* ... and a scrub */

struct cosmo_fsctl {            /* written whole, at exactly this size */
    uint16_t version;
    uint16_t op;
    uint32_t flags;
    uint64_t mount_id;          /* ignored by LIST */
};

/*
 * Every result begins with this header, so a reader knows what follows
 * and how big one record is before it parses any of it. `kind` is the op
 * whose result this is; `count` is how many records follow; `total` is
 * how many there were to report, which for a LIST that raced a mount is
 * larger than `count` -- a short listing that says so, rather than one
 * that looks complete.
 */
struct cosmo_fsctl_result {
    uint16_t version;
    uint16_t kind;
    uint32_t count;
    uint32_t total;
    uint32_t bytes;             /* of one record, so a reader can skip one it does not know */
};

struct cosmo_fsctl_mount {      /* LIST: one per mount */
    uint64_t id;
    uint64_t ns_id;             /* the namespace whose path this is */
    uint32_t flags;             /* the mount flags */
    uint32_t caps;              /* COSMO_FSCTL_CAP_* */
    char fstype[16];
    char path[VFS_PATH_MAX];    /* as recorded when this namespace gained the mount */
};

/*
 * CHECK: one record, and the ten classes are an array rather than ten
 * named fields, so a version that adds a class grows `nclasses` and does
 * not move anything. The kernel's own struct cosmofs_check_report stays
 * kernel-private; this is its serialised form and the device copies
 * field by field, because a kernel struct is not an ABI.
 */
#define COSMO_FSCTL_CLASSES 10
#define COSMO_FSCTL_NAMES   8

struct cosmo_fsctl_class {
    uint64_t count;
    uint64_t repaired;
    uint64_t name[COSMO_FSCTL_NAMES];
    uint32_t named;
    uint32_t reserved;
};

struct cosmo_fsctl_check {      /* CHECK: one record */
    uint32_t nclasses;          /* COSMO_FSCTL_CLASSES for version 1 */
    uint32_t flags;             /* PARTIAL, CLEAN, REPAIR_REFUSED */
    uint64_t blocks_seen, inodes_seen, dirs_seen, snapshots_seen;
    uint64_t counted_free, counted_inodes, bytes_allocated, elapsed_ns;
    struct cosmo_fsctl_class class[COSMO_FSCTL_CLASSES];
};

#define COSMO_FSCTL_R_PARTIAL        (1u << 0)
#define COSMO_FSCTL_R_CLEAN          (1u << 1)
#define COSMO_FSCTL_R_REPAIR_REFUSED (1u << 2)

struct cosmo_fsctl_scrub {      /* SCRUB: one record */
    uint64_t blocks_read;
    uint64_t inodes;
    uint64_t repaired;
    uint64_t unrecoverable;
};
```

The class order is the order `struct cosmofs_check_report` declares them
and the report's own findings table lists them, and it is part of the
ABI: index 0 is `alloc_not_seen` and index 9 is `unreadable`. A header
comment names all ten, because an array whose meaning lives only in
another file is a parser bug waiting to happen.

```c
/* kernel/include/kernel/vfs.h */
struct fs_type {
    /* ... */
    int (*check)(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);
    int (*scrub)(struct mount *mnt, struct cosmofs_scrub_stats *out);
};

/* Find a mount by id, visible to the calling process's namespace, and
 * take a reference plus a pass count. -ENOENT if this namespace does not
 * hold it, -EBUSY if it is unmounting. */
int vfs_mount_acquire(uint64_t id, struct mount **out);
void vfs_mount_release(struct mount *mnt);
```

## Migration plan

1. **The name.** `id` on `struct mount`, the counter in `mount_alloc`,
   and a test that two mounts get two ids, that an id is not reused
   after an unmount, and that a mount carried into a second namespace
   has the same id in both.
2. **The pin.** `passes_running`, the `passes_quiet` waitqueue,
   `vfs_mount_acquire`/`release`, and `vfs_umount` draining the count
   after it sets `unmounting` — dropping `g_mounts_lock` across the wait,
   because the releasing thread needs it. One test holds an acquisition
   and asserts the unmount waits and then succeeds, and that an
   acquisition attempted during the drain is refused, which is what makes
   the wait terminate. A second drives the wakeup itself with two
   threads. A third asserts that one unmount at a time is enforced,
   which the unbroken lock used to give for free and the drain's gap
   takes away. Nothing here depends on which lock a sync takes, which is
   the property the first draft of this step got wrong.
3. **The channel, read-only.** `/dev/fsctl`, the listing, the namespace
   visibility rule. Tests: a second namespace sees its own mounts and
   not the first's; the paths are that namespace's paths; an
   unprivileged open is refused.
4. **The commands.** `CHECK` and `SCRUB` through `fs_type`, the result
   read back from the same open file, the per-open isolation, the flag
   for repair. Tests: a manufactured fault is found through the device;
   repair through the device fixes it; a ramfs mount is `-EOPNOTSUPP`;
   a stale id is `-ENOENT`.
5. **The release build.** The `CONFIG_DEBUG` gate comes off the check
   (`cosmofs_check.c:31`); the scrub has no gate to remove and only
   gains a caller. `make BUILD=release` builds and boots with the device
   present, and the release-build test of step 4 runs a check through
   it — which is the first time either pass has run outside a debug
   build.
6. **The tool.** `fsctl list`, `fsctl check <id> [--repair]`,
   `fsctl scrub <id>`, and the user-mode test that drives it.
7. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; step 5 runs the release build; step
4 also runs `make test-crash`, because it is the crash suite's
filesystem the commands are pointed at.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `vfs-mount-id` — built | two mounts get two ids; an unmount and a fresh mount at the same path get *different* ids; the same mount in two namespaces reports one id | make the id the table index: the third assertion fails, because the slot is reused |
| `vfs-mount-pin` — built | an unmount does not complete while a pass holds the mount, and completes once it is released; the reference survives a concurrent mount of something else | drop `passes_running` from `vfs_umount`'s drain: the unmount completes while a pass still holds the mount |
| `vfs-mount-pin-drain` — folded into `vfs-mount-pin` | an unmount begun while a pass is in flight waits and then succeeds; a pass that tries to start after the unmount began is refused, so the count only falls; a forced unmount waits on the same drain | let `vfs_mount_acquire` ignore `unmounting`: a pass starts during the drain and the unmount waits for a mount that never quiesces, which the test catches as a timeout |
| `vfs-mount-pin-wake` — folded into `vfs-mount-pin` | the release that takes the count to zero wakes the drain: a second thread acquires, the first begins an unmount and blocks, the second releases, and the unmount completes without anything else happening on the machine | wake the queue on every release rather than on the falling edge, and then not at all on the last one: the unmount never returns, which is the missed wakeup this protocol exists to prevent |
| `vfs-umount-exclusive` — folded into `vfs-mount-pin` | a second unmount of a mount already draining is `-EBUSY` and changes nothing; after the first completes, the mount is gone and a third is `-EINVAL` rather than a double teardown; a first unmount that is refused clears the flag and a later one succeeds | drop the `unmounting` test from `vfs_umount2`: two unmounts enter the gap the drain opens and both remove the same namespace links |
| `fsctl-perm` — **not built** | an unprivileged open of `/dev/fsctl` is refused; a privileged one succeeds; a command from an unprivileged caller that somehow holds the fd is `-EPERM` | check only the mode: the second half passes and the third fails |
| `fsctl-list` — built | every mount the namespace holds appears once with its id, type, capabilities and this namespace's path; a mount only another namespace holds does not appear | list `g_mounts` directly rather than the namespace's view: the isolation assertion fails |
| `fsctl-list-root` — folded into `fsctl-list` | the root filesystem is in the listing, at `/`, in a fresh namespace as well as the initial one -- it holds no `mount_ns_ref` in either | build the listing from the namespace's `mounts` list alone: the root is missing, which is the filesystem most worth checking |
| `fsctl-list-ns` — folded into `fsctl-list`, from the other side | a child in a new mount namespace lists its own mounts, and the parent's private mount is absent from it and present in the parent's listing | same injection, from the other side |
| `fsctl-check` — built | a manufactured leak is found through the device, named by block, with the same numbers `cosmofs_check` reports directly; `REPAIR` gives the block back and a second command is clean | dispatch `CHECK` to the scrub: the class counts are all zero |
| `fsctl-caps` — folded into `fsctl-check` | a ramfs mount lists no passes and a `CHECK` against it is `-EOPNOTSUPP`, *before* any lock is taken | call through a null `fs_type` entry: the kernel faults, which the test catches as a failure to return the error |
| `fsctl-stale-id` — folded into `fsctl-check` | an id whose mount is gone is `-ENOENT`, not a hit on a reused slot | as `vfs-mount-id`'s injection: a reused id makes this command reach a different filesystem |
| `fsctl-result-per-open` — built | two open files run two commands against two mounts and each reads its own result; a read with no prior command returns zero bytes | keep the result in one global: the two readers see one answer |
| `fsctl-release` — **not built as a test** | a release build contains the device and both passes and a check through it works -- the first time either pass runs outside a debug build | no self-test runs in a release build. Replaced by a line at boot, checked by reading the release boot log; weaker, and "As built" says so |
| `fs_selftest` (user mode) — built as `fsctl_selftest` | `fsctl list` names the mounted filesystems; the test then writes a file, deletes it, unmounts and remounts, and asserts what that sequence leaves behind, reading the flags back through the device because an exit status cannot say "clean" when a finding is not an error. **Superseded by the unmount-leak unit (PR #148)**: when this was built the sequence stranded blocks and the test asserted the check found them and `--repair` reclaimed them; format version 9 made it strand nothing, so the same sequence now asserts `COSMO_FSCTL_R_CLEAN` and the find-and-repair path is covered by the kernel's tests instead. Exit status zero for a clean check *and* for one that found faults, non-zero only for a refusal | make a finding an error exit: the assertion on the first check fails. The finding itself is made by the test rather than inherited, so it does not pass on somebody else's residue |

**Vacuity, named in advance.** `fsctl-check` asserts the numbers match
what `cosmofs_check` returns when called directly, not merely that a
command succeeded: a device that ran nothing and returned a zeroed
report would pass a weaker test. `fsctl-list-root` exists because the
listing's most likely bug is an omission, and an omission is what a
test asserting "everything listed is correct" cannot see. `fsctl-list`
asserts each mount appears
**once** and with its own path, so a listing that repeated one entry or
reported every mount's path as the root would fail. `vfs-mount-pin`
asserts the unmount is refused *and then succeeds after release*, so a
`vfs_umount` that had simply become stricter would fail the second half.
`fsctl-caps` asserts the refusal happens before a lock is taken, which
is the difference between an error and a deadlock.

## Benchmarks

- **The listing**, against the number of mounts the boot has, so the
  cost of the linear scan is on the record before anyone proposes a hash
  for it.
- **A check through the device against a check called directly**, to
  show the channel adds nothing measurable to a pass that walks a
  filesystem.
- **The mount lock's hold time against filesystem size**, on the 512-
  and 16384-block test disks, which is the number an operator needs to
  predict what a check costs the machine. This is the measurement that
  decides whether the deferred progress-and-cancel interface is the next
  unit or a footnote.

## Risks

- **A lock held unbroken is now broken.** The drain drops
  `g_mounts_lock`, and every invariant the unmount path got for free
  from holding it throughout is now something to check rather than
  assume. One is found and guarded above: two unmounts of one mount.
  The others are what step 2's review of that function is for, and the
  step is written to expect more than one.
- **An unmount that now blocks.** Rule 3 makes `vfs_umount` wait, for
  every caller including shutdown. The wait is bounded by one pass over
  one filesystem, because no new pass can start once `unmounting` is
  set, and a pass is a kernel call rather than a userland round trip.
  The risk that remains is the size of that bound: a check of a large
  filesystem delays a shutdown by its duration, which is the same
  measurement the benchmark section takes for a different reason. If it
  turns out to be intolerable, the answer is a cancellable pass, which
  is already the deferred work named above — not a shorter wait here.
- **A result buffer per open file.** A caller can open the device many
  times and hold a buffer each. Bounded by the file descriptor limit,
  which is already an rlimit, and the buffer is allocated on first
  command rather than at open, so an idle open costs a pointer.
- **The `fs_type` entries name cosmofs types.** The VFS gains two
  prototypes mentioning structs it does not use. The alternative is a
  neutral result type with one implementation, which is worse; the risk
  is that a second filesystem with a different report makes the
  prototypes wrong, and the answer then is a union with a kind, decided
  when there is a second filesystem rather than now.
- **Namespace visibility has no test today.** The rule this unit adopts
  (`covering_mount_ns`) is exercised by the walk, not by an enumeration,
  and enumerating a namespace's mounts is new. Step 3's two tests exist
  for that reason and are the ones to write first.

## Alternatives considered

- **A procfs `/proc/mounts` hierarchy.** Rejected above, on the tree's
  own statements: procfs is per-process by its header's rule, its vnodes
  are never hashed, and its files have no open, release or write. Three
  changes to procfs for one feature that is not procfs's.
- **A `sysctl` name.** `fs.check` would fit the existing get-only shape
  and there is even a precedent for a privileged read with a side effect
  (`debug.preempt_probe`). Rejected because sysctl returns a string and
  takes no argument: naming a mount and carrying a flag would mean
  encoding both into a name, and reading a ten-class report out of a
  string is a parser nobody should write twice.
- **A new system call.** `SYS_fscheck` would be the most direct thing
  and the least reusable: a syscall number spent on one filesystem
  operation, with a second one owed for the scrub and a third for
  whatever comes next. The device carries all of them under one number
  that is already spent, which is the argument the netctl unit made and
  five versions have since justified.
- **An ioctl.** The tree has no ioctl and this is not the unit to
  introduce one. A write of a fixed-layout struct is an ioctl with the
  argument encoding written down.
- **Running the passes from the kernel on a timer.** A background check
  needs everything this unit builds and then a policy about when, plus
  the lock-hold problem without an operator who chose the moment.
  Deferred, and it is the unit after the progress interface rather than
  before it.

### As built

Differences from the plan, each found by building rather than reading.

1. **The id cannot be assigned under `g_mounts_lock`.** The design said
   to take it there; `mount_alloc` runs inside `do_mount`, *before*
   `vfs_mount` takes that lock, so there is no lock there to put it
   under. It is an atomic fetch-and-add, which needs none.
2. **A device vnode needed its own lockdep class, and the first boot
   said so by panicking.** A chrdev operation runs with the vnode lock
   held (`file_pwrite` takes it before dispatching), so consulting the
   mount table inside one inverts `mounts -> vnode`, the order
   `vfs_mount` takes. It cannot deadlock -- nothing mounts onto a
   character device, so the two are never the same lock -- but the
   classes were conflated. `ramfs_mkchr` now names a device node's lock
   `vnode-chr`. The deeper fix, not holding a filesystem lock across
   device I/O at all, is a different unit's argument and an inventory
   row.
3. **The one-unmount-at-a-time guard is not reachable the way the report
   implied**, and its bug-proof is what said so: remove the guard and
   nothing fails. `vfs_umount2` resolves its path through `follow_mount`,
   which refuses to walk to a mount that is unmounting, so a second
   unmount fails while resolving and never reaches the guard. The guard
   stays -- a relative path resolved from inside the mount does not
   traverse the mountpoint and does reach it -- but no test fires it, and
   that is an inventory row rather than a passing assertion left to look
   like proof.
4. **Fifteen named tests became six**, because most of them assert
   moments rather than fixtures and the moments overlap. The mapping is
   below; nothing named in the plan is unasserted except where this
   section says so.
5. **A release build has no self-test**, so "the device is there" is
   asserted by a line at boot rather than by a test. Weaker, and said
   plainly:
   `fsctl: /dev/fsctl ready (0600): mount listing, check and scrub`.
6. **The unprivileged-open test was not built.** A kernel self-test runs
   as root and the user-mode suite runs as root, so nothing in the tree
   can currently attempt an unprivileged open. The check is there in
   both the open and the write path; neither is fired by a test.
   Inventory row.
7. **The user-mode test mounts the test disk itself.** Nothing checkable
   is mounted when init runs -- the listing is a ramfs and a procfs --
   so a test that only named filesystems without passes would have
   proved the refusal and nothing else.
8. **The decision the drain invalidates has to be re-made, and that is
   the general shape of this whole class.** `vfs_umount2` decides
   `seen == 1` -- "I am the last namespace out" -- and *then* drops
   `g_mounts_lock` to drain. A namespace created in that window copies
   its parent's view and takes a reference, so the decision is no longer
   true and the teardown would leave the new namespace holding freed
   memory.

   The first fix was to stop the clone copying a mount that is
   unmounting, and it was wrong: an unmount can fail on a busy vnode or
   a sync error and be restored, and the child would then be permanently
   short a mount its parent has. A copied view that is missing something
   is the one thing a copied view may not be.

   So the clone copies everything, and the unmount **counts again after
   the drain**. If somebody else can see the mount now, the unmount
   steps down to being this namespace forgetting it. That covers the
   class rather than the case: this window has now produced a second
   unmount, a namespace clone, and a lock-order inversion, which is what
   the risk section meant by "every invariant the unmount path got for
   free from holding one lock throughout is now something to check".
9. **A mount id is a number, whole, or it is not an id.** The tool used
   `strtoull` and ignored where it stopped, so `fsctl check 12junk
   --repair` would have repaired mount 12. This command mutates
   filesystems; a misread argument is not cosmetic.
10. **A fixed listing buffer is a limit on how many filesystems an
    operator can find.** The kernel returns the listing whole or refuses
    a buffer too small, so the tool's 64-record allocation meant a
    machine with more than 64 mounts had none of them visible. It grows
    until it fits, and the user-mode test makes twenty mounts to walk
    that path.
11. **Unknown flag bits were accepted.** `LIST` ignored all of them,
    `SCRUB` took the check-only repair bit. Each op now refuses a bit it
    does not define, which is a caller's mistake caught now instead of
    an ambiguity when a later version gives that bit a meaning.
12. **And the first real filesystem it was pointed at was not clean.**
   See "As run": 28 leaked blocks on the boot's own test disk, which is
   a fact about cosmofs rather than about this unit, and the reason this
   unit exists.

**The tests, as named and as built:**

| the design's test | as built |
| --- | --- |
| `vfs-mount-id` | `vfs-mount-id`, unchanged |
| `vfs-mount-pin`, `-drain`, `-wake`, `vfs-umount-exclusive` | one test, `vfs-mount-pin`: the drain, the refusal that makes it terminate, the wakeup that ends it, and the second unmount are four assertions about one moment, and recreating that moment four times would assert the fixture rather than the rule |
| `fsctl-perm` | **not built** -- see difference 6 |
| `fsctl-list`, `fsctl-list-ns`, `fsctl-list-root` | one test, `fsctl-list`. The namespace rule is asserted from the other side: a mount this namespace drops leaves the listing while `vfs_mount_count` still counts it, because another namespace holds it. A kernel thread cannot run *in* a second namespace, so that is the reachable half |
| `fsctl-check`, `fsctl-caps`, `fsctl-stale-id` | one test, `fsctl-check`: the leak found by number, repaired, and the three refusals (a filesystem with no pass, a name nothing holds, a version and a size the channel does not know) |
| `fsctl-result-per-open` | `fsctl-result-per-open`, unchanged, plus: a result survives being read twice, and a buffer too small is `-ERANGE` rather than a truncated listing that parses as a whole one |
| `fsctl-release` | a boot line, not a test -- difference 5 |
| `fs_selftest` (user mode) | `fsctl_selftest` in `init.c`: the listing, the refusals, and a real cosmofs checked, scrubbed and repaired through the tool |

### As run

On both architectures, debug and release:

| run | result |
| --- | --- |
| `make test` (x86-64, AArch64) | 286 self-tests pass, five of them this unit's |
| `vfs-mount-id` | two mounts, two ids; a mount at a path an unmount just freed gets a *new* id; the root has one and it is neither |
| `vfs-mount-pin` | an unmount begun while a pass is held does not complete; a second acquisition is refused once it has begun; a second unmount is refused; releasing wakes the drain and the unmount takes the mount |
| `fsctl-list` | the mount at its path, the root at `/`, each once, `count == total`; a mount dropped from this namespace leaves the listing while the machine still counts it |
| `fsctl-check` | a leak found by block number through the device, the numbers equal to the pass called directly, repaired through the device, clean after; a scrub through the same channel; `-EOPNOTSUPP`, `-ENOENT` and two `-EINVAL`s |
| `fsctl-result-per-open` | two open files, two results, neither the other's |
| `fsctl_selftest` (user mode) | the tool lists, refuses, checks, scrubs and repairs; **the first time either pass has run from userland in this tree**. It strands its own blocks to have something to find: a file written, deleted, and the filesystem unmounted -- 41 blocks on the run above |
| `make BUILD=release test` (both) | builds, boots, and announces the device |

**Twelve bug-proofs, eleven of which fail for their stated reason:**

| injection | result |
| --- | --- |
| `id-is-an-index`: hand out the table index | `vfs-mount-id` fails: a freed slot is reused |
| `no-drain`: do not wait for a pass | `vfs-mount-pin` fails: the unmount completes while a pass is held |
| `acquire-ignores-unmounting`: let a pass start during the drain | `vfs-mount-pin` fails on the refusal that makes the wait terminate |
| `no-wake`: wake on every edge but the falling one | the **boot times out after 184 s**: the unmount never returns, which is the missed wakeup the protocol exists to prevent |
| `umount-not-exclusive`: drop the second-unmount guard | **passes** -- see difference 3 |
| `list-is-global`: list the mount table, not the namespace | `fsctl-list` fails |
| `list-omits-root`: build the listing from the namespace's list alone | `fsctl-list` fails: the root filesystem disappears |
| `check-runs-scrub`: dispatch every command to the scrub | `fsctl-check` fails on the result's size |
| `no-caps-check`: call a pass a filesystem does not have | **kernel panic: execute at 0x0** -- which is what the report predicted |
| `result-is-global`: one result for every open file | `fsctl-result-per-open` fails: one reader sees the other's answer |
| `refusal-is-success`: a refused command exits 0 | the user-mode suite fails |
| `finding-is-an-error`: a check that finds something exits non-zero | the user-mode suite fails -- and it fails *because the test disk is not clean*, which is the next line |

### What the first real filesystem said

The user-mode test mounts the boot's own scratch disk and checks it. It
is **not clean**, and the reason is a fact about cosmofs that the fsck
unit measured only for crashes:

```
mount 294: 20 blocks seen, 2000 free, 4 inodes, 2 directories, 0 snapshots (3360 us)
  28 leaked blocks: 2, 3, 4, 5, 6, 7, 25, 26
```

Twenty-eight blocks, all reclaimed by `fsctl check --repair`. They are
the residue of the boot's own `cosmofs-format` and `cosmofs-ops` tests.
The user-mode test no longer relies on finding them there -- it strands
its own, deterministically, by writing a file, deleting it and
unmounting, which produced 41 blocks on the same run. **The two numbers
count different things**: 28 is the residue this boot had already
accumulated before the tool looked, and 41 is what one deliberate
write-delete-unmount stranded. The mechanism is
the one the fsck report named: a commit publishes
the new root, *then* clears the freed blocks' bits in memory and dirties
those bitmap chunks **for the next commit** (`cosmofs_core.c`, "the
frees dirtied bitmap chunks for the next commit"). At an unmount there
is no next commit, so the cleared bits never reach the disk and the next
mount reads them as allocated and unreachable.

**So a cleanly unmounted filesystem strands what its last transaction
freed, for the same reason a crashed one does.** The fsck unit measured
the crash case at 162 of 199 replayed prefixes; nobody had measured
this one, because nothing could look. It is not fixed here -- an
operator-interface unit does not rewrite a filesystem's commit ordering
on the way past -- and it is an inventory row with this measurement
attached.

**Fixed by the unmount-leak unit (PR #148), which that row became.**
Format version 9 gives the superblock a `free_root` naming a chain of
blocks the root freed, written before the root and replayed at mount, so
the space returns whether or not another commit ever happens. The
measurement above stands as what this unit found; the numbers after it
are in `docs/audit/next-subsystem-unmount-leak.md`. The guess recorded
here and in the inventory -- that a second commit at unmount would fix
it -- was wrong, and that report says why: writing the bitmap frees the
bitmap, so the loop does not terminate.
