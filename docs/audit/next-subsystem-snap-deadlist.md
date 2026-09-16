# NEXT SUBSYSTEM — the list the root does not name

Date: 2026-09-16. Tree: `main` at e5e50e0 (after PR #148, the record of
what a transaction freed). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the snapshot list and its deadlists made copy-on-write, so
that the record of a held block is published by the root that freed it
instead of written into a transaction already gone.**

This report closes the inventory's §3 row that begins "The same defect
the deferred-free record fixes still lives in the snapshot path", which
the unmount-leak unit added when it found it and deliberately did not
fix. It also closes the row's silent half, which that unit did not
name: **the snapshot list is mutated in place**, so every change to it
is visible to the root it is not part of.

The previous unit ends with an assertion it had to weaken. In
`cosmofs-freelog-snapshot` (`cosmofstest.c:2579`):

```c
    CHECK(rep.alloc_not_seen.count <= 4);   /* the deadlist's, not the record's */
```

A bound, where every other filesystem in the suite asserts zero. This
unit is the one that makes it zero.

## Problem

**A block a snapshot holds is recorded after the root, by a write the
root cannot carry.** `cfs_commit` publishes the superblock
(`cosmofs_core.c:1128`), and *then* runs the release loop
(`:1150-1163`):

```c
    bool snapshots = fs->snap_count > 0;
    for (unsigned i = 0; i < fs->nr_pending; i++) {
        uint64_t dva = fs->pending_free[i];
        ...
        if (snapshots && cfs_snapshot_hold_block(fs, dva))
            continue;
```

`cfs_snapshot_hold_block` (`cosmofs_snap.c:410-435`) does two things
that belong to a transaction, at a moment when there is no transaction:

1. **It allocates.** `deadlist_append` (`:196-222`) calls `cfs_buf_new`
   when the head block is full, which calls `cfs_alloc_block`. That is
   an allocation *after* `commit_bitmap`, the one pass that makes the
   on-disk bitmap agree with the bits in memory — the exact thing the
   previous unit's own commit comment forbids (`cosmofs_core.c:1035`):

   > Everything that allocates must happen *before* `commit_bitmap`
   > […] Allocating after it publishes a root whose bitmap does not know
   > about the blocks, so the next allocation hands them out and the
   > filesystem eats its own metadata.

   The rule was written for the record's blocks and is true of every
   block. The snapshot path is the one place left that breaks it.

2. **It dirties blocks for a commit that may never come.** The deadlist
   block and the `CFS_KIND_SNAPLIST` block holding the entry are marked
   dirty and left for the next commit — and `vfs_umount` calls `sync`
   exactly once. This is the unmount leak, one level down, and the
   previous unit's fix does not reach it: the free record is written
   *before* the root and lists what phase 7 will clear, and a held block
   is by definition not cleared, so it is not in the record. Nothing
   carries it.

And a third, which the inventory row does not mention and which is the
worse of the three:

3. **The snapshot list is not copy-on-write.** `snap_walk` with
   `write=true` (`:38-62`), `cfs_snapshot_create` (`:546-556`),
   `clear_entry` (`:577-584`), the keeper write-back in
   `cfs_snapshot_delete` (`:646-669`) and `deadlist_append`'s
   partially-full-head case (`:203-209`) all take the block with
   `cfs_buf_get`, edit it, and call `cfs_buf_mark_dirty` — which is
   `buf_mark_dirty`, not `cfs_buf_cow` (`cosmofs_core.c:231-234`). The
   block is rewritten **in place**, under the block number the current
   root already names. Every other metadata tree in this filesystem is
   copy-on-write; `snap_root` is the one pointer whose target is edited
   where it lies.

## Current implementation

`cfs_super.snap_root` names a chain of `CFS_KIND_SNAPLIST` blocks, 42
`struct cfs_snapshot` entries each (`cosmofs_format.h:215`). Each entry
has a `deadlist` head naming a chain of `CFS_KIND_DEADLIST` blocks, 506
block numbers each (`:236`). A commit that frees a block a snapshot
still occupies leaves its bitmap bit set and appends its number to that
snapshot's deadlist; `cfs_snapshot_delete` settles the list against the
snapshots that remain (`deadlist_settle`, `:234-275`).

The verdict — does any snapshot still name this block — is exact and
cheap, and the previous unit already factored it: `snapshot_holds`
(`:300-349`) is the walk, `cfs_snapshot_holds` is the question alone,
and `freelog_fill` (`cosmofs_core.c:923-1017`) asks it *before* the root
so that the record it writes and phase 7's loop cannot disagree. The
question is settled. **Only the answer's storage is in the wrong
transaction.**

The free record itself shows the shape of the fix, in the same file: a
bound computable before `commit_bitmap`, blocks reserved against it
there (`freelog_reserve`, `:890-916`), and filled after the fixpoint
with what is finally true (`freelog_fill`), with the over-reservation
listed in the record as free. Reserve before, fill after, no second
pass and no loop.

## Why it matters

Three consequences, in the constitution's order.

**Correctness, at an unmount.** A filesystem with a snapshot that frees
a block the snapshot holds, and then unmounts, loses the deadlist entry
and the block. The next mount sees a block whose bit is set, which no
tree reaches, and which nothing will ever free — because deleting the
snapshot walks a deadlist that never heard of it. `cosmofs_check` finds
it as `alloc_not_seen`; that is what the `<= 4` bound above is counting.
It is small per unmount and it is permanent per occurrence.

**Correctness, across a crash — the in-place half.** A dirty
`CFS_KIND_SNAPLIST` block is written by the commit's dirty loop
(`cosmofs_core.c:1093-1105`), *before* the root. If the machine dies
between that write and the superblock, the surviving root is the
previous one, and it names the same block — now carrying the next
transaction's contents. Concretely:

- `deadlist_append` added block numbers to the head. Under the old root
  those blocks are still live in the live tree. Deleting the snapshot
  then asks only "does a *remaining snapshot* occupy this", not "does
  the live tree", so `deadlist_settle` hands live blocks to the
  allocator (`:266`). That is data loss, not a leak.
- `clear_entry` removed a snapshot. Under the old root the snapshot is
  gone and its blocks are still allocated: a permanent strand of the
  whole snapshot.

Nothing tests this, because **the crash suite's workload has no
snapshot in it** — `cosmofscrash.c` has one occurrence of the word, and
it is `ramblk_snapshot` (`:375`), an unrelated function. The suite
replays 211 prefixes and reports zero stranded blocks on every one of
them, on a filesystem that never took a snapshot.

**Cleanliness.** The tree has one rule for metadata — copy it, do not
edit it — and one exception, undocumented, in the subsystem whose whole
premise is that copy-on-write makes history free. The exception is why
the deadlist append had to be after the root in the first place: an
append published early is an append the old root can see.

## Design

### The rule this unit applies

The previous unit stated it for the record and this one applies it
everywhere (that is the whole of the change):

> **A block is allocated before the bitmap fixpoint; a statement about
> what this root freed is published by that root, not before it and not
> after it.**

"Not before it" is what copy-on-write buys, and it is the piece the
inventory row's guess omits. The row says the fix is "the same
reserve-before-fill treatment the record got, applied in
`cosmofs_snap.c`". Reserve-before-fill alone moves the append from after
the root to before it, and an append before the root under an in-place
update is worse than the leak it fixes: it is the crash above, which
frees live blocks. The reservation is necessary and it is not
sufficient. What makes it safe is that the appended state is reachable
only through the new root.

### Copy-on-write for the snapshot list

`snap_root` is a superblock field (`cosmofs_format.h:359`), so a new
chain is published exactly when the superblock is — atomically, by the
same `BIO_FUA` write, with no new ordering machinery. The change:

- **Every write to a `CFS_KIND_SNAPLIST` block copies it.** One helper,
  `snap_cow`, takes the chain and returns a writable copy of it,
  re-chaining `next` pointers from the head down and assigning
  `fs->sb.snap_root`, exactly as `cfs_buf_cow` does for a tree node with
  its parent slot. Because the chain is singly linked, copying a block
  in the middle means copying the blocks before it; the chain is one
  block per 42 snapshots, so "copy the chain" is the honest and the
  cheap answer both. Idempotent within a transaction: `cfs_buf_cow`
  already returns the same buffer when `h->generation == fs->gen`
  (`cosmofs_core.c:257-260`), so a commit that touches two entries
  copies once.
- **A deadlist block is never edited in place either.** The
  partially-full head is copied and appended to; a full head gets a
  fresh block chained in front of it. The chain's length is what it is
  today — one new block per 506 held blocks — plus one copied head per
  commit that appends.
- **The old blocks are freed exempt.** `cfs_free_block_exempt`
  (`cosmofs_core.c:409-425`) frees a block that this transaction owns
  outright, skipping the snapshot filter, and the rule it rests on is
  already written for the free record (`:848-856`): *a snapshot
  preserves `imap_root` and `alloc_root`, not the superblock fields
  beside them.* A snapshot's recorded bitmap does have the old
  snaplist's bit set — it was allocated before the snapshot was taken —
  so the generic filter would hold it and append it to a deadlist, which
  is the deadlist it is a copy of. The exemption is not an optimisation;
  it is what stops the regress.

### One reservation, one verdict, two records

The commit's window is unchanged in shape and gains a second consumer:

```
 freelog_release_previous     the old record's blocks, freed exempt
 reserve                      the record's blocks AND the snapshot list's
 commit_bitmap                the fixpoint: bitmap agrees with memory
 fill                         one walk of pending_free, splitting it:
                                held      -> the copied deadlist
                                not held  -> the free record
                              leftovers   -> the free record, as free
 dirty loop                   every copied block written
 labels, flush, superblock    the root publishes all of it at once
 phase 7                      clear bits in memory; no allocation,
                              no dirtying, no walk of the snapshot list
```

**One walk, not two.** Today `freelog_fill` asks `cfs_snapshot_holds`
per pending block and phase 7 asks `cfs_snapshot_hold_block`, which asks
the same question again — two walks of the snapshot list per freed
block, and two verdicts that are equal by argument rather than by
construction. After this unit the verdict is taken once and each block
goes into exactly one of the two records. Phase 7 keeps its own
`bit_test` guard and clears what the fill did not hold; it needs no
snapshot knowledge at all, and `cfs_snapshot_hold_block` disappears as a
commit-time entry point.

**The bound.** `freelog_reserve`'s bound is
`nr_pending + nr_exempt + nr_chunks + nmembers` (`:894`), which already
counts every block `commit_bitmap` can free. This unit widens it by the
snapshot list's own needs: the chain's length (the copies), plus one
copied deadlist head, plus `nr_pending / CFS_DEAD_PER_BLOCK + 1` fresh
deadlist blocks, plus the copies' own exempt frees, which are entries in
the record. Generous by design, as before, and the leftovers say so in
the record itself — the mechanism that keeps the bound from having to be
tight already exists and is tested (`cosmofs-freelog-overreserve`).

### What a snapshot change outside the commit does

`cfs_snapshot_create` and `cfs_snapshot_delete` edit the list and then
commit (`snap_commit`, `:444-454`). They go through `snap_cow` like
everything else, which is what makes the crash cases above vanish: a
create that dies mid-commit leaves the old root naming the old chain,
with no entry; a delete likewise, with the snapshot intact and its
blocks still held. Both of those are the correct answer to "this
transaction did not happen", and neither is true today.

`deadlist_settle` needs one adjustment beyond the copy: it frees the
doomed chain's blocks with `cfs_free_block_deferred` (`:271`), which
sends them through the snapshot filter, which will hold them on the
keeper's deadlist — the same regress. They become exempt frees, on the
same rule.

### What it does not do

- It does not change the on-disk **shapes**. `CFS_KIND_SNAPLIST` and
  `CFS_KIND_DEADLIST` are byte-for-byte what they were, and a chain
  built by this kernel is read by the previous one. **`CFS_VERSION`
  stays 9.** What changes is where the blocks live, which is not a
  format property; a version bump here would be a claim of
  incompatibility that is not true, and `CFS_VERSION_MIN` stays 2.
- It does not touch the *unlinked-but-open inode* clause of the
  inventory row the previous unit left open. That is still an orphan
  list, still its own unit.
- It does not make the snapshot list a tree. It stays a chain, because
  42 snapshots per block against `CFS_SNAP_ID_MAX` of 0xFFFE is at most
  1561 blocks in the worst case nobody has, and copying a chain of one
  is what every real filesystem here will do.

### The §70 gate

*Ownership and lifetime.* Every block the snapshot list occupies is
allocated by the transaction that writes it and freed, exempt, by the
transaction that supersedes it — the same lifetime the free record has.
Nothing outlives a mount; `struct cfs` gains a reservation array that
`cfs_commit` owns for its duration and frees on every path, success or
failure.

*Concurrency.* All of it is inside `cfs_commit` under `fs->lock`, which
that function already holds throughout. `cfs_snapshot_create` and
`cfs_snapshot_delete` are called with the mount lock held today and
still are. No new lock, no new order, so nothing for lockdep to learn.

*Memory.* One copied snaplist block per commit that changes a snapshot
entry, one copied deadlist head per commit that appends, one fresh
deadlist block per 506 held blocks. The reservation is a `uint64_t`
array of the bound, `kmalloc`ed and freed in `cfs_commit`.

*Error handling.* A reservation that cannot be taken fails the commit
before anything is published, and the existing rollback
(`cosmofs_core.c:1069-1088`) gives every reserved block back deferred and
marks the buffers clean — it is extended to the snapshot list's share
and is the same code. A copy that cannot be written fails the commit the
way any metadata write does. The one case with a choice is a snapshot
list that cannot be *read* during the fill: the commit fails rather than
proceeding, because a commit that cannot find the entry cannot honestly
say whether a block is held, and the existing "never hand a snapshot's
block back" fallbacks (`:417`, `:432`) silently leaked it on the
assumption a later commit would tidy up. There is no later commit. That
is this unit's subject.

*Security.* No new interface, no new privilege, no userland surface.

*Performance.* One or two extra block writes per commit, on filesystems
that have a snapshot *and* free something the snapshot holds; zero on
every other filesystem, because `snap_count == 0` short-circuits exactly
as it does now. Inside the flush the commit already does, so no extra
barrier. Against it: two walks of the snapshot list per freed block
become one, which is a read saved per block on precisely the filesystems
that pay the write.

*Observability.* `cosmofs_check` already claims deadlist and snaplist
blocks as metadata and is what measures the defect; the unit's headline
number is its `alloc_not_seen` count, which is the number the bound at
`cosmofstest.c:2579` was hiding.

*Future extensibility.* The orphan list, when it is built, is a chain
named by a superblock field and changed by a commit — the same shape as
this one and as the free record. Three of these and the pattern is a
pattern: `snap_cow` is written to be the general "copy a chain named by
a superblock field" helper, not a snapshot-specific one.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/filesystem/cosmofs/cosmofs_snap.c` | `snap_cow`; `deadlist_append` copies rather than edits; `cfs_snapshot_hold_block` replaced by a fill-time entry point taking the reservation; `deadlist_settle`'s frees exempt; create/delete through the copy |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | the reservation widened to cover the snapshot list; one verdict walk feeding both records in the fill; phase 7 reduced to clearing bits; the rollback extended |
| `kernel-services/filesystem/cosmofs/cosmofs_internal.h` | the reservation in `struct cfs`, and the snapshot entry points the commit calls |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | no change expected — the blocks are claimed as metadata already; the check is that it needs none |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | the new tests; `cosmofs-freelog-snapshot`'s `<= 4` becomes `== 0` |
| `kernel-services/filesystem/cosmofs/cosmofscrash.c` | **a snapshot in the replay workload**, which the suite has never had |
| docs | cosmofs `design.md` ("Not freeing what a snapshot names") and `architecture.md`, README Status, `docs/README.md`, the inventory row struck through |

## New APIs

No format change and no kernel-facing API change. Internal to cosmofs:

```c
/* cosmofs_internal.h */

/* A writable copy of the chain named by *root, published by the commit
 * that assigns it. Idempotent within a transaction. */
int cfs_snap_cow(struct cfs *fs, uint64_t *root);

/* How many blocks the snapshot list may need from this commit's
 * reservation: the chain's length, a copied deadlist head, and one
 * block per CFS_DEAD_PER_BLOCK possible holds. */
unsigned cfs_snapshot_reserve_bound(struct cfs *fs, unsigned nr_pending);

/* Called from the commit's fill, before the root: does a snapshot hold
 * `dva`, and if so record it, taking blocks from `res` rather than
 * allocating. The verdict the free record uses is this call's return
 * value -- one walk, two records. */
bool cfs_snapshot_hold_block_reserved(struct cfs *fs, uint64_t dva, struct cfs_res *res);
```

`cfs_snapshot_hold_block` goes: nothing may append to a deadlist outside
a reservation, and leaving the old entry point in place would leave the
defect reachable. The rule is enforced by
there being no other door, which is the only enforcement that survives
the next person to touch the file.

## Migration plan

1. **The copy, before anything depends on it.** `cfs_snap_cow`, and
   every in-place write to a `CFS_KIND_SNAPLIST` or `CFS_KIND_DEADLIST`
   block routed through it — create, delete, settle, append — with the
   append still where it is, after the root. Nothing about the leak
   changes in this step and the crash hazard closes. Its test is the
   crash suite with a snapshot in the workload, which fails before this
   step and passes after it.
2. **The reservation.** The bound widened and the snapshot list's blocks
   taken from it, with the fill still writing nothing new: the
   assertion is that a commit under a snapshot allocates nothing after
   `commit_bitmap`, checked by a remount and a structural check, and by
   the over-reservation showing up in the record as free.
3. **The move.** The append moved into the fill, phase 7 reduced to
   clearing bits, `cfs_snapshot_hold_block` removed. This is the step
   that closes the leak: the unmount test, and `cosmofs-freelog-snapshot`
   tightened to `== 0`.
4. **The one walk.** The fill's verdict feeds both records; the second
   walk goes. Measured, not assumed: the benchmark below.
5. **Docs, README Status, the inventory row struck through, the
   report's as-built sections.**

Each step boots both architectures; steps 1 and 3 run `make test-crash`;
step 1 runs the release build, because the new test hooks need
`CONFIG_FAULTINJECT` stubs or the release step does not compile
(`docs/verification/invariants.md:89`: `CONFIG_FAULTINJECT` defaults to
`CONFIG_DEBUG`, so a hook without a release stub fails `make
BUILD=release`, which CI runs).

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `cosmofs-snap-unmount` | **the defect**: a snapshot, a file it holds deleted in the live tree, an unmount and a remount — and the structural check is `clean`, with `alloc_not_seen.count == 0` | the unfixed tree: today this is 2 to 4 blocks, which is the bound `cosmofs-freelog-snapshot` asserts |
| `cosmofs-snap-cow` | a commit that appends to a deadlist leaves the *old* head block free and the new chain reachable only from the new root: the block numbers differ, and the old numbers are clear in the bitmap the new root publishes | edit the head in place: the numbers are equal and the next assertion, the crash one, fails |
| `cosmofs-snap-crash` (in `cosmofs-replay`) | the replay suite's workload **takes a snapshot**, deletes a file it holds, and every prefix mounts clean, reads the snapshot's copy back, and strands nothing | revert step 1: a prefix that ends between the snaplist write and the root mounts on the old root with a mutated list, and the check reports either a stranded snapshot or, worse, a live block on a deadlist |
| `cosmofs-snap-settle-live` | the sharp end of the above, deterministically: a prefix image whose deadlist names a block the surviving root's live tree still uses, deleted — and the delete must not free it. With the fix the image cannot exist, which is the assertion; the test constructs it with the poison hook and checks the checker names it | without the fix the hook is unnecessary, because an ordinary crash prefix produces it |
| `cosmofs-snap-reserved` | a commit with a snapshot present allocates **nothing** after `commit_bitmap`: every block the snapshot list gained is set in the bitmap the root published, checked by a remount finding neither a leak nor a reachable-and-free block | allocate the deadlist block in the fill instead of taking it from the reservation: the check reports `seen_not_alloc`, the direction that hands live data to the allocator |
| `cosmofs-snap-overreserve` | a commit whose bound over-reserves for the snapshot list lists the leftovers in the free record, and they are free after a remount | drop them: the free count is short by the slack on every commit that has a snapshot |
| `cosmofs-snap-rollback` | a fill that fails with the reservation outstanding (the `test_fail_freelog` hook's sibling) gives every reserved block back, publishes no root, and leaves the filesystem exactly as it was: free count, generation and check identical either side | give back only the record's share: the snapshot list's blocks stay allocated and named by nothing, and the remount's check finds them |
| `cosmofs-snap-onewalk` | the verdict is taken once: a counter incremented in `snapshot_holds` shows one walk per freed block across a commit that frees many, not two | keep phase 7's walk: the counter doubles, which is the only statement that distinguishes "both are right" from "one is asked" |
| `cosmofs-snap-delete-crash` | a snapshot deleted, the commit interrupted at every prefix: each image has either the snapshot whole with its blocks, or no snapshot and no strand — never one without the other | in-place `clear_entry`: an image has the entry gone and the blocks allocated |
| `cosmofs-freelog-snapshot` (existing) | `alloc_not_seen.count == 0`, the bound removed | the assertion is the bug-proof: it is `<= 4` today because the tree fails `== 0` |
| `cosmofs-snapshot`, `cosmofs-snapshot-delete` (existing) | unchanged behaviour: the free count still rises at exactly the point the design says, and the snapshots still read | — |

**Vacuity, named in advance.** Three of
these fail on today's tree and that is stated per row rather than
claimed in general: `cosmofs-snap-unmount`, `cosmofs-snap-crash` and the
tightened `cosmofs-freelog-snapshot`. `cosmofs-snap-onewalk` is the one
that can pass while testing nothing — a counter that is never
incremented reads zero twice — so it asserts the count is *one per
freed block*, a positive number derived from the workload, not a
difference.

**The crash suite's workload changes, and that is the point.** Adding a
snapshot to it re-prices every prefix (211 today), so step 1 reports the
new count and the new runtime, and the per-test budget is checked before
the harness fails on it rather than after
(`docs/testing/flakes.md`).

## Benchmarks

- **A commit's cost with a snapshot present, before and after**, in
  block writes and flush barriers, on the 512- and 16384-block test
  disks. The claim: one or two writes more, zero barriers more, and the
  claim fails if a barrier appears.
- **A commit's cost with no snapshot**, which must be unchanged to the
  write. The short-circuit is the whole reason most filesystems pay
  nothing.
- **Walks of the snapshot list per freed block**, before and after: two
  to one, counted rather than argued (`cosmofs-snap-onewalk`'s counter,
  reported).
- **The crash suite's stranded total and runtime** with the snapshot
  workload, which is the number this unit exists to make zero.

## Risks

- **An append published early is worse than an append lost.** The whole
  unit turns on the copy landing before the move: step 1 before step 3,
  in that order, with the crash suite between them. Doing the
  reserve-before-fill the inventory row describes *without* the copy
  produces a filesystem that hands live blocks to the allocator after a
  crash — a leak traded for corruption. The migration plan's order is
  the mitigation and it is not negotiable.
- **The regress is easy to reintroduce.** Any future `free` of a
  snaplist or deadlist block that goes through `cfs_free_block_deferred`
  instead of `..._exempt` will be held on the deadlist it is a copy of,
  and the symptom is a deadlist that grows by a block per commit — slow,
  silent, and invisible to a single-commit test. `cosmofs-snap-cow`
  watches the chain's length across a hundred commits for exactly this,
  the way `cosmofs-freelog-supersede` does for the record.
- **The bound.** A bound that is too small fails the commit, which is
  safe and loud; a bound computed from `nr_pending` *before*
  `commit_bitmap` adds to it is the way to get it wrong, and it is the
  same mistake the record's bound already accounts for by counting
  `nr_chunks + nmembers`. The over-reserve test is what keeps the
  generosity honest.
- **The crash suite gets slower.** A snapshot in the workload means more
  metadata per prefix and a structural check that walks a snapshot as
  well as the live tree. If it costs more than the budget allows, the
  answer is a second, shorter snapshot workload rather than a weaker
  check — the fsck unit's rule, that a suite which stops asserting is a
  suite that stops finding things.
- **`snap_count == 0` is the short-circuit and it is in-memory state.**
  It is set at mount by `snap_max_id` and maintained by create and
  delete. A commit that trusts it when it is stale would skip the filter
  entirely and free a live snapshot's blocks. It is trusted today at
  `cosmofs_core.c:1150` and the fill will trust it in the same way; the
  unit does not change that, and says so here rather than discovering it
  later.

## Alternatives considered

- **Reserve-before-fill without copy-on-write**, which is what the
  inventory row proposes. Rejected above: it moves an in-place mutation
  to before the root, which is the crash that frees live blocks. The
  row's guess was written from the free record's shape, where the
  record's blocks are fresh every commit and nothing is edited; the
  snapshot list is the case where that is not already true.
- **Keep the append after the root and write it synchronously.** A small
  transaction of its own after the superblock: the deadlist block and
  the snaplist block written and flushed. It works and it costs a second
  barrier pair on every commit that frees a held block, for a result the
  copy gets inside the barrier that is already there — and it leaves a
  window between the root and the second write in which a crash strands
  exactly what this unit is removing.
- **Record held blocks in the free record and let the next mount do the
  append.** The record is already durable and already written before the
  root, so a `held` flag beside each entry would carry the information
  across an unmount. Rejected: a mount is the least supervised moment
  there is (the fsck report's argument, and this report agrees with it),
  a long-lived mount would run for weeks with a deadlist missing entries
  that a snapshot deletion in that session needs, and the in-place
  mutation stays unfixed underneath.
- **Make the deadlist an in-memory structure flushed at unmount.** It
  removes the ordering question by removing the ordering, and reinstates
  it as "what if the unmount does not happen", which is the same defect
  wearing the crash's clothes.
- **Do nothing and let `fsck` reclaim it.** It is four blocks per
  unmount. It is also the answer the previous two units refused, for the
  reason that keeps being right: a defect that a record makes free is
  not a defect an operator should be told to sweep up, and the operator
  has to know to do it.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
