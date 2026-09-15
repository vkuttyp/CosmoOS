# NEXT SUBSYSTEM — the commit after the last one

Date: 2026-09-15. Tree: `main` at ac78cbe (after PR #146, the operator
interface). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§3.

**Subsystem: a record of what a transaction freed, published with the
root that freed it, so that the blocks reach the allocator whether or
not another commit ever happens.** Nothing in this report is built; the
migration plan is the plan, and the "as run" and "as built" sections are
filled by the implementation pull request. This report is to close the
inventory's §3 row that begins "No on-disk orphan list, and no record of
a deferred free", in its second clause, and the row the fsctl unit added
beside it.

Two units in a row found this and neither fixed it. The fsck unit
measured it across crashes: 162 of 199 replayed prefixes stranded
blocks, 1912 in all. The fsctl unit found the sharper half by pointing
the new operator tool at a real filesystem — **a clean unmount strands
them too**, 41 blocks from one file written and deleted, and 28 on the
boot's own scratch disk. Every unmount, on every filesystem, loses the
space its last transaction freed.

## Problem

**The frees are applied after the root, deliberately, and nothing
carries them.** `cfs_commit` (`cosmofs_core.c:715-806`) publishes the
new superblock with `BIO_PREFLUSH | BIO_FUA` (`:766`), swaps the slot,
and *then* walks `pending_free`:

```c
    /* The new root is durable: the old generation's blocks are free. */
    ...
    bit_clear(fs->bitmap, lin);
    fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
```

and says what it is counting on (`:800-802`):

```c
    /* The frees dirtied bitmap chunks for the next commit; that is
     * bookkeeping of this commit, not a new change to age. */
```

The ordering is right and the fsck report argued it: a block the old
root still names cannot be freed before the new root lands, or a crash
between the two hands out a block the surviving tree is using. The bug
is not the ordering. The bug is **"the next commit"**, which is a
promise nothing keeps.

- `first_dirty_ns = 0` on the same line, so the writeback thread will
  not pick the chunks up either (`wb_due`, `:1425`): the bits are
  deliberately not aged into a commit of their own.
- `vfs_umount` calls the filesystem's `sync` exactly once
  (`vfs.c:633`), which is one `cfs_commit`, and then `unmount`
  (`:658`). `cfs_destroy` (`:1341`) frees `bitmap`, `bitmap_dirty`
  and `pending_free` outright.

So the last transaction's frees are cleared in memory, marked for a
commit that will not happen, and thrown away. The next mount reads the
on-disk bitmap (`load_bitmap`, `:1281-1331`) and those blocks are
allocated and reachable from nothing.

**And the obvious fix does not work.** "Commit again at unmount" is the
first thing anyone would try, and it reduces the leak rather than
removing it, because a commit generates its own:

- `commit_bitmap` (`:635-689`) copies each dirty chunk into a freshly
  reserved block and hands the old one to `cfs_free_block_deferred`
  (`commit_member_bitmap`, `:624`).
- That free lands in `pending_free`, which phase 7 applies *after this
  commit's root*, dirtying chunks again.

A second commit therefore writes the first one's frees and leaves its
own. **A loop does not terminate**: every iteration has a non-empty
`pending_free` by construction, because writing the bitmap is itself a
change to the bitmap. The residue shrinks to the handful of blocks one
bitmap-only commit costs and then stays there. Committing twice would
turn "every unmount loses its last transaction's frees" into "every
unmount loses about four blocks", which is better arithmetic and the
same defect.

**Nothing else reclaims them.** `cosmofs_check` finds them, and since
PR #146 an operator can run it — but an operator reclaiming space that
every unmount loses is a workaround with a person in it.

## Current implementation

- **The commit**, `cfs_commit` (`cosmofs_core.c:715-806`): early-out
  when nothing is dirty and nothing is pending (`:719-725`);
  `commit_bitmap` (`:726`); dirty buffers written (`:731-740`);
  `sb.free_blocks = fs->free_blocks` (`:747`, excluding this
  transaction's pending frees); labels and `pool_flush` (`:753,:762`);
  the root into the alternate slot (`:766`); then phase 7.
- **The bitmap fixpoint**, `commit_bitmap` (`:635-689`): every member's
  allocation index is copied first (`:659-666`), then a loop reserves
  one block per dirty-but-unreserved chunk from that chunk's own member
  (`:667-682`) until a pass makes no progress. A dirty chunk with no
  reservation is `-EIO` (`:601-608`).
- **The deferred free list**, `cfs_free_block_deferred` (`:389-397`):
  in memory, grown by doubling from 64, with no on-disk form at all.
- **Snapshots**, which any fix must respect: phase 7 consults
  `cfs_snapshot_hold_block` (`cosmofs_snap.c:287`) when `snap_count >
  0`, which appends the block to the newest snapshot's deadlist and
  keeps its bit set. On failure it still answers true and logs "block
  %llu leaked until unmount" (`:340-344`) — a second instance of this
  report's defect, from a different direction.
- **The superblock** (`cosmofs_format.h:346-365`) has `reserved[5]`, so
  a root pointer for the record needs no growth and no version bump for
  space — though it needs one for meaning.
- **The chained-block shape** already exists: `struct cfs_dead_block`
  (`:239-243`) is `next`, `count`, and an array of block numbers, which
  is exactly the shape a free record wants.

## Why it matters

**Every unmount, on every filesystem.** Not a crash path, not an edge
case: the ordinary, clean, successful shutdown loses space. A machine
that mounts and unmounts a filesystem a thousand times has leaked a
thousand transactions' worth of blocks, and nothing in the system says
so.

**It is the last thing standing between the checker and "clean".** The
fsck unit built a pass that says whether a filesystem adds up, and the
honest answer on a real filesystem is currently "no, and that is
expected", which is a bad sentence to have to write about a
correctness tool. The crash suite already had to weaken its assertion
from *clean* to *no finding a crash cannot explain* for this reason.

**The same mechanism costs a snapshot its blocks.** When a deadlist
cannot grow, the snapshot code keeps the bit set and logs that the block
is leaked until unmount — and at unmount it is leaked for good. A record
of intent is the same answer to both.

**And it is bounded work.** The list exists in memory already, the
chained-block shape exists, the superblock has the field. This is not a
new subsystem; it is finishing a sentence the commit path already
started.

## Design

### The record

The superblock gains `free_root`: the DVA of a chain of
`CFS_KIND_FREELOG` blocks, or 0. Each block is `next`, `count`, and
block numbers — `struct cfs_dead_block`'s shape, and the report proposes
reusing that struct rather than inventing a twin.

**The chain holds exactly what phase 7 is about to clear**, after the
snapshot filter rather than before it: a block a snapshot still holds is
not freed and must not be recorded as free.

### Where it is written, and in what order

The record is built and written **before** the root, and becomes
authoritative **because** of the root. The order within the commit is
the whole of the design, and getting it wrong in either direction
corrupts the filesystem rather than merely leaking, so it is argued
here rather than asserted.

Two things make the naive order wrong:

- **Writing the record allocates blocks**, and so does the snapshot
  filter, which appends to a deadlist. `commit_bitmap` (`:635-689`) is
  what makes the bitmap on disk agree with the bits in memory, and it
  runs once. Allocating after it publishes a root whose bitmap does not
  record the record's own blocks as allocated -- so the next allocation
  hands them out and the filesystem eats its own metadata.
- **The set to record is not final until the fixpoint has run**, because
  the fixpoint frees every bitmap chunk and allocation index it copies
  (`commit_member_bitmap`, `:624`). Recording before it misses exactly
  the frees that this report is about.

Allocate-before and fill-after resolves the circle:

1. **Free the previous record.** Walk the chain `sb.free_root` names and
   hand every block to `cfs_free_block_deferred`. It is the old root's
   statement and the new root replaces it; without this step every
   commit leaks its predecessor's record.
2. **Filter for snapshots.** Run `cfs_snapshot_hold_block` over
   `pending_free` now, so the deadlist appends it makes are inside the
   transaction rather than after the root. What survives is what this
   transaction actually frees.
3. **Reserve the record's blocks** -- allocate them, do not fill them.
   The count is an upper bound, computable here: what `pending_free`
   holds now, plus what the fixpoint can add, which is one block per
   dirty chunk plus one allocation index per member. Both are known
   before the fixpoint runs (`fs->bitmap_dirty`, `fs->nmembers`).
4. **Run the bitmap fixpoint.** It now sees the deadlist blocks and the
   reserved record blocks as allocated, because they were allocated
   before it, and writes a bitmap that says so. Its own copy-on-write
   frees land in `pending_free`, which is the set step 5 records.
5. **Fill the reserved blocks.** The record is `pending_free` as it now
   stands. If the bound was generous, the leftover reserved blocks are
   **listed in the record as free**: a block that describes its own
   release, which is what keeps the bound from having to be tight.
6. Labels, `pool_flush` -- everything stable.
7. The root into the alternate slot, `BIO_PREFLUSH | BIO_FUA`.
8. Phase 7 applies the frees in memory, exactly as today.

**Why this terminates**, which the rejected alternative does not: steps
3 and 5 allocate a bounded number of blocks *once*, before the fixpoint,
so the fixpoint converges exactly as it does now and nothing after it
allocates. There is no second fixpoint and no loop -- the circularity is
broken by separating the reservation from the content, not by iterating
until it stops moving.

**The root is what makes the record true.** Before step 7 the old root
is live and the record is unreachable from it: a crash there leaves a
filesystem that never heard of those blocks, which is correct, because
the transaction that freed them did not happen. After step 7 the new
root names the new tree *and* the record, so the blocks are free and the
statement that they are free is durable. That is the property the
in-memory list never had.

Step 2 also moves the snapshot filter earlier, which the report counts
as a fix rather than a side effect: appending to a deadlist is a change
to the filesystem, and doing it after the root write means a crash can
lose the entry for a block the root already treats as held.

### Where it is applied

Two places, and the second is the point of the whole unit:

- **The next commit**, as today. It writes the dirtied bitmap chunks and
  publishes a *new* record (its own frees) or 0. The old record's blocks
  are freed by that same commit, which is the ordinary case and costs
  nothing new.
- **Mount.** If `sb.free_root != 0`, `load_bitmap` replays it: clear
  each block's bit in the in-memory bitmap and mark the chunk dirty.
  The space is available to the allocator immediately, in this session,
  before any commit.

**Replay is idempotent**, which is what makes it safe to leave the
record in place. A mount that replays and then unmounts without
committing changes nothing on disk; the next mount replays the same
list to the same effect. A block that is replayed, reallocated and
written is made durable by a commit that publishes a root naming it —
and that same commit replaces the record. A crash before that commit
leaves the block unwritten and the record still saying "free", which is
true.

So the invariant is: **a block is either reachable from the root, or
recorded as free by the root, or free in the bitmap the root names.**
There is no fourth state, which is exactly the state this defect
creates.

### What it does not do

It does not fix the *unlinked-but-open inode* half of the inventory row
it shares. An inode whose last name went while a handle held it is a
different record (an orphan list), with a different replay, and joining
them would be two designs in one unit. This report closes the
deferred-free clause and leaves the orphan clause where it is, named.

### The §70 gate

*Ownership and lifetime.* The record's blocks are allocated from the
transaction that writes them and freed by the commit that supersedes
them, like every other metadata block. The in-memory `pending_free`
array is unchanged and still owned by `struct cfs`. Nothing outlives a
mount.

*Concurrency.* All of it happens under `fs->lock`, inside `cfs_commit`,
which already holds that lock for its duration. The replay at mount runs
before the filesystem is published, with no other thread able to reach
it.

*Memory.* One chained block per `CFS_DEAD_PER_BLOCK` frees. A
transaction that frees ten thousand blocks writes a handful of blocks to
say so, and the chain is walked once at replay and dropped.

*Error handling.* A record that cannot be written fails the commit
before the root is published, which leaves the filesystem exactly as it
was — the same failure mode as any other metadata write in the
transaction. A record that cannot be *read* at mount is the interesting
case and the report chooses: **the mount fails**. A filesystem whose
root says "these blocks are free" and cannot say which is a filesystem
whose allocator cannot be trusted, and the checker exists for the
operator who wants to look. `VFS_UMOUNT_FORCE`'s discard path is
unaffected: it commits nothing, so it records nothing.

*Security.* No new interface, no new privilege, no userland surface. The
record is metadata like any other and carries block numbers, which every
metadata block does.

*Performance.* One extra block write per `CFS_DEAD_PER_BLOCK` frees per
commit, inside the flush the commit already does. No extra barrier: the
record is written before the same `pool_flush` that makes the rest
stable. The benchmark section measures it against a commit's existing
cost.

*Future extensibility.* The orphan list is the same shape — a record
published with the root and replayed at mount — so this unit's replay
path is the one that unit will extend. Format version 9, and the
version gate is on *mount*: a version-8 filesystem has no `free_root`
and replays nothing.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/filesystem/cosmofs/cosmofs_format.h` | `CFS_KIND_FREELOG` (13, after `CFS_KIND_KEYS`); `free_root` in `struct cfs_super`, taken from `reserved[5]`; `CFS_VERSION` 9 |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | the record written in `cfs_commit` before the root; the snapshot filter moved ahead of it; the replay in `load_bitmap`; the old record freed by the superseding commit |
| `kernel-services/filesystem/cosmofs/cosmofs_snap.c` | `cfs_snapshot_hold_block` called from the new place; the "leaked until unmount" path becomes a recorded free |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | the checker claims the record's blocks, like any other metadata, and must not call a recorded free a leak |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | the tests, and the ones that assert a generation or a free count across an unmount |
| `kernel-services/filesystem/cosmofs/cosmofscrash.c` | the replay suite's leak accounting, which should now measure ~0 |
| docs | cosmofs `design.md` and `architecture.md`, the fsck report's measurement annotated, README Status, `docs/README.md`, the inventory |

## New APIs

```c
/* cosmofs_format.h */
#define CFS_VERSION 9u            /* version 9: a record of deferred frees */
#define CFS_KIND_FREELOG 13u      /* a chain of blocks this root freed */

struct cfs_super {
    /* ... */
    uint64_t free_root;           /* v9: head of the FREELOG chain, or 0 */
    uint64_t reserved[4];
};
```

No kernel-facing API changes: the record is internal to the commit and
the mount. `cosmofs_check`'s report gains nothing — a recorded free is
simply not a finding.

## Migration plan

1. **The format.** `free_root`, `CFS_KIND_FREELOG`, version 9, and a
   mount of a version-8 image that replays nothing and still works.
2. **The write.** The record built and written before the root, with the
   snapshot filter moved ahead of it. No replay yet: the record is
   written and ignored, and the test asserts it is *there* and names the
   blocks phase 7 then clears.
3. **The replay.** `load_bitmap` applies it. The test is the defect
   itself: write a file, delete it, unmount, remount, and the free count
   is what it was before the file existed.
4. **The crash suite.** The replayed prefixes should now be clean rather
   than "clean except leaks". This is where the fsck unit's measurement
   is re-taken, and the assertion it had to weaken is restored.
5. **The checker.** A recorded free is metadata, not a leak: the record
   is claimed and the blocks in it are accounted.
6. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; steps 3 and 4 run `make test-crash`;
step 1 runs the release build.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `cosmofs-freelog-format` | a version-8 image mounts, works and replays nothing; a version-9 image has `free_root` 0 when nothing is pending | gate the replay on the version being ≥ 8: a version-8 image's zeroed reserved field is read as a chain and the mount fails |
| `cosmofs-freelog-written` | after a transaction that frees blocks, `free_root` is non-zero and its chain names exactly the blocks phase 7 clears, after the snapshot filter | record before the filter: a block a snapshot holds appears in the record, and the next assertion (that a snapshotted filesystem loses nothing) fails |
| `cosmofs-unmount-leak` | **the defect**: write a file, delete it, unmount, remount, and `free_blocks` is what it was before the file existed; the structural check is clean | skip the replay at mount: the free count is short and the check reports leaked blocks — which is today's behaviour, so this bug-proof is the unfixed tree |
| `cosmofs-freelog-idempotent` | mount, replay, unmount without committing, mount again: the same blocks, the same count, no double-free | clear the record at replay rather than at the next commit: the second mount loses it and the blocks are stranded again |
| `cosmofs-freelog-reuse` | a replayed block is handed out by the allocator in the same session, written, and committed; after a remount it is in use and not in any record | free the record at replay: a crash between the replay and the commit loses the record while the block is unwritten |
| `cosmofs-freelog-snapshot` | a filesystem with a snapshot holding freed blocks records none of them, and loses nothing across an unmount | as `cosmofs-freelog-written`'s injection, from the other side |
| `cosmofs-freelog-chain` | more frees than one block holds are recorded across a chain and all of them replay | write only the first block of the chain: the count is short by the overflow |
| `cosmofs-freelog-accounted` | the record's own blocks, and the deadlist blocks the filter appends, are **allocated in the bitmap the root publishes**: remount and the structural check finds neither a leak nor a block that is reachable and free | allocate the record after the bitmap fixpoint rather than before it: the check reports the record's blocks as `seen_not_alloc`, the dangerous direction, because the allocator can hand them out again |
| `cosmofs-freelog-supersede` | a hundred commits in a row leave one record and no residue: the free count after the hundredth equals the count after the first, and the check is clean | do not free the previous chain: the count falls by a block or two per commit, which a single-commit test cannot see |
| `cosmofs-freelog-overreserve` | a transaction whose bound over-reserves lists the leftover blocks in the record itself, and they are free after a remount | drop the leftovers instead of recording them: the free count is short by the slack, every commit |
| `cosmofs-replay` (extended) | every replayed crash prefix is **clean** — the assertion the fsck unit had to weaken, restored | the fsck unit's `no-crash-check` injection, which now has a stronger claim to break |
| `cosmofs-check-clean` (existing) | still clean, with the record's own blocks claimed as metadata | do not claim the record in the checker: `alloc_not_seen` names the record's blocks |

**The tests this changes, and why that is correct.** `cosmofs-format`
asserts `st.generation == 1` after an unmount and a remount, under the
comment "Unmount committed nothing new: still generation 1"
(`cosmofstest.c:129-131`). That stays true, and it is the assertion that
says this unit adds no commit -- the difference between this design and
the one it rejects. What changes is `cosmofs-ops`'s free-count
assertions across a mount cycle, which today encode the leak — they
assert the count the defect produces. Each one is re-derived in step 3,
and the report says plainly that a test asserting a wrong number is not
evidence the number is right.

**Vacuity, named in advance.** `cosmofs-unmount-leak` is the unit: it
fails on today's tree, which is the strongest possible statement that it
is not vacuous. `cosmofs-freelog-idempotent` and `-reuse` exist because
the easy implementation — clear the record when you replay it — passes
the headline test and loses the blocks on the *second* mount, which no
single-cycle test would catch.

## Benchmarks

- **A commit's cost, before and after**, in block writes and flush
  barriers, on the 512- and 16384-block test disks. The claim to check
  is "no extra barrier": the record rides the flush the commit already
  does.
- **The crash suite's leak total**, which the fsck unit measured at 1912
  blocks over 199 prefixes. The number to report is the new one, and the
  unit fails its own argument if it is not near zero.
- **A mount's cost with a long record**, to price the replay: a
  transaction that frees ten thousand blocks, unmounted, then mounted.

## Risks

- **The order is the design, and two of its steps are load-bearing in a
  way that fails silently.** Allocating the record after the bitmap
  fixpoint publishes a root whose bitmap does not know about the
  record's own blocks, so the allocator hands them out and the
  filesystem overwrites its own metadata -- a corruption, not a leak,
  and one that a single mount cycle would not show. Failing to free the
  previous chain leaks a block or two per commit, which is this report's
  own defect reintroduced one level up. `cosmofs-freelog-accounted` and
  `-supersede` exist for exactly these two, and both were found by
  review of this report rather than by writing it.
- **A format change, and this one is in the commit path.** Every write
  the filesystem makes goes through `cfs_commit`, so a mistake here is
  not a feature that misbehaves but a filesystem that loses data. The
  migration plan's step 2 writes the record without acting on it for
  exactly this reason: the write can be wrong in a way the tree survives
  before anything depends on it.
- **The snapshot filter moves.** It is called earlier, which is the
  right place, but "the right place" is a claim about a code path that
  currently appends to a deadlist after the root is durable. Step 2 is
  where that is proved, and `cosmofs-freelog-snapshot` is the test.
- **A mount that now fails.** Choosing `-EIO` over "ignore the record"
  means an unreadable record takes the filesystem offline where today it
  would mount and quietly lose space. That is the right trade for a
  correctness unit and it is a behaviour change worth stating loudly.
- **The record is one more thing a crash can tear.** It is written
  before the flush that precedes the root, so a torn record is a record
  the root does not name. The `cfs_mhdr` check catches a torn block and
  the mount refuses, which is the case above.

## Alternatives considered

- **Commit twice at unmount.** The first thing to try and it does not
  converge: `commit_bitmap` frees the blocks it copies from, so every
  commit leaves a `pending_free` for the next one. Bounded at a few
  blocks per unmount rather than a transaction's worth, which is better
  arithmetic and the same defect. It also costs a second full barrier
  pair on every unmount for a result that is still wrong.
- **Apply the frees before the root write.** Removes the record and the
  crash safety together: a crash between the bitmap write and the root
  leaves the old root naming blocks the bitmap says are free, which is
  the allocator handing out live data. This is the ordering the fsck
  report defended and it stays defended.
- **Reclaim at mount with the structural checker.** It already finds
  them, and a walk of the whole filesystem at every mount is a price
  nobody would pay for a defect that a record makes free. The fsck
  report also argued against automatic repair, and a mount is the least
  supervised moment there is.
- **Leave it to the operator, now that `fsctl` exists.** An operator
  reclaiming space that every unmount loses is a workaround with a
  person in it, and the person has to know to do it.
- **Record the frees in the journal.** There is no journal
  (`architecture.md`: "recovery is choosing the newer valid superblock
  slot; there is no journal to replay"), and introducing one to carry a
  list of block numbers would be the largest possible answer to the
  smallest part of the question.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
