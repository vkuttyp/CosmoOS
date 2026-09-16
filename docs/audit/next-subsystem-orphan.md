# NEXT SUBSYSTEM — the name is gone and the handle is not

Date: 2026-09-16. Tree: `main` at 6a72c4c (after PR #150, the snapshot
deadlist). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§3.

**Subsystem: an on-disk record of the inodes whose last name has gone
while a handle still holds them, published with the root that removed
the name, so the blocks come back whether or not the handle is ever
closed.**

This report closes the last open clause of the inventory's §3 row that
begins "No on-disk orphan list", which two units in a row named and
neither took. The fsck unit built the pass that finds these; the
unmount-leak unit closed the row's deferred-free half and said plainly
what it was leaving:

> It does not fix the *unlinked-but-open inode* half of the inventory
> row it shares. An inode whose last name went while a handle held it is
> a different record (an orphan list), with a different replay, and
> joining them would be two designs in one unit.

The tree already contains a test whose entire purpose is to document
this hole. `cosmofs-check-orphan-crash`
(`cosmofstest.c`) opens its own comment with:

```
 * The leak the design admits by omission, as a test: a file unlinked
 * while a handle still holds it keeps its blocks until the last
 * reference goes (cfs_evict frees them), and there is no on-disk record
 * of that intention.
```

and then asserts `r.orphan.count == 1` after a remount, and that the
space does not come back until an operator runs the checker with the
repair flag. A test that asserts the defect is a placeholder for the
unit that removes it.

## Problem

**The intention to delete lives only in memory, and the name is already
gone from disk.**

`cfs_unlink_common` (`cosmofs.c:1547-1590`) removes the directory slot
and decrements the victim's link count (`:1578-1582`), then writes the
inode. Nothing else happens. The blocks are released later, by
`cfs_evict` (`:2003-2021`), when the VFS drops the last reference:

```c
static void cfs_evict(struct vnode *vn)
{
    ...
    if (fs && cv && vn->nlink == 0) {
        /* The last link and the last reference are gone: release the
         * data blocks and the inode slot. */
        mutex_lock(&fs->lock);
        if (cfs_truncate_blocks(fs, &cv->inode, 0) == 0) {
            struct cfs_inode empty;
            memset(&empty, 0, sizeof(empty));
            cfs_inode_write(fs, vn->ino, &empty);
            if (fs->sb.inode_count > 0)
                fs->sb.inode_count--;
        }
```

Between those two moments a commit can land — the writeback thread's,
another operation's, an `fsync`. When it does, **the durable filesystem
contains an inode with `nlink == 0`, its extents intact, and no
directory entry anywhere that reaches it.** Whether those blocks ever
come back depends on a `cfs_evict` call that exists only in this
mount's memory.

- **A crash between them loses it for good.** The next mount reads an
  inode map with a live slot, a bitmap with the blocks set, and no name:
  an orphan. `cosmofs_check` reports it and `COSMOFS_CHECK_REPAIR`
  reclaims it, which is an operator with a tool, not a filesystem that
  keeps its own accounts.
- **A forced unmount is the same thing without the crash.**
  `VFS_UMOUNT_FORCE` discards the open transaction, so the eviction that
  would have freed the blocks is thrown away exactly as a crash throws
  it away.
- **`rename` has the same hole** and is easier to miss: replacing an
  open file sets `replaced->nlink = 0` (`cosmofs.c:1748`) and relies on
  the same eviction.

This is the third defect of one shape, and the first two are built. A
commit's frees waited for a commit that might never come (PR #148); a
snapshot's holds were recorded after the root that dropped them
(PR #150); and an inode's deletion waits for a handle that might never
close. Each time the answer was the same: **write down what this root
intends, before the root, and let the next mount finish the job.**

## Current implementation

There is none to describe, which is the point. The relevant facts are
what the surrounding units already built and what this one must fit
into:

- `cfs_super.free_root` (version 9) names a `CFS_KIND_FREELOG` chain
  listing what the transaction that produced this root freed. It is
  written in the commit's window — reserved before `commit_bitmap`,
  filled after it, published by the root — released exempt by the
  superseding commit, and replayed at mount before the bitmap is trusted
  (`cosmofs_core.c`, `freelog_reserve` / `freelog_fill` /
  `freelog_replay` / `freelog_release_previous`).
- The commit's reservation (`struct cfs_res`, `commit_reserve`) is the
  one place a commit may take blocks from after the fixpoint, and it
  already has two consumers. `cfs_res_take` is paired with
  `cfs_res_untake`.
- `cosmofs_check` counts an inode as an orphan when the walk reaches its
  slot and no name (`cosmofs_check.c:549-569`), and its comment already
  records the rule this unit must keep: the superblock's `inode_count`
  "follows the slot and not the name: it is decremented where the slot
  is zeroed, when the last handle on an unlinked inode goes […] not at
  the unlink."

## Why it matters

**Correctness.** Space and an inode slot are lost on every crash that
catches an unlinked-but-open file, and the loss is permanent: no mount
reconsiders it. It is a small leak per occurrence and an unbounded one
over a machine's life, and the pattern that produces it — open a file,
unlink it, keep writing to it — is the ordinary idiom for a temporary
file, not an exotic case.

**Consistency of the argument.** The unmount-leak report refused
"reclaim at mount with the structural checker" on the grounds that a
walk of the whole filesystem at every mount is a price nobody would pay
for a defect that a record makes free, and refused "leave it to the
operator, now that `fsctl` exists" as a workaround with a person in it.
Both refusals apply here word for word, and today this defect is
answered by exactly those two things.

**It is the last one of its shape.** After this the filesystem has no
intention that lives only in a mount's memory: every statement a root
makes about what it has done and what it still owes is on disk, in a
chain that root names.

## Design

### A record, not a structure

The obvious reading of "orphan list" is a list on disk that entries are
added to and removed from — which is what the snapshot list is, and why
that unit had to make it copy-on-write. **This is not that**, and the
difference is the design.

The set of inodes whose blocks are not yet free is **derived**: it is
exactly the inodes this mount has unlinked to zero links and not yet
evicted. So it lives in `struct cfs` as a plain array, and the commit
writes it out **whole**, the way `freelog_fill` writes `pending_free`
whole. Nothing is edited on disk, so nothing needs copying; the previous
chain is released exempt by the commit that supersedes it, exactly as
the free record's is.

That buys the thing that makes the common case cheap. An ordinary unlink
of a file nobody has open adds the inode to the set and then evicts it
— and the VFS drops the last reference in the same breath, so the add
and the remove normally fall inside **one transaction** and cancel in
memory before anything is written.

**Normally, not always, and the report says which.** `cfs_unlink_common`
releases `fs->lock` before returning; the VFS then drops the victim's
last reference, and `cfs_evict` takes the lock again. The two are not
one atomic step, and a writeback commit can land in the gap -- in which
case that commit writes a record naming an inode that is about to be
evicted, and the next commit retires it. That is *correct* and it is not
free, so the claim this design makes is the narrow one: **an unlink
costs nothing extra unless a commit falls between the unlink and the
eviction, and then it costs one record.** Widening it to a guarantee
would mean holding a filesystem lock across a VFS reference drop, which
is not the filesystem's to hold.

The test says so too: `cosmofs-orphan-cancels` runs with the writeback
thread off, which is what makes "no commit intervenes" a fact about the
test rather than a hope, and the benchmark below measures the ordinary
unlink under the same condition and states it.

### The format

```c
#define CFS_VERSION 10u            /* version 10: a record of pending deletions */
#define CFS_KIND_ORPHAN 14u        /* inodes unlinked with a handle still open */

struct cfs_super {
    /* ... */
    uint64_t free_root;            /* v9 */
    uint64_t orphan_root;          /* v10: head of the ORPHAN chain, or 0.
                                    * The first of today's reserved[4]; */
    uint64_t reserved[3];          /* the other three stay reserved */
};
```

The payload is the deadlist's and the freelog's shape reused a third
time — a `next` and a count and an array of `uint64_t` — holding inode
numbers rather than block numbers. Reused rather than twinned, as
`CFS_FREELOG_PER_BLOCK` already reuses `CFS_DEAD_PER_BLOCK`.

`CFS_VERSION_MIN` stays 2. A version-9 filesystem mounts and behaves
exactly as it does today: `orphan_root` is a reserved zero, read only
when `version >= 10`, and a version-9 commit records nothing — which is
correct, because nothing on such a filesystem will ever read it.

### Where it is written, and in what order

Inside the commit's existing window, as a third consumer of the
reservation it already takes:

```
 release the previous free record and the previous orphan record
 reserve                      the record's blocks, the snapshot list's,
                              and the orphan record's
 commit_bitmap                the fixpoint
 the deadlist fill            (PR #150)
 the orphan fill              the in-memory set, written whole
 the free record fill         what is left, plus every leftover
 dirty loop, labels, flush, superblock
 the release loop             bits only
```

The orphan fill goes **before** the free record's, for the same reason
the deadlist fill does: it consumes blocks from the shared reservation
and frees the blocks of the chain it supersedes, and both of those are
facts the record must be able to state. The record is last because it is
the one that names the leftovers.

The bound is the easiest of the three: `nr_orphans` is known before
`commit_bitmap` and `commit_bitmap` cannot add to it, so the number of
blocks is exactly `ceil(nr_orphans / CFS_ORPHANS_PER_BLOCK)` with no
slack — unlike `pending_free`, which the fixpoint grows.

### Where it is applied

`orphan_replay`, beside `freelog_replay` at mount and after it, because
freeing an orphan's blocks needs a bitmap that has already had the free
record applied to it. For each inode the chain names:

1. Read the inode. **If the slot is already empty, skip it** — a commit
   got there first, and that is not an error. This is the same tolerance
   `freelog_replay` has for a bit that is already clear, and it is what
   makes a repeated replay harmless.
2. Free its blocks (`cfs_truncate_blocks(fs, &in, 0)`), clear the slot,
   and decrement `inode_count` — precisely what `cfs_evict` would have
   done, which is the definition of this record's promise.
3. Leave the chain alone. It is retired by the next commit, which writes
   its own (empty) record and releases this one. **Not cleared as it is
   read**: a mount that replays and then goes away without committing
   must leave the filesystem as it found it, or the second mount loses
   what the first reclaimed. The free record's `cosmofs-freelog-idempotent`
   exists because the easy implementation got that wrong, and the same
   mistake is available here.

`inode_count` is the trap worth naming in advance. It follows the slot,
so the replay must decrement it exactly once per inode it actually
clears — which step 1 guarantees, because an inode already cleared is
skipped before the decrement rather than after it.

### What the checker must stop saying

An inode the orphan record names is a recorded intention, not a finding.
`cosmofs_check` gains two small things:

- the chain's blocks claimed as metadata, exactly as the free record's
  are (`cosmofs_check.c:440-452`);
- an inode named by the chain excluded from the `orphan` class, because
  reporting it would make every correctly-recorded pending deletion a
  fault.

The `orphan` class does not go away, and its repair does not either:
an inode with no name that the record does *not* name is still a real
finding, and after this unit it is a rarer and more interesting one —
it means the record was lost or never written.

### What it does not do

- It does not make an unmount succeed while a file is open. That is the
  VFS's business and unchanged; what changes is that a forced unmount
  no longer loses the space.
- It does not add a general pending-work queue. What it holds is any
  inode whose last name has gone while something still references it,
  and **that includes directories** -- see below; the earlier draft of
  this report said it did not, and was wrong.
- It does not generalise `snap_cow`. The previous report said that
  helper was "written to be the general 'copy a chain named by a
  superblock field' helper, not a snapshot-specific one". **It was
  not** — it is `snap_cow(fs, res)` over `fs->sb.snap_root` with
  `CFS_KIND_SNAPLIST` in it — and this unit does not need it, because a
  record that is rewritten whole is never copied. Generalising a helper
  for a caller that does not exist is the speculative kind of
  abstraction; the correction belongs here rather than in a refactor.

### Directories are in this state too

The first draft of this report excluded them, on the reasoning that
`rmdir` refuses a non-empty directory and that a directory has no
handles outliving its name. The first half is true and the second is
not.

A process's working directory is a **referenced** vnode
(`process.h`, `cwd_locked`, "referenced; p->lock" --
`docs/audit/next-subsystem-cwd-ref.md` is the unit that made it one), and
an empty directory can be removed while a process sits in it. A
directory opened for `readdir` holds a reference the same way. So
`rmdir` succeeds, `cfs_unlink_common` sets `victim->nlink = 0`
(`cosmofs.c:1578`), and the blocks wait for a `cfs_evict` that comes
when the process moves or exits -- which is the defect, with a
directory's data blocks instead of a file's.

The record covers them with no special case: the replay does what
`cfs_evict` does, and that is the same operation for both types.

**The trap, named in advance.** `rmdir` also decrements the *parent's*
link count and writes it in the same transaction
(`cosmofs.c:1579-1581`), so by the time the record is written the parent
is already correct on disk. **The replay must not touch the parent**, or
a directory removed and replayed loses a link its parent never had.
`cosmofs-orphan-dir` asserts the parent's `nlink` across the whole
cycle for exactly this reason.

### The §70 gate

*Ownership and lifetime.* The chain's blocks are allocated by the
transaction that writes them and freed, exempt, by the transaction that
supersedes them — the free record's lifetime exactly. The in-memory set
is owned by `struct cfs` and freed in `cfs_destroy` beside
`pending_free` and `pending_exempt`.

*Concurrency.* Every add is under `fs->lock` in `cfs_unlink_common` and
the rename path; every remove is under `fs->lock` in `cfs_evict`; the
fill is inside `cfs_commit`, which holds it throughout. The replay runs
at mount before the filesystem is published. No new lock and no new
order.

*Memory.* One `uint64_t` per pending deletion, in a doubling array, plus
one block written per `CFS_ORPHANS_PER_BLOCK` of them per commit that
has any. Zero on a filesystem that holds nothing open across a commit.

*Error handling.* A record that cannot be written fails the commit
before the root is published, and the existing rollback gives the
reservation back. A record that cannot be *read* at mount gets the same
answer the free record gets and for the same reason: **the mount
fails**. A root that says "these inodes are mine to delete" and cannot
say which is a filesystem whose inode map cannot be trusted, and the
operator has a checker. An inode the chain names that cannot be read is
weaker — it is one inode, not the map — so it is reported and skipped,
leaving an orphan the checker can still find.

*Security.* No new interface, no new privilege, no userland surface.

*Performance.* Nothing on the common path, under the condition stated
above: the add and the remove cancel in memory when no commit falls
between them. A commit that does have pending deletions writes
`ceil(nr_orphans / CFS_ORPHANS_PER_BLOCK)` blocks -- one for any
ordinary number of them -- inside the flush the commit already does, so
no extra barrier.

*Observability.* `cosmofs_stats` gains `orphan_root` and the count,
beside `free_root` and `pending_frees`, so a test and an operator can
see the record rather than infer it.

*Future extensibility.* This is the last intention that lives only in
memory, so the pattern ends here rather than growing. If a fourth ever
appears, the three built records are the template and the commit's
window has room for it.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/filesystem/cosmofs/cosmofs_format.h` | `CFS_KIND_ORPHAN` (14); `orphan_root` in `struct cfs_super`, the first word of the four-word `reserved[4]` taken for it and `reserved[3]` left; `CFS_VERSION` 10; `CFS_ORPHANS_PER_BLOCK` |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | the in-memory set in `struct cfs`; the fill and the release in `cfs_commit`; the bound in `commit_reserve`; `orphan_replay` at mount; `cosmofs_stats` |
| `kernel-services/filesystem/cosmofs/cosmofs.c` | the add in `cfs_unlink_common` and in the rename path's replaced victim; the remove in `cfs_evict` |
| `kernel-services/filesystem/cosmofs/cosmofs_internal.h` | the set, and the two calls that maintain it |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | the chain claimed as metadata; a recorded inode excluded from the `orphan` class |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | the new tests, and `cosmofs-check-orphan-crash` rewritten — it asserts the defect |
| `kernel-services/filesystem/cosmofs/cosmofscrash.c` | the replay workload keeps a handle on an unlinked file across a sync |
| docs | cosmofs `design.md` and `architecture.md`, README Status, `docs/README.md`, the inventory row |

## New APIs

```c
/* cosmofs_internal.h */

/* This inode's last name is gone and its blocks are not free yet. The
 * commit writes the set out whole; the pair cancels in memory when the
 * eviction lands in the same transaction, which is the ordinary unlink. */
void cfs_orphan_add(struct cfs *fs, uint64_t ino);
void cfs_orphan_remove(struct cfs *fs, uint64_t ino);
bool cfs_orphan_named(const struct cfs *fs, uint64_t ino);   /* for the checker */
```

No kernel-facing API change beyond two fields in `struct cosmofs_stats`.

## Migration plan

1. **The format and the accounting.** Version 10, `orphan_root`,
   `CFS_KIND_ORPHAN`, and the in-memory set maintained by the unlink,
   the rename and the evict paths — with nothing written yet. The test
   is that the set is empty at every quiescent point, which is the
   invariant everything else rests on and the cheapest place to get it
   wrong.
2. **The write.** The record filled in the commit's window from the
   reservation, released by the superseding commit, and *not yet
   replayed*. The tests are the ordering ones: the chain's blocks
   accounted in the bitmap the root publishes, a hundred commits leaving
   one chain and no residue, and an ordinary unlink writing no record at
   all.
3. **The replay.** `orphan_replay` at mount. The test is the defect
   itself, and `cosmofs-check-orphan-crash` is rewritten here rather
   than earlier, because until this step it is still telling the truth.
4. **The checker.** A recorded inode is not a finding, and the chain is
   metadata.
5. **The crash suite.** A handle held across a sync, so prefixes carry a
   live record.
6. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; steps 3 and 5 run `make test-crash`;
step 1 runs `make BUILD=release` (`docs/verification/invariants.md:89`:
`CONFIG_FAULTINJECT` defaults to `CONFIG_DEBUG`, so a hook without a
release stub fails the release build, which CI runs).

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `cosmofs-orphan-crash` | **the defect**: a file opened, unlinked and committed, the mount then discarded with the handle still open — and the *remount* reclaims the inode and its blocks with no operator and no repair flag, `clean` and the free count back to what it was | the unfixed tree, which is what `cosmofs-check-orphan-crash` asserts today |
| `cosmofs-orphan-cancels` | an ordinary unlink of a file nobody holds writes **no record**: `orphan_root` is 0 and the free count is unchanged, because the add and the remove fall in one transaction | write the set before running the evictions: every unlink on every filesystem costs a block and a chain |
| `cosmofs-orphan-written` | with a handle held across the commit, `orphan_root` is non-zero and its chain names exactly that inode | add at evict rather than at unlink: the record is empty and the next test's reclaim finds nothing to do |
| `cosmofs-orphan-idempotent` | mount, replay, force-unmount without committing, mount again: the same blocks, the same `inode_count`, nothing freed twice | clear the record as it is read, or decrement `inode_count` before the already-empty check: the second mount double-counts |
| `cosmofs-orphan-reserved` | the chain's blocks are **allocated in the bitmap the root publishes**: a remount finds neither a leak nor a reachable-and-free block | allocate in the fill instead of taking from the reservation: the check reports `seen_not_alloc`, the direction that hands live data to the allocator |
| `cosmofs-orphan-chain` | more pending deletions than one block holds are recorded across a chain and all of them replay | write only the first block: the count is short by the overflow and the remainder are orphans again |
| `cosmofs-orphan-supersede` | a hundred commits with a handle held leave one chain and no residue | do not release the previous chain: the free count falls by a block per commit, which no single-commit test sees |
| `cosmofs-orphan-dir` | an **empty directory removed while it is a process's working directory** is recorded and reclaimed the same way, and the *parent's* `nlink` is what it was before the rmdir at every point in the cycle -- before, after the record, and after the replay | have the replay decrement the parent as well as clearing the slot: the parent loses a link it never had, which `cosmofs_check` reports as `nlink_wrong` and no amount of reclaiming space would have caught |
| `cosmofs-orphan-rename` | a file **replaced by a rename** while open gets the same record and the same reclaim | handle only `cfs_unlink_common`: the replaced victim leaks, which is the half of this defect that is easiest to miss |
| `cosmofs-orphan-format` | the replay is gated at **version ≥ 10**: a version-9 image mounts, works and replays nothing | lower the gate: the version-9 image's reserved word is read as a chain head and the mount fails |
| `cosmofs-orphan-check` | the checker claims the chain as metadata and does **not** call a recorded inode an orphan | leave the checker alone: `cosmofs-check-clean` reports the chain as leaked and every correctly recorded deletion as a fault |
| `cosmofs-replay` (extended) | the crash workload holds a handle on an unlinked file across a sync, so prefixes carry a live record; every prefix still mounts clean and strands nothing | revert the replay: the prefixes that caught the window report an orphan, which is the measurement this unit exists to take to zero |
| `cosmofs-check-orphan-crash` (existing, **rewritten**) | the checker still finds and repairs an orphan the record did *not* name — built with the poison hook, because after this unit an ordinary unlinked-but-open file is no longer one | the rewrite is the point: left as it is, it asserts the defect, and a test that asserts the defect passes by the unit failing |

**Vacuity, named in advance.** `cosmofs-orphan-crash` fails on today's
tree, which is the strongest statement that it is not vacuous.
`cosmofs-orphan-cancels` is the one to watch from the other direction:
"no record was written" is also true of a filesystem whose record never
works, so it asserts in the same breath that the *held* case does write
one — the two halves in one test, because separately either passes for
the wrong reason.

**The existing test that must change, and why that is not cheating.**
`cosmofs-check-orphan-crash` asserts `r.orphan.count == 1` after a
remount and that the space returns only under `COSMOFS_CHECK_REPAIR`.
Both statements are descriptions of the defect. Rewriting it is
required, and the report says so in advance rather than discovering it
during the build: the repair path it exercises stays tested, on an image
the poison hook builds, because an inode orphaned *without* a record is
exactly what a lost or unwritten record leaves and is the case the
checker still owns.

## Benchmarks

- **An ordinary unlink's cost, before and after**, in block writes per
  commit, **with the writeback thread off** so that no commit falls
  between the unlink and the eviction. The claim is *zero* change under
  that condition, and the cancel is what makes it true; the benchmark
  fails the design if a record appears. With writeback on, the number to
  report is how often one does appear, which is the price of the
  narrowed guarantee rather than a defect.
- **A commit with N pending deletions**, N from 1 to a few thousand:
  one block per `CFS_ORPHANS_PER_BLOCK`, no extra barrier.
- **A mount's cost with a long record**: a thousand files opened,
  unlinked, and the mount discarded, then mounted.
- **The crash suite's prefix count and runtime** with the new workload,
  which is the number the per-test budget has to accommodate
  (`docs/testing/flakes.md`).

## Risks

- **The set must be exact in both directions.** An inode left in it
  after eviction is one the next mount tries to delete again — harmless
  only because the replay skips an already-empty slot, which is why that
  check is in the design rather than an optimisation. An inode missing
  from it is the leak back, silently. The add is in two places (unlink,
  rename) and the remove in one (evict), and the unit's first step is
  the invariant rather than the record.
- **`inode_count` is the number most easily got wrong**, because it
  follows the slot and not the name and the replay is a second place
  that zeroes a slot. `cosmofs-orphan-idempotent` is aimed at it
  directly.
- **A forced unmount leaves a record that may name an inode already
  evicted**, because the discard throws away the transaction in which
  the eviction and the record's removal both happened. The replay's
  tolerance for an empty slot is what makes that a no-op rather than a
  double free.
- **The set is unbounded in principle.** A program that opens and
  unlinks ten thousand files holds ten thousand entries. The array grows
  by doubling like `pending_free`, and the failure mode at the bound is
  the one `cfs_free_block_deferred` already has — log and leak one,
  rather than fail the unlink. The report prefers that to an unlink that
  can return `-ENOMEM` for a reason the caller cannot act on, and says
  so here so that the choice is visible rather than inherited.
- **Three records now share one reservation and one window.** Each
  addition has been a place to get the ordering wrong, and the ordering
  is what corrupts rather than leaks. The migration plan writes before
  it replays for exactly this reason.

## Alternatives considered

- **A flag in the inode instead of a list.** The most attractive
  alternative and the one that fails on the same argument the last two
  units used: an inode marked "pending deletion" is invisible until
  somebody looks at every inode, and a full walk of the inode map at
  every mount is the price the unmount-leak report refused for the free
  record. It is also strictly more on-disk churn — an inode block
  rewritten per unlink against a single record block per commit.
- **Reclaim at mount with the structural checker.** It already works,
  behind a flag, and a whole-filesystem walk at every mount is what this
  design exists to avoid. The fsck report also argued against automatic
  repair, and a mount is the least supervised moment there is.
- **Leave it to the operator with `fsctl`.** A workaround with a person
  in it, and the person has to know to do it. The same sentence the
  unmount-leak report used, and it has not become less true.
- **Free the blocks at unlink and let the open handle read what it
  wrote.** This is the defect, not the fix: a handle would read blocks
  the allocator had handed to something else.
- **Refuse to unlink an open file.** Correct for a different operating
  system. Every program that makes a temporary file by creating and
  immediately unlinking it would break, and the constitution's
  POSIX-compatibility direction rules it out.
- **Fold it into the free record.** The two are written in the same
  window and released the same way, so one chain with a tagged entry
  type would work. Rejected: the free record's entries are facts about a
  transaction that has finished, and an orphan entry is a promise about
  one that has not, so a mount must apply them at different times and
  with different tolerances. One chain would have to carry that
  distinction anyway, and the seam would be inside a block rather than
  between two.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
