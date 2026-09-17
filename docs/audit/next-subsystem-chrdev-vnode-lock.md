# NEXT SUBSYSTEM — a filesystem lock held across a device that sleeps

Date: 2026-09-17. Tree: `main` at 5927316 (after PR #162, the VMState
layout rule). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: no filesystem lock is held across a device operation — the
rule, and the one place that breaks it.**

This report **takes up** the inventory's §3 row that reads "A character
device's operations run with the vnode lock held (`file_pwrite` takes it
before dispatching), so any device that consults the mount table inverts
`mounts -> vnode`", and specifically its last clause: *"the deeper
answer is that a filesystem lock should not be held across device I/O at
all, which no other device needed enough to argue for"*.

The row is marked *taken up* and is **not struck until the unit lands**,
which is the convention the rows beside it use. The unit proposed here
is what closes it; striking it is step 4 of the migration plan below.

## Problem

`file_pread` and `file_pwrite` take the vnode's mutex and then dispatch
to the character device inside it
(`kernel-services/vfs/vfs.c:1406,1428`):

```c
    mutex_lock(&vn->lock);
    if (vn->type == VNODE_CHR)
        n = vn->ops->read_file ? vn->ops->read_file(vn, f, off, buf, len)
          : vn->ops->read      ? vn->ops->read(vn, off, buf, len) : -ENOTSUP;
    else
        n = pagecache_read(vn, off, buf, len);
    mutex_unlock(&vn->lock);
```

The row frames this as a **lock-order** problem, and it is one. But the
ordering is the smaller half. The larger half is that one of those
devices **sleeps**.

### The chain, verified

1. **A device read can block indefinitely.** `tty_dev_read` calls
   `tty_read`, which waits for a line:

   ```c
   /* kernel/tty/tty.c:~485 */
   int rc = wait_event_killable(&t->readers,
                                t->lines > 0 || (!(t->flags & TTY_ICANON) && t->vmin == 0));
   ```

   That is a sleep with no timeout. A terminal with nobody typing at it
   waits forever, which is the correct behaviour for a terminal.

2. **Every open of one device node shares one vnode.** `ramfs_lookup`
   returns the directory entry's `child` (`ramfs.c:92`) — the same
   `struct vnode` object, and therefore the same `vn->lock`, for every
   opener of `/dev/console`.

3. **So a blocked reader holds that mutex for as long as it blocks.**

4. **And every other reader or writer of the same node queues behind
   it.** `file_pwrite` takes the same lock before dispatching.

Put together: **while one process waits at a terminal, a second
process's write to that terminal cannot proceed.** Not "is slow" —
cannot proceed, until the first read is satisfied or killed.

### Why nothing has failed

This is a live defect that nothing exercises, and the report should say
exactly why rather than imply it is theoretical or imply it is urgent.

- The kernel's own messages do not go through the VFS: `kprintf` reaches
  the console driver directly, so kernel output is unaffected.
- The shell harness has one interactive reader and its background jobs
  produce no output while it waits (`sleep 30`, `jobs`, `fg`), so no
  writer is ever queued behind a blocked read.
- The other character devices do not sleep. `tap_chr_read_file` returns
  0 when no frame is waiting (`tap.c:232`); `/dev/fsctl`, `/dev/vmm` and
  `/dev/net/tapctl` answer from memory.

So the one device that sleeps is the one nothing writes to
concurrently, and the tree has never had two processes share a terminal
in anger.

### The workaround that is there now

The row's own history: `/dev/fsctl` panicked on its first boot because
it consults the mount table, which inverted `mounts -> vnode`. The fix
was to give a device node's lock its own lockdep class
(`mutex_init(&vn->lock, "vnode-chr")`, `ramfs.c:452`), with a comment
arguing it is sound because nothing mounts onto a character device.

That argument is correct and the change is not wrong. But what it does
is **tell the checker not to look**. The ordering it silenced was a real
signal about a real rule, and the sleeping-reader problem above was
never the reason it was silenced, so it survived untouched.

## Current implementation

`vn->lock` is taken in **ten** places in `vfs.c`. **Two** dispatch to a
driver under it; the other **eight** are filesystem operations on
filesystem state, where holding it is the point:

| line | function | dispatches to a driver? |
| --- | --- | --- |
| `:1355` | `vfs_open` (the `O_TRUNC` truncate) | no |
| **`:1406`** | **`file_pread`** | **yes, on the `VNODE_CHR` arm** |
| **`:1428`** | **`file_pwrite`** | **yes, on the `VNODE_CHR` arm** |
| `:1516` | `file_sync` | no |
| `:1535` | `file_flush` | no |
| `:1584` | `file_readdir` | no |
| `:1778` | `vfs_truncate` | no |
| `:1904` | `vfs_stat` | no |
| `:1917` | `vfs_lstat` | no |
| `:1936` | `vfs_readlink` | no |

`file_sync` and `file_flush` call `vn->ops->sync` and
`vn->ops->writepage`, which *would* be driver entry points if a
character device supplied them. `ramfs_chr_ops` supplies neither
(`ramfs.c:385`) — it has only `read`, `write`, `open`, `release`,
`read_file`, `write_file` and `evict` — so those two cannot dispatch for
a `VNODE_CHR` vnode. There is no `ioctl` path either: this kernel gives
the terminal its own system calls rather than a multiplexer.

So the rule has exactly one violation site with two halves.

**And the tree already keeps the rule everywhere else it matters.** A
character device's other two driver entry points are called *outside*
the lock, deliberately: `vn->ops->open` from `file_run_open`
(`vfs.c:1190`), which takes no vnode lock at all, and `vn->ops->release`
from `file_release` (`vfs.c:1145`), which runs before the page-cache
flush takes it three lines later. So this is not a new policy being
introduced — it is `read` and `write` being brought into line with
`open` and `release`.

And for a `VNODE_CHR` vnode that lock guards **nothing**. Read both
branches again: the character-device arm touches no vnode field at all.
The regular-file arm is the one that needs it — it walks the page cache
and updates `mtime_ns`. The lock is held across the device call by
position in the function, not by design.

## Why it matters

- **It is a liveness bug in shipping code**, not a style question. Two
  processes, one terminal, one of them waiting: the other stops. The
  only reason it has never been seen is that nothing in this tree has
  ever put a writer behind a blocked reader on the same node.
- **It gets worse exactly when the system gets more useful.** A second
  interactive session, a job that logs while the shell waits for input,
  a `tee` to `/dev/console` — each is an ordinary thing to want, and
  each turns the latent bug into a hang.
- **The checker was told not to look.** `vnode-chr` is a class split,
  and class splits are how a lock-order checker is quieted. This unit
  replaces it with the rule it was standing in for, which means lockdep
  can go back to checking the thing it was reporting.
- **The rule generalises and the exception does not.** "A filesystem
  lock is not held across a device operation" is one sentence, it is
  checkable, and every future character device inherits it. "This
  device's lock has its own class because nothing mounts onto it" has to
  be re-argued for every device, and it was already wrong about *why*
  the ordering mattered.

## Design

### 1. The rule

> **A filesystem lock is never held across a device operation.** The
> VFS may hold a vnode's lock to inspect or change *its own* state; it
> releases it before calling a driver, and does not hold it again until
> the driver has returned.

### 2. The change

For `VNODE_CHR`, do not take the lock at all:

```c
int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off)
{
    struct vnode *vn = f->vn;
    ...
    if (vn->type == VNODE_CHR)
        /* No vn->lock: the driver owns its own state and may sleep for
         * an unbounded time (tty_read waits for a line), and this lock
         * is the one every other opener of the node needs. Nothing on
         * the vnode is read or written on this path. */
        return vn->ops->read_file ? vn->ops->read_file(vn, f, off, buf, len)
             : vn->ops->read      ? vn->ops->read(vn, off, buf, len) : -ENOTSUP;
    mutex_lock(&vn->lock);
    int64_t n = pagecache_read(vn, off, buf, len);
    mutex_unlock(&vn->lock);
    return n;
}
```

and the same shape in `file_pwrite`, where the `mtime_ns` update stays
inside the lock because it is vnode state and the regular-file path is
the only one that does it.

This is safe because:

- **The vnode cannot go away.** `f->vn` is a counted reference held by
  the open file; the read cannot outlive it.
- **The driver does its own locking.** `tty_read` takes `t->lock`;
  `tap_chr_read_file` works from `f->priv` and the tap's own state.
  None of them has ever relied on `vn->lock` — they cannot even see it.
- **Nothing on the vnode is touched** on the character-device path, so
  there is no field left unprotected. This is the claim to check while
  building, per device, and the report expects it to hold for all of
  them; a device that *does* touch vnode state is a finding and goes in
  the report as built.

### 3. The rule, checked rather than described

The tree already has the primitive for this:

```
kernel/include/kernel/lockdep.h:100
  #define lockdep_assert_not_held(lock, kind) KASSERT(!lockdep_is_held((lock), (kind)))
```

Both macros are **defined and used nowhere** — `grep` finds no call in
the kernel or the services, only the definitions and the lockdep suite's
own `lockdep_is_held`. So this unit is their first use, which is worth
saying plainly: the facility for checking "this lock is not held here"
was built and then never pointed at anything, which is the same shape as
the last two units and part of why this row survived.

The dispatch asserts it:

```c
    if (vn->type == VNODE_CHR) {
        /* The rule: a filesystem lock is never held across a device
         * operation. Checked here rather than stated, because the last
         * time this was only stated it was wrong for months. */
        lockdep_assert_not_held(&vn->lock, LOCKDEP_KIND_MUTEX);
        return vn->ops->read_file ? ... ;
    }
```

That fires in every debug build, on every character-device read and
write, for every device — including ones not written yet — rather than
in one test that exercises one device. It is the same move the VMState
unit made with `_Static_assert`: put the check where the rule is, so it
cannot be true only where someone remembered to look.

It costs nothing in release builds, where `lockdep_assert_not_held`
compiles to `((void)0)` (`lockdep.h:120`).

### 4. What replaces the lockdep class

`mutex_init(&vn->lock, "vnode-chr")` in `ramfs_mkchr` goes back to the
ordinary `"vnode"` class. The inversion it was hiding cannot recur,
because the lock is no longer held when the driver runs — and if a
future change reintroduces it, lockdep reports it instead of being
excused from the question.

This is the part that makes the unit worth doing rather than a comment:
the workaround is removed, not layered over.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/vfs/vfs.c` | `file_pread` and `file_pwrite`: the `VNODE_CHR` arm outside the lock |
| `kernel-services/vfs/ramfs.c` | `:452` — the `vnode-chr` lockdep class removed, and the comment that argued for it |
| `kernel-services/vfs/vfstest.c` | the test below |
| `docs/kernel-services/vfs/invariants.md` | the rule, as a numbered invariant, *checked by* the tests |
| `docs/kernel-services/vfs/design.md` | the locking section: what `vn->lock` covers and what it explicitly does not |
| `docs/kernel-services/vfs/api.md` | `file_pread`/`file_pwrite`: the concurrency note |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row, struck — **when the unit lands**, per migration step 4; this report only marks it taken up |

No driver changes are expected: no character device reads `vn->lock`,
because no character device can reach it. **If one turns out to need it,
that is a finding** and it is the interesting outcome of the unit.

## New APIs

None. One rule, two call sites, one lockdep class removed.

## Migration plan

1. **The failing test first**, against the tree as it is: two threads,
   one blocked reading `/dev/console`, one writing to it, and an
   assertion that the write completes. It fails today by hanging, so it
   is written with a bounded wait and a clear verdict rather than a
   deadlock (see Tests).
2. **The change**: the `VNODE_CHR` arm out of the lock in both
   functions. The test passes.
3. **The lockdep class removed**, and `make test` run with lockdep on to
   show the inversion it was hiding does not reappear.
4. Docs, README Status, the inventory row struck.

Each step boots both architectures. Step 3 is the one that needs
lockdep's own report read, not just a pass.

## Tests

| test | claim | how it fails if the fix is reverted |
| --- | --- | --- |
| `vfs-chr-write-during-blocked-read` (new) | a write to a character device completes while another thread is blocked reading the same node | reverted, the writer never runs: the test waits a bounded time for the write to finish and reports the timeout, so a revert gives a **named failure rather than a hung boot** |
| the `lockdep_assert_not_held` at the dispatch (not a test) | the vnode's lock is not held when *any* driver runs | reverted, it fires on the first character-device read of the boot — `/dev/console` — and names the lock. It is an assertion in the path rather than a test beside it, so it covers every device and every future one |
| `make test` with lockdep (existing) | the `mounts -> vnode` inversion does not return once the class split is gone | reverted together with the class, lockdep reports the inversion the `/dev/fsctl` boot originally panicked on |

The first is the bug and must be written so that **failing is not
hanging**: the reader is unblocked by the test itself on a timer, and
the writer's completion is waited for with a deadline, so a reverted fix
reports "the write did not complete in N ms" instead of stopping the
boot. A test that proves a deadlock by deadlocking is not a test.

The second is the rule rather than the instance, and is the one worth
more: it holds for every character device, including ones not written
yet, and it is what the invariant's *checked by* will name. It is also
not a test — it is an assertion on the path, which is why it cannot be
skipped, mis-run or forgotten.

## Benchmarks

None needed. The change removes a lock acquisition from the character
device path; it cannot be slower. If review wants a number, the shell
harness's round-trip and `net-bench` over `/dev/net/tap` both exercise
character-device reads and writes and already report timings.

## Risks

- **A device that does rely on `vn->lock`.** The report claims none
  does, from reading every `chrdev_ops` in the tree. The build checks it
  per device, and a counter-example changes the design rather than the
  plan — it would mean the lock has a second job and the rule needs a
  narrower form.
- **The assertion depends on lockdep being on.** `lockdep_assert_not_held`
  is `((void)0)` in release builds (`lockdep.h:120`), so the rule is
  checked in debug and guard boots and not in release. That is the same
  bargain every `KASSERT` in the tree makes, and it is stated here
  rather than left implied; the behavioural test below runs in both.
- **Removing the lockdep class widens what lockdep checks**, which may
  surface an unrelated pre-existing ordering. That would be a finding
  and good news, but it can extend the unit; it is the reason step 3 is
  its own step with its own boot.
- **Nothing exercises the bug today**, so the unit's value is in what it
  prevents rather than what it fixes. That is an honest reason to rank
  it below a live failure, and an honest reason to do it before the
  second terminal exists rather than after.

## Alternatives considered

- **Keep the lock; make the terminal read non-blocking under it.**
  Wrong shape: it moves the constraint into every driver, forever, and
  the constraint is the VFS's to keep. It also cannot be stated as a
  rule a future driver will find.
- **A separate lock for the device path.** Adds a lock to remove a
  lock's misuse. The character-device path needs no VFS lock at all, so
  a second one is worse than none.
- **Hold the lock only for the duration of the dispatch decision.**
  That is what the recommendation does; stated as an alternative because
  an implementation might be tempted to take-check-drop-call-retake, and
  the retake is unnecessary — nothing after the call reads vnode state
  on this path.
- **Leave it: no writer has ever queued behind a blocked reader.** The
  honest version of doing nothing, and it is what the tree has done
  since the `/dev/fsctl` panic. It is defensible only while the system
  has one terminal and one session. The row has been open since 8.2 and
  the lockdep class has been standing in for a rule the whole time; a
  third sighting should cost more than the first two, which is the same
  argument the VMState unit was built on.
