# NEXT SUBSYSTEM — the invariants the checker takes on trust

Date: 2026-09-17. Tree: `main` at 0576f57 (after PR #158, thread
placement). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§3. **This report is as built** (PR #189, two years of calendar days
after it was merged unbuilt), and the banner below records where the
build differed from it.

**What the build changed, each found by building rather than reading:**

1. **The report predicted at least one of the five unfired reporting
   paths would be wrong. None were.** "Five paths that have never
   executed is five chances" was a good prior and the measurement
   disagreed with it: every one reported the right class, naming the
   right block or inode, on its first execution. What *was* wrong
   twice was my own test fixture — see items 2 and 4. Recorded because
   a prediction the build refutes is worth as much as one it confirms.
2. **The vacuity the report named in advance actually happened.** It
   warned that an overlap fixture sharing a pool block would be caught
   by the existing cross-link map and prove nothing. The first build
   declared a two-block run and allocated one, so the run claimed its
   neighbour, `dup` fired, and the guard `CHECK(r.dup.count == 0)`
   failed. The hook now allocates the wide run and refuses with
   `-ENOSPC` rather than producing a short one.
3. **The duplicate-name bitmap cannot be "sized once and reused per
   directory", because `walk_dir` recurses.** The design said exactly
   that, and it is only true if a directory is finished before the
   next is begun — which the walk does not do: it descends in the
   middle of its own entry loop, and the child's `memset` wipes the
   parent's sheet. The first build reported nothing for a real
   duplicate that sat *after* a subdirectory. The names pass is now a
   separate loop that completes before any recursion, which keeps the
   fixed budget; a stack of bitmaps would not have.
4. **The false-positive test was nearly worthless, and the arithmetic
   says so.** It used sixty-four distinct names and expected a
   collision "occasionally": in a 32768-bit map that is about a six
   per cent chance, so the bug-proof for the re-scan would have passed
   against a broken checker nineteen times in twenty. It now uses two
   names *known* to collide (`asl` and `bea`), and **asserts the
   collision first** through a debug-only `cosmofs_test_name_hash`, so
   a change to the hash fails loudly instead of quietly making the
   test vacuous.
5. **`docs/kernel-services/filesystem/cosmofs/testing.md` does not
   exist.** The affected-files table named it; cosmofs documents its
   tests in `design.md`. The same class of mistake as reserving an
   invariant number without checking which are taken.
6. **Thirteen classes, not "ten plus three".** The report counted
   eight new corruption *kinds* and five unfired reporting *paths*
   and was careful to distinguish them; the number that ends up in the
   documentation is the class count, which is ten before and thirteen
   after (the eight kinds include five that fire classes that already
   existed).

**Subsystem: the three format invariants `cosmofs_check` does not
verify, and the five corruption classes it can report that nothing has
ever made it report.**

This report **takes up** two adjacent §3 rows — the one beginning
"`cosmofs_check` claims no extent overlaps another inside one inode",
and the one beginning "`cosmofs_check`'s `chain_cycle` class has no test
that manufactures it". Neither is struck until the implementation lands.

## Problem

A filesystem checker's output is a claim: *this filesystem is sound*.
Two things weaken that claim here, and they are different in kind.

### The checker does not check three things the format requires

`docs/kernel-services/filesystem/cosmofs/design.md` states the extent
invariant as a property of the format:

> Runs are sorted by `lblk` and never overlap; a logical block that no
> run covers is a **hole** and reads as zeros.

`cosmofs_check` does not verify either half. To its credit **it says so**
— the design document has a "What it does not check" section that names
all three gaps — so this is a documented limitation rather than a false
claim, and that distinction is why this report is about closing a gap
rather than correcting a lie. What is missing:

| invariant | what happens when it is violated | caught today? |
| --- | --- | --- |
| no two runs in one inode cover the same `lblk` | two extents claim one logical block; which one a read gets depends on the walk order | **only** if the runs also point at the same *pool* block, where `block_seen` catches it as a cross-link. An overlap in logical space with distinct pool blocks is invisible |
| runs are sorted by `lblk` | a lookup that assumes order can stop early and read a hole where data exists | no |
| no name appears twice in one directory | two entries resolve to different inodes; unlink removes one and the name survives | no |

The third is the one with a real design obstacle, and the design document
names it exactly: the pass "keeps maps of numbers and that needs a set of
strings".

### And five classes it reports are fired by nothing

`dir_bad` is reported from six places in `cosmofs_check.c` and
`chain_cycle` from five. Between them, **five distinct corruptions can be
reported and no test has ever produced one**:

| class, site | the corruption | fired by a test? |
| --- | --- | --- |
| `dir_bad` (`:507`) | an entry whose type disagrees with its inode | yes — `COSMOFS_CORRUPT_DIRENT` |
| `dir_bad` (`:334`) | an inode slot whose number is not its position | yes — `COSMOFS_CORRUPT_INO_SLOT` |
| `dir_bad` (`:189`) | a block pointer outside the pool's range | **no** |
| `dir_bad` (`:402`) | a snapshot member table whose count does not fit its block | **no** |
| `dir_bad` (`:498`) | an over-long `namelen` | **no** |
| `dir_bad` (`:524`) | a directory reached from two parents | **no** |
| `chain_cycle` (five sites) | a chain that returns to itself. **Two different guards**: the extent chain at `:247` is bounded by `CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2` (about 18); `:364`, `:413` and `:451` use `CFS_CHECK_MAX_CHAIN` (4096) and `:477` uses `CFS_CHECK_MAX_DEPTH` (64) | **no** |

A reporting path with no test is a path that has never executed. These bounds are numbers nothing has ever
reached: if a guard were off by one, or its counter reset in the wrong
place, or the class reported the wrong block, nothing in this tree would
say so. This unit tests the **extent** walker's guard, which is the
cheapest to reach; the other four sites stay untested, and the report
says so rather than implying one test covers five walkers.

## A live instance, found while writing this

While this report sat in review, CI failed `cosmofs-orphan-reserved` on
**three of three** aarch64 runs of a **documentation-only** branch, and
on none of three local boots of the same tree. Two of the three
signatures, the first and the cleanest:

```
[ERROR] cosmofs: block 104: bad metadata header or checksum
[ WARN] cosmofs: check: 0 leaked, 39 free-in-use, 0 cross-linked, 0 bad nlink,
                        13 orphan, 0 dangling, 0 bad entries, 1 counters, 0 cycles, 1 unreadable

[ERROR] cosmofs: block 88: bad metadata header or checksum
[ WARN] cosmofs: check: 0 leaked, 0 free-in-use, 0 cross-linked, 0 bad nlink,
                        0 orphan, 0 dangling, 0 bad entries, 0 counters, 0 cycles, 1 unreadable
```

The second is the defect and the first is the same defect plus its
consequences: a walk cannot follow a tree through a block it cannot
read, so the 39 free-in-use and 13 orphan are downstream of the one
unreadable block, not separate findings.

It was **diagnosed and fixed in PR #160**, and it was not a checksum
bug. cosmofs starts its writeback thread lazily on the first dirty
buffer, and a mount's own replay dirties buffers, so the thread was
being started from inside the replay -- where it takes only the mount's
sync lock, which the mount path does not hold, and commits a half-built
filesystem across an `fs->bufs` the mount walks with `fs->lock` unheld.
Two threads on one intrusive list. It was **not** this unit's to fix:
the diagnosis, the fix and the inventory row that records them are
#160's, and that has now merged. This unit's two rows are the ones
below it, struck when this lands.

It is this unit's to learn from, in three ways.

**The checker did its job.** It is the thing that noticed, and the
finding is exactly the kind the three unchecked invariants would extend
the reach of.

**A different view outranks another instance of the same one.** Three
aarch64 runs of "one metadata block will not verify" said no more than
the first had. What named the cause was the *other* architecture in the
same run: x86_64 was failing too, with a panic whose backtrace named the
thread and the call chain. The reach this unit is about is reach across
*classes* of finding, not more of one.

**And the test made the finding harder to read than it had to be.** The
first failure was `CHECK(r.clean)` — a boolean. `report_clean` is ten
counts and the checker *logs all ten* on the way out, but the assertion
discards that, so the first run said only "not clean" and the class had
to be recovered from a `kwarn` in the serial log. The second run
happened to fail one assertion earlier, on `seen_not_alloc`, which is
why the class is known at all.

So this unit adds one thing the two inventory rows did not ask for: **a
test that asserts a clean report says which class was not clean.** A
helper that prints the non-zero counts on failure, used by every test
that asserts `r.clean`, costs a few lines and is the difference between
a finding and a mystery.

## Why it matters

- **A clean `fsck` is the basis for trusting a repair.** Four of the ten
  classes are repaired automatically with `COSMOFS_CHECK_REPAIR`. A pass
  that walks a filesystem with overlapping extents and calls it sound is
  a pass that will later repair something else on that filesystem and
  report success.
- **`/dev/fsctl` put this in an operator's hands** (PR #146). "No
  findings" is now an answer a person acts on.
- **The crash suite runs `cosmofs_check` over every replayed image**
  (`cosmofs-replay`, 410 prefixes in the last run). Every one of those
  images is being checked against invariants the checker does not test,
  so the suite's silence on extent ordering means nothing at all.
- **Five reporting paths have never run.** That is not a gap in coverage
  so much as five pieces of code whose behaviour is unknown.

## Current implementation

`cosmofs_check.c` walks the tree once, keeping bitmaps of numbers: a
`block_seen` map (which catches two claims on one pool block) and an
inode map, both sized from the pool and **allocated in full before the
walk starts**, so a filesystem too large for the available memory is
`-ENOMEM` rather than a half-finished answer. That discipline is the
constraint every addition here has to live inside.

Corruption for tests goes through one door,
`cosmofs_test_corrupt(mnt, kind, ino, what)`
(`kernel/include/kernel/cosmofs.h`), with nine kinds today. Every check
this unit adds gets a kind, which is what makes each one provable.

## Design

### Extent overlap and ordering: one pass, no new memory

Both invariants are about the runs of a single inode, and the checker
already reads every extent of every inode. Neither needs a new map:

- **Ordering first**: `lblk` must be strictly greater than the previous
  run's `lblk`.
- **Overlap second, and only on a pair already known to ascend**:
  `this->lblk < prev->lblk + prev->count`.

**The order of those two tests is not a detail.** Tested on an unordered
pair, every descending pair also looks like an overlap — and the test
below requires an ordering fault *not* to be reported as one. So a pair
that fails the ordering test is reported as `extent_order` and the
overlap test is skipped for it: there is nothing useful to say about the
extent of an overlap between runs that are not in order.

**And the end calculation is 64-bit.** `lblk` and `count` are both
32-bit in `struct cfs_extent`, so `lblk + count` overflows in 32-bit
arithmetic for a run near the 2^32-block file bound — wrapping small and
making a real overlap invisible. The production mapper already promotes
this sum; a checker that did not would carry the bug it is looking for.

So one `prev` per inode, checked as the extents stream past, closes both.
The cost is a comparison per extent and no allocation, which is the point:
the memory discipline above is not disturbed.

An unordered run is reported as `extent_order` and an overlapping one as
`extent_overlap` — two new classes rather than one, because they are
different repairs (an ordering fault may be repairable by sorting; an
overlap is a data loss and is not).

### Duplicate names: the design obstacle, and what it costs

The pass keeps maps of numbers. Names are strings, and a directory may
hold many. Three options, and the reason for the choice:

1. **Sort each directory's names and scan for adjacent equals.** Needs
   the whole directory in memory at once — unbounded in the worst case,
   and the pass has a fixed budget.
2. **A hash set per directory, sized to the entry count.** One allocation
   per directory, freed before the next; bounded by the largest directory
   rather than the filesystem. Collisions need the names compared, so the
   entries have to be re-read on a hit.
3. **Hash every name into a per-directory Bloom-ish bitmap and re-scan
   the directory only when a hit says "maybe".** Fixed memory, and the
   re-scan is the exact case where correctness matters.

**Option 3.** It keeps the fixed-budget property the pass already has —
a bitmap sized once, reused per directory — and pays its cost only on a
collision, which on a sound filesystem is rare and on a corrupt one is
the finding. The false-positive rate is a tunable, and a false positive
costs a re-scan, not a wrong answer. Option 2 is the fallback if the
re-scan proves too expensive for large directories, and the report says
so rather than pretending the choice is obvious.

The class is `dir_dup_name`, reported once per duplicate name.

### The five unfired classes, and eight corruption kinds in all

Each gets a `COSMOFS_CORRUPT_*` kind, because the tree already made that
the way to fire a class:

| new kind | what it writes |
| --- | --- |
| `COSMOFS_CORRUPT_EXTENT_OVERLAP` | a second run in one inode covering an `lblk` the first already covers, pointing at a different pool block |
| `COSMOFS_CORRUPT_EXTENT_ORDER` | two runs swapped, so `lblk` descends |
| `COSMOFS_CORRUPT_DUP_NAME` | a directory entry whose name copies another entry's, pointing at a different inode |
| `COSMOFS_CORRUPT_CHAIN_CYCLE` | an **extent**-chain block whose next pointer names an earlier block of the same chain |
| `COSMOFS_CORRUPT_BAD_PTR` | a block pointer past the end of the pool |
| `COSMOFS_CORRUPT_NAMELEN` | an entry whose `namelen` exceeds what its slot can hold |
| `COSMOFS_CORRUPT_TWO_PARENTS` | a directory named by an entry in a second directory |
| `COSMOFS_CORRUPT_SNAP_MEMBERS` | a snapshot member count larger than its block can hold |

**Eight kinds**, and the arithmetic, because an earlier draft of this
report gave three different numbers for it: three kinds for the new
checks above (overlap, order, duplicate name), four for the unfired
`dir_bad` sites (bad pointer, snapshot members, namelen, two parents),
and one for the cycle. Eight. The **five** elsewhere in this section
counts *reporting paths*, which is a different thing and is now labelled
as one everywhere it appears.

### What it does not do

- **No repair for the new classes.** An overlap is data loss and an
  ordering fault may be either; choosing a survivor is a policy question
  this report does not answer. They are reported, like six of the ten
  existing classes.
- **`next_ino` stays uncompared.** The design document calls it a
  high-water mark rather than a total, which is a deliberate choice, not
  an oversight. Reading it as a total would make a legal filesystem look
  corrupt.
- **No new on-disk format**, no version bump: every check reads what is
  already there.
- **No change to the fixed-budget property.** If the duplicate-name
  bitmap cannot be allocated, the pass reports `partial` exactly as it
  does for its existing maps.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | the ordering and overlap comparisons; the duplicate-name bitmap and re-scan; three new classes in the report |
| `kernel/include/kernel/cosmofs.h` | `extent_order`, `extent_overlap`, `dir_dup_name` in `struct cosmofs_check_report`; **eight** `COSMOFS_CORRUPT_*` kinds |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `cosmofs_test_corrupt` writes the **eight** new corruptions |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | a test per class; **a helper that prints a report's non-zero counts**, and every existing `CHECK(r.clean)` swept to use it |
| `userland/bin/fsctl.c` (or wherever the report is printed) | the three new counts |
| `docs/kernel-services/filesystem/cosmofs/design.md` | "What it does not check" shrinks to `next_ino`, with the reason it stays |
| `docs/kernel-services/filesystem/cosmofs/testing.md` | the new tests |

## Migration plan

1. **The two extent checks**, which need no memory: one `prev` per
   inode. With `COSMOFS_CORRUPT_EXTENT_OVERLAP` and
   `COSMOFS_CORRUPT_EXTENT_ORDER` and their tests.
2. **The five unfired classes**, each with its corruption kind and its
   test — *before* the duplicate-name work, because they are the cheap
   half and they exercise reporting paths that have never run. Expect at
   least one of them to be wrong: five paths that have never executed is
   five chances.
3. **Duplicate names**: the bitmap, the re-scan on a hit, and the
   allocation folded into the existing up-front budget.
4. **The crash suite re-run.** `cosmofs-replay` checks 410 images; with
   three new checks it is checking three new things about each of them,
   and if any replayed image trips one, that is a finding about the
   filesystem rather than about this unit — and the most valuable
   possible outcome.
5. **The `r.clean` diagnostic helper**, and a sweep of every existing
   assertion that a report is clean, so each says *which* class was not.
   This is what the live instance above added to the unit, and it is
   last because it is the one step that touches tests this unit did not
   write.
6. Docs, README Status, the two inventory rows struck, as-built/as-run.

Each step boots both architectures; step 3 runs `make BUILD=release`;
the whole CI step list (`host-test`, `fuzz`, `analyze`, `reproducible`,
`test-gic`, `test-guard`, `test-crash`, `test-wxn`) runs before the pull
request, because `make test` is a third of what CI does and this unit
touches a fuzzer's target.

## Tests

| test | what it asserts | bug-proof |
| --- | --- | --- |
| `cosmofs-check-extent-overlap` | two runs in one inode covering the same `lblk`, pointing at *different* pool blocks, are reported as `extent_overlap` | remove the comparison: **run, this test alone fails** — the pass calls the filesystem sound, which was the behaviour before this unit |
| `cosmofs-check-extent-order` | two runs whose `lblk` descends are reported as `extent_order`, and **not** as an overlap | test overlap *first*: **run, this test alone fails** — the descending pair is reported as an overlap and the two classes stop meaning different things |
| `cosmofs-check-dup-name` | the same name twice in one directory, pointing at two inodes, is reported once as `dir_dup_name`. **As built** it also asserts the opposite half with two names *known* to collide (`asl`, `bea`), and asserts the collision itself first so the half cannot go vacuous | drop the re-scan and trust the bitmap hit: **run, 1 failure**, at `r.dir_dup_name.count == 0` — a sound filesystem is reported as having a duplicate |
| `cosmofs-check-chain-cycle` | an inode's **extent chain** whose last block points back at an earlier block of the same chain is reported as `chain_cycle`, and the pass terminates | **remove the guard at `cosmofs_check.c:247`**: the walk does not terminate and the boot times out at 180 s, which is what that guard prevents. Raising `CFS_CHECK_MAX_CHAIN` proves nothing — the chain is still cyclic, the guard still fires after more iterations, and that constant does not govern this walker at all |
| `cosmofs-check-bad-ptr` | a block pointer past the pool's end is `dir_bad`, and the pass survives it rather than indexing the seen map with it | remove the range report: **run, this test alone fails** |
| `cosmofs-check-namelen` | an over-long `namelen` is `dir_bad` | remove the length test: **run, this test alone fails** |
| `cosmofs-check-two-parents` | a directory named from two directories is `dir_bad` | remove the second-parent report: **run, this test alone fails** |
| `cosmofs-check-snap-members` | a snapshot member count too large for its block is `dir_bad`, **and the walk still reads what the block does hold** — the half the site's own comment is about | remove the fit report: **run, this test alone fails** |
| `cosmofs-replay` (existing) | unchanged, now also checking the three new invariants over 410 images | — |
| every test asserting `r.clean` (existing) | on failure, reports **which** classes were non-zero rather than only that the report was not clean | assert the boolean alone: a failure says "not clean" and the class has to be recovered from the serial log, which is what happened to the live instance above |

**Vacuity, named in advance.** `cosmofs-check-extent-overlap` is the one
at risk: the existing `block_seen` map already catches an overlap *that
shares a pool block*, so a careless test would be caught by the old code
and prove nothing about the new. The corruption therefore points the
second run at a **different** pool block, which is exactly the case the
design document says is invisible today. The bug-proof confirms it: with
the new comparison removed, the pass reports nothing.

## Benchmarks

- **`cosmofs_check` wall time** over the boot's scratch filesystem,
  before and after. The extent checks are a comparison per extent; the
  claim is that they do not show, and the benchmark is what makes that a
  measurement.
- **The duplicate-name cost**, separately, because it is the one that
  re-reads a directory on a hit: time a directory of many names with no
  duplicates (the false-positive path) against one with a duplicate.
- **`cosmofs-replay`'s total**, which is 410 checks and is already the
  slowest test in the suite at ~13 s. If three new checks move it, the
  crash suite's budget is where it shows.

## Risks

- **A new check that fires on a sound filesystem is worse than no
  check**, because `cosmofs-replay` runs the checker over 410 images on
  every boot and a false positive fails the suite. The extent checks are
  exact comparisons, so the risk is concentrated in the duplicate-name
  bitmap — which is why its false positives resolve by re-reading rather
  than by reporting.
- **Five paths that have never executed are five chances to be wrong.**
  Step 2 puts them first for that reason. If one of them reports the
  wrong block, or the guard is off by one, the test is how it is found;
  the report predicts at least one and will say which if it happens.
- **A replayed image may trip a new check.** That would be a real finding
  about cosmofs, not a bug in this unit, and the plan treats it as the
  best outcome rather than an obstacle.
- **The memory budget.** The pass allocates everything up front so it can
  answer `-ENOMEM` honestly; a per-directory bitmap must join that
  discipline rather than allocating inside the walk.

## Alternatives considered

- **Check extents against each other using the existing `block_seen`
  map.** It already catches the sharing case. Rejected because the case
  it misses — distinct pool blocks, same logical block — is precisely
  the one the format forbids and the document admits to missing.
- **Sort each directory's names.** Simplest and exact, rejected for the
  memory: the pass's whole discipline is a fixed budget taken before the
  walk.
- **Report duplicate names without resolving collisions** (trust the
  bitmap). Rejected: a false positive on a sound filesystem fails
  `cosmofs-replay` on every boot, which is the one failure mode a
  checker must not have.
- **Leave the unfired classes alone and only add the three checks.**
  Rejected because the two rows are one subject: a checker is worth what
  it checks *and* what its reports are known to do.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
