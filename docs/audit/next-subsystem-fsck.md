# NEXT SUBSYSTEM — the blocks nobody can reach

Date: 2026-09-15. Tree: `main` at 15a19c0 (after PR #142, the symlink
unit). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: a structural checker for cosmofs — a pass that walks the
live tree and every snapshot, counts what is reachable, and compares it
with what the filesystem says about itself: the allocation bitmaps, the
link counts, the inode map, and the superblock's own totals.** Nothing
in this report is built; the migration plan is the plan, and the "as
run" and "as built" sections are filled by the implementation pull
request. This report is to close the first clause of the inventory's §3
row "no fsck; no checksum algorithm id in the metadata header" (audit
8.5, 8.6). What that row keeps is named at the end.

cosmofs already reads everything it owns. `cosmofs_scrub`
(`kernel-services/filesystem/cosmofs/cosmofs_scrub.c:265-295`) walks
both superblock slots, every allocation bitmap, every snapshot list and
deadlist, the inode map, and every inode's extents, checksum tree and
data blocks, verifying each against the checksum that block records for
itself and rewriting a rotten copy from a good mirror. It answers one
question completely: *is every block I can name still the block I
wrote?*

It does not ask the other question: *do the blocks I can name add up?*
Nothing in the tree checks that a block the allocator believes is free
is not also the third extent of somebody's file, that an inode's `nlink`
equals the number of directory entries that name it, that every
allocated block is reachable from the root or from a snapshot, or that
the superblock's `free_blocks` and `inode_count` describe the
filesystem underneath them. The scrub explicitly skips an inode whose
`nlink` is zero (`cosmofs_scrub.c:201-202`), which is exactly the inode
a checker exists to find.

And there is a leak the design admits by omission. An inode's blocks are
freed in `cfs_evict` (`cosmofs.c:1983-1996`), when the last reference to
an `nlink == 0` vnode drops. There is **no orphan list and no on-disk
pending-delete record**, so a crash between the unlink and the eviction
leaves an inode with `nlink == 0`, its blocks still marked allocated,
and no name anywhere that reaches it. The space is gone until the
filesystem is reformatted. The snapshot code has a second, narrower one:
when a deadlist cannot grow it logs `snapshot deadlist full (%d); block
%llu leaked until unmount` (`cosmofs_snap.c:341`) and carries on. Both
are the same class, and both are invisible.

## Problem

**Nothing compares what is used with what is marked used.** The
allocation state lives in per-member `ALLOCIDX` → `BITMAP` chunks
(`cosmofs_format.h:90,268-280`), loaded at mount by `load_bitmap`
(`cosmofs_core.c:1096-1147`), which already notices one discrepancy and
shrugs at it: `free block count %llu differs from the superblock's %llu;
using the bitmap` (`cosmofs_core.c:1143-1145`). That warning is the only
cross-check in the filesystem, it fires after the fact, and it prefers
one of two disagreeing sources without asking why they disagree.

**Nothing checks a link count.** `nlink` is set at creation
(`cosmofs.c:1345,1362,1421`), decremented in `cfs_unlink_common`
(`cosmofs.c:1527-1570`) and moved by rename (`cosmofs.c:1728-1737`).
Every one of those is a write to an inode and an entry in a directory
block, in one transaction — but if the two ever disagree, whether from a
bug or from a torn history no replay noticed, nothing says so. A
directory entry naming an inode whose slot is free reads as `-ENOENT` at
lookup and as nothing at all from the outside.

**An orphan is invisible and permanent.** As above: no orphan list, so
the unlinked-but-open file that a crash interrupts keeps its blocks
forever.

**The crash suite proves the data and not the shape.**
`selftest_cosmofs_replay` (`cosmofscrash.c:313-403`) replays every write
prefix of a workload, mounts the result, checks that every file
committed before the last sync is present and byte-exact, and walks the
tree reading everything (`check_prefix`, `cosmofscrash.c:269-310`). A
prefix that mounted and read correctly but left a block allocated to
nobody would pass. Dozens of images are built and thrown away every
boot, each one an opportunity to ask a question nobody asks.

**And there is no way to ask.** `cosmofs_scrub` is reachable only from
the kernel's own self-tests (`kernel/include/kernel/cosmofs.h:70`; every
caller is in `cosmofstest.c`). There is no cosmofs syscall, no ioctl,
and `sysctl` carries read-only strings with no `fs.*` names
(`kernel/syscall/native.c:1661-1673`). Whatever this unit builds has to
decide how an operator reaches it.

## Current implementation

- **Scrub.** `cosmofs_scrub(struct mount *, struct cosmofs_scrub_stats *)`
  (`cosmofs_scrub.c:265-295`) under `fs->lock` for the whole walk;
  metadata by its own header (`cfs_mhdr_ok`: magic, kind, its own DVA,
  CRC32C, `cosmofs_format.h:76-83`), data by the inode's recorded
  checksum; repairs only by rewriting a good copy over a bad one.
- **The structures a checker must walk.** Superblock
  (`cosmofs_format.h:346-365`, two slots, no dirty flag, `reserved[5]`);
  metadata header (`:76-83`); inode (`:245-263`, 256 bytes, 15 per
  block); extent and extent block (`:144-148,189-192`); directory entry
  (`:338-344`, 64 per block, `ino == 0` free); the inode map (IMAP1 →
  IMAP0 → INODES); the member table (`:268-290`); snapshots and
  deadlists (`:199-243`).
- **The primitives.** `cfs_inode_read` / `cfs_inode_read_at`
  (`cosmofs_core.c:499,483`), `extents_load` (`cosmofs.c:60`), `cfs_map`
  and `cfs_map_ext` (`cosmofs.c:203,224`), `dir_read_block`
  (`cosmofs.c:908` — takes a `struct vnode *`, so a checker that walks
  inodes rather than vnodes needs an inode-shaped variant),
  `cfs_buf_get` (`cosmofs_internal.h:125`), and
  `cfs_snapshot_references` (`cosmofs_snap.c:162-189`), which decides
  whether a block is still held by a snapshot.
- **The crash suite.** `check_prefix` mounts the replayed image
  (`cosmofscrash.c:273`), compares against the last sync point, walks
  and reads (`:191,300-306`), unmounts (`:308`).

## Why it matters

A filesystem that cannot be checked cannot be trusted with anything that
matters, and this one now carries snapshots, compression, encryption,
mirrored members and — since last week — symbolic links, each of which
adds a way for the accounting to drift. The scrub answers for the bytes;
nothing answers for the shape.

The leak is not hypothetical. Unlink a file that a process still has
open, crash before the process exits, and the blocks are unreachable and
still allocated, on every mount, forever. That is the oldest bug in
filesystems and every mature one has both halves of the answer: an
orphan record to make recovery possible, and a checker to find what was
lost anyway. This unit builds the checker, which is the half that also
tells you whether the other half is working.

The crash suite is the second reason. It already manufactures dozens of
post-crash images per boot and asks them one question. Asking a second
question of the same images costs nothing and turns every replay into a
structural proof — including of the checker itself, since a
crash-consistent image that the checker calls unsound is the checker
being wrong.

## Design

### One pass, two maps

`cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out,
unsigned flags)` runs under `fs->lock`, as the scrub does, on a mounted
filesystem. It builds two bitmaps of `total_blocks` bits and one array
of counted link counts, and then walks.

**Sharing is the normal case, and the maps have to say so.** A snapshot
holds the live tree's blocks on purpose: a copy-on-write root, every
unchanged inode block, every unmodified extent. A map that called the
second visit a duplicate would report a cross-link for every block a
snapshot preserves, which is most of them. So the two maps are not
"seen" and "seen twice" but:

- ***seen***: any structure anywhere -- live tree, any snapshot, any
  bitmap or index or deadlist -- claims this block. This is the map the
  allocation bitmap is compared against, and it is a union: a shared
  block is marked once and marked again harmlessly.
- ***live***: the **live tree** claims this block as file or directory
  content. Only this map detects duplication, and only within itself: a
  block claimed twice by the live tree is two files sharing one extent,
  which is the corruption. A block in *live* that a snapshot also holds
  is not in question at all.

Each structure is visited once: the deadlist chains are walked in the
snapshot step alone (the fixed-structure step walks the snapshot *list*,
not the lists hanging off it), and a block reached twice through the
same chain is a cycle, reported as `dir_bad`'s metadata sibling
(`chain_cycle`) rather than silently followed.

The walk:

1. **The fixed structures**: both superblock slots, the member table,
   the inode map's three levels, the allocation index and its chunks,
   the snapshot list and every deadlist chain. Each block is marked in
   *seen*; a block already there goes in *dup*.
2. **Every inode** in the map with `ino != 0`: its inode block (from
   the map walk), its extent chain, its checksum-tree blocks, and every
   data block its extents name. Extents are validated the way the tree
   already validates them (`extent_valid`, `cosmofs.c:37-56`) and, in
   addition, checked for overlap within the inode and ordering by
   `lblk`.
3. **Every directory**, from the root inode down, reading blocks
   through an inode-shaped `dir_read_block`: each entry's `namelen`,
   its `type` against the mode nibble of the inode it names, that the
   inode is allocated, and that no name repeats in a directory.
   Reachability is recorded per inode, and each entry counts one
   reference toward that inode's expected `nlink` (a directory's own
   `..` counting as the parent's, as `cfs_create_common` does when it
   sets `nlink = 2`).
4. **Every snapshot**: its `imap_root` tree walked the same way (through
   `cfs_inode_read_at`, which is already snapshot-aware), its
   `alloc_root` bitmaps, and its deadlist chain. A block a snapshot
   holds is *seen*: it is not a leak, and this is the step whose absence
   would make the checker condemn every snapshotted filesystem.

Then it compares:

| finding | what it means |
| --- | --- |
| `alloc_not_seen` | the bitmap says allocated, nothing reaches it — a leak |
| `seen_not_alloc` | something reaches it, the bitmap says free — the dangerous one: the allocator may hand it out |
| `dup` | two **live-tree** inodes claim the same block — a cross-link (a block shared with a snapshot is not this) |
| `nlink_wrong` | an inode's `nlink` differs from the entries that name it |
| `orphan` | an inode allocated and unreachable (`nlink == 0` with blocks, or `nlink != 0` with no name) |
| `dangling_entry` | an entry naming a free or out-of-range inode slot |
| `dir_bad` | a malformed entry, a repeated name, a type that disagrees with its inode |
| `counter_wrong` | `free_blocks`, `inode_count` or `next_ino` disagreeing with the walk |
| `chain_cycle` | a metadata chain (extent, deadlist, snapshot list) that revisits a block |
| `unreadable` | a metadata block that could not be read: the answer is partial |

The report carries a count per class and the first `CFS_CHECK_NAMES`
(8) offenders of each with enough to find them (an inode number, a
block, a parent and a name), because "1 leak" is a fact and "inode 41's
second extent" is a diagnosis.

### What it repairs, and what it refuses

`COSMOFS_CHECK_REPAIR` is off by default; the pass is read-only and
holds no transaction. With it on, and only with it on, the checker
repairs exactly four classes, each of which has one right answer:

- **`alloc_not_seen`**: clear the bit. Nothing references the block.
- **`orphan`**: free the inode's blocks and zero the inode, which is
  what `cfs_evict` would have done.
- **`nlink_wrong`**: set `nlink` to the counted number. The directory
  entries are the filesystem's own answer to who names an inode.
- **`counter_wrong`**: write the counted totals.

It refuses the rest, and the refusals are the interesting half:

- **`seen_not_alloc`** is not repaired by setting the bit, because a
  block both reachable and free may already have been handed to someone
  else; the filesystem is mounted, and the honest answer is to report
  it, mark the filesystem for attention and let an operator unmount.
- **`dup`** is never repaired: choosing which of two inodes keeps a
  block is data loss dressed as a fix.
- **`dangling_entry` and `dir_bad`** are reported. Removing an entry
  destroys the only name a file has; that is a decision for whoever
  reads the report.

Every repair happens in one transaction and the pass re-runs afterwards.
What the second pass must show is **the repaired classes empty and the
refused ones unchanged** -- not `clean`, which a filesystem carrying a
cross-link can never be, and demanding it would report a correct partial
repair as a repair bug. The report therefore carries `repaired` and
`remaining` per class, and the rule is: every class the repair claims,
zero; every other class, the count it had before.

### Where it is called from

Three callers, and no new system call:

1. **The self-tests**, as the scrub is called, through
   `cosmofs_check` in `kernel/include/kernel/cosmofs.h`.
2. **The crash suite**, in `check_prefix` after the mount succeeds
   (`cosmofscrash.c:273`) and before the unmount (`:308`): every
   replayed prefix of the *existing* workload must be structurally
   sound, with no finding of any class. This is the unit's sharpest
   test in both directions -- it asserts the filesystem's crash
   behaviour and it asserts the checker does not cry wolf on an image
   that is merely mid-history.

   The unlinked-but-open workload is deliberately **not** added to that
   suite, because a prefix taken after its unlink *must* show an orphan
   and the generic assertion would then have to be weakened for every
   prefix of every workload. It gets its own test
   (`cosmofs-crash-orphan`), which asserts the opposite: that the orphan
   is there, that it is the only finding, and that repair reclaims
   exactly the blocks the file held.
There is deliberately no third caller, and the reason is worth stating
rather than discovering in the implementation. An operator interface
needs a *name for a mount*, and this tree has none: procfs is a
per-process hierarchy (`procfs.c:183-248`) with no mounts directory and
no stable mount identity, mount points are not unique across mount
namespaces, and a file held open across an unmount would need the
procfs node to pin the target mount -- a reference rule procfs does not
have today. Inventing all three inside a checker unit would be three
designs in a trench coat. The pass is therefore kernel-facing in this
unit; the operator interface (a mount identity, a procfs node that holds
a reference to its target, and whether repair is ever reachable from
userland) is named as the next unit and gets an inventory row.

### The §70 gate

*Ownership and lifetime.* The two bitmaps and the link-count array are
the pass's own, allocated at entry and freed at exit; nothing outlives
the call. The pass takes `fs->lock` for its duration, as the scrub does,
so no mutation interleaves with the walk and the answer describes one
moment.

*Concurrency.* Holding `fs->lock` across a whole-filesystem walk is what
the scrub already does, and it is the reason the pass is a diagnostic
rather than something a file server runs hourly. The report says how
long it took so a reader can see what it costs.

*Memory.* Two bits per block and one 32-bit count per inode:
`total_blocks / 4` bytes plus `inode_count * 4`. That is bytes for the
test disks (512 and 16384 blocks) and 64 MiB for a 1 TiB filesystem,
which is well past what `kmalloc` will hand out -- `KMALLOC_MAX_SIZE` is
4 MiB (`kernel/include/kernel/kmalloc.h:23`, PMM order 10), so a single
allocation caps the checker at a 128 GiB filesystem and would fail
*deterministically* above it rather than under pressure.

The maps are therefore **chunked**: an array of page pointers, one
4 KiB page per 32768 blocks, each page from `pmm_alloc_page`, with an
accessor that indexes page then bit. The pointer array itself is one
`kmalloc` of `total_blocks / 32768 * 8` bytes -- 256 KiB for that 1 TiB
filesystem, comfortably inside the slab path. The link-count array is
chunked the same way. Every chunk is allocated up front, so the pass
returns `-ENOMEM` before it starts rather than half way through a walk,
and the report says how much it took.

*Error handling.* An unreadable metadata block is a finding
(`unreadable`), not an abort: the pass records it, marks every block it
could not enumerate as unknown, and reports at the end that its answer
is partial -- a checker that stops at the first bad block is the least
useful thing at exactly the moment it is needed. The scrub is the tool
for unreadable blocks and the report says so.

*Security.* The `/proc` file exposes block numbers and inode numbers of
a filesystem the reader can already stat; it is readable by root only,
as the rest of `/proc/fs` is. Repair is not reachable from userland at
all in this unit.

*Performance.* One read of every metadata block and no reads of data
blocks (the checker validates extents, not their contents -- the scrub
reads data, and doing it twice would make the pass cost what the scrub
costs for no new answer). The benchmark below measures it on the test
disks.

*Future extensibility.* The report structure is the seam: an offline
checker over a block device, a maintenance system call, and an orphan
list that makes most of the `orphan` class impossible in the first place
all consume or produce the same findings.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | new: the pass, the two maps, the eight checks, the four repairs |
| `kernel-services/filesystem/cosmofs/cosmofs_internal.h` | the inode-shaped directory read, shared with `cosmofs.c` |
| `kernel-services/filesystem/cosmofs/cosmofs.c` | `dir_read_block` split so the checker and the VFS path share one reader |
| `kernel/include/kernel/cosmofs.h` | `struct cosmofs_check_report`, `cosmofs_check`, the flags |
| `kernel-services/filesystem/cosmofs/cosmofscrash.c` | the check in `check_prefix`, and the unlinked-but-open workload |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | the seven fault tests and the clean test |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| docs | `docs/kernel-services/filesystem/cosmofs/{design,architecture,testing,invariants}.md`, README Status, `docs/README.md`, the inventory (the struck row, plus new rows for the operator interface and the orphan list) |

## New APIs

```c
/* kernel/include/kernel/cosmofs.h */
#define COSMOFS_CHECK_REPAIR   (1u << 0)   /* fix what has one right answer */
#define CFS_CHECK_NAMES        8u          /* offenders named per class */

struct cosmofs_check_class {
    uint64_t count;
    uint64_t repaired;                     /* with COSMOFS_CHECK_REPAIR */
    uint64_t name[CFS_CHECK_NAMES];        /* block or inode, per class */
    unsigned named;
};

struct cosmofs_check_report {
    /* Ten classes, in the order the design's table names them. */
    struct cosmofs_check_class alloc_not_seen, seen_not_alloc, dup, nlink_wrong,
                               orphan, dangling_entry, dir_bad, counter_wrong,
                               chain_cycle, unreadable;
    uint64_t blocks_seen, inodes_seen, dirs_seen, snapshots_seen;
    uint64_t bytes_allocated; /* the chunked maps, so the cost is visible */
    bool partial;             /* something was unreadable: the answer is incomplete */
    bool clean;               /* every class empty */
    uint64_t elapsed_ns;
};

/* Under the mount's own lock; -ENOMEM rather than a walk it cannot finish. */
int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);
```

## Migration plan

1. **The walk, read-only, reporting nothing but counts.** The two maps,
   the fixed structures, the inodes, the directories. `cosmofs-check-clean`
   asserts a populated filesystem is clean and that `blocks_seen` plus
   the free count equals `total_blocks` -- the arithmetic that makes the
   rest meaningful.
2. **Snapshots.** The snapshot trees, bitmaps and deadlists, and the
   test that a snapshotted filesystem is still clean after the live tree
   frees a block the snapshot holds. Without this step the checker
   reports a leak for every snapshot, which is the loudest possible
   false positive.
3. **The findings.** All ten classes, each with a test that manufactures
   exactly that fault through a test hook and asserts the class fires,
   the count is what the fault made it, and the offender is named.
4. **The crash suite.** `cosmofs_check` in `check_prefix`, asserting no
   finding of any class over every prefix of the existing workload; and
   `cosmofs-crash-orphan`, a separate workload that unlinks a file while
   it is open and crashes, asserting the orphan is found, is the only
   finding, and is reclaimed by repair. This step is where the unit's
   own bug -- the permanent leak -- becomes a test.
5. **Repair.** The four repairable classes, the refusals, and the
   re-run that proves a repair produced a clean filesystem.
6. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; steps 4 and 5 also run the release
build; step 5 runs `make test-crash`.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `cosmofs-check-clean` | a formatted, populated, synced filesystem reports `clean`, `partial` false, every class zero; `blocks_seen + free_blocks == total_blocks`; `inodes_seen == inode_count` | mark one extra block in *seen*: the arithmetic assertion fails |
| `cosmofs-check-leak` | with a block's bit set and nothing referencing it, `alloc_not_seen.count == 1` and names that block; with repair, the bit is cleared and a second pass is clean | skip the bitmap comparison: the count is zero |
| `cosmofs-check-crosslink` | two inodes' extents naming one block give `dup.count == 1` naming it; repair refuses and the filesystem is unchanged | drop the *dup* map: the block is merely seen twice and nothing fires |
| `cosmofs-check-nlink` | an inode whose `nlink` is one too high is reported with its number; repair sets it to the counted value and re-checks clean | count entries without counting a directory's own `..`: every directory reports a wrong `nlink` and the test fails on the count |
| `cosmofs-check-orphan` | an inode with `nlink == 0` and allocated blocks is reported; repair frees the blocks and zeroes the inode; the free count rises by exactly the blocks it held | skip inodes with `nlink == 0`, as the scrub does: nothing fires |
| `cosmofs-check-dangling` | a directory entry naming a free inode slot is reported with the parent and the name; repair refuses | validate only the entry's shape: nothing fires |
| `cosmofs-check-free-in-use` | a block an inode's extent names, with its bit cleared in the bitmap, gives `seen_not_alloc.count == 1` naming it; repair refuses and says why | compare only in one direction (allocated-not-seen): nothing fires |
| `cosmofs-check-dirbad` | an entry with a `namelen` past the record, a `type` that disagrees with its inode's mode nibble, and a name repeated in one directory each give `dir_bad`, named by parent and offset | check the entry's inode but not its shape: two of the three do not fire |
| `cosmofs-check-counters` | a superblock whose `free_blocks` is one too high and whose `inode_count` is one too low gives `counter_wrong.count == 2`; repair writes the counted values and the second pass shows that class empty | take the counters as the truth rather than the walk: nothing fires |
| `cosmofs-check-snapshot` | a filesystem with a snapshot holding blocks the live tree has freed is **clean**; deleting the snapshot and re-checking is still clean | omit the snapshot walk: every held block is reported as a leak, which is the false positive this test exists for |
| `cosmofs-check-partial` | with a metadata block made unreadable, `unreadable.count == 1`, `partial` true, and the pass still finishes and reports every other class | abort at the first unreadable block: the pass returns early and the other classes are empty |
| `cosmofs-replay` (extended) | every replayed prefix is structurally sound: `cosmofs_check` reports clean after each mount, over every prefix the suite already replays | leave a freed block's bit set in the commit path: some prefix reports a leak |
| `cosmofs-crash-orphan` | the workload unlinks a file that is still open and crashes; the replayed image has an inode with `nlink == 0` and blocks; the checker finds it, repair reclaims it, and the free count returns to what it was before the file existed | none needed: this is the defect, and the test is its proof. The bug-proof is the *repair* -- disable it and the space stays gone |

There are **ten** finding classes: the eight structural ones above plus
`chain_cycle` and `unreadable`, and every one of them has a test in this
table. The migration plan's step 3 builds them all.

**Vacuity, named in advance.** `cosmofs-check-clean` asserts the
arithmetic (`seen + free == total`), not merely "no findings": a checker
that walked nothing would report clean and fail this. Every fault test
asserts the count is *exactly one* and the offender is named, so a
checker that reported everything as broken would fail them all.
`cosmofs-check-snapshot` is the control for the whole snapshot step. And
the crash suite's extension asserts the checker on images that are
*supposed* to be sound, which is the only way to find out whether it
cries wolf.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.

## Benchmarks

Measured by the implementation:

- The pass's wall time and blocks read on the 512-block and
  16384-block test disks, from `elapsed_ns` and `blocks_seen`.
- What it adds to `cosmofs-replay`, which runs it once per replayed
  prefix -- the number that decides whether the crash suite keeps
  checking every prefix or a sample of them.
- Peak allocation, to check the `total_blocks / 4` estimate against
  what is actually asked of `kmalloc`.

## Risks

- **A false positive on a crash-consistent image.** The likeliest
  failure, because the checker's rules and the commit's rules have to
  agree about blocks in flight -- deferred frees (`fs->pending_free`,
  `cosmofs_core.c:379-395`) and the deadlist in particular. Mitigated by
  running it over every replayed prefix from step 4, where a
  disagreement shows up as a failing test rather than as an operator's
  bad afternoon.
- **The lock held too long.** A whole-filesystem walk under `fs->lock`
  blocks every file operation on that mount. The scrub set the
  precedent; the report's `elapsed_ns` makes the cost visible, and
  nothing runs the pass automatically.
- **Repair making things worse.** Answered by scope: four classes with
  one right answer each, a refusal for everything else, one transaction,
  and a re-run that must come back clean.
- **Memory on a large filesystem.** Stated, allocated up front, and
  `-ENOMEM` rather than a partial walk.
- **The checker and the format drifting apart.** Every format version so
  far added a structure (snapshots, members, keys, and links last week);
  a checker that does not know about a new one reports it as a leak. The
  mitigation is that the clean test runs on a filesystem with every
  feature turned on, so a new structure that the checker has not learned
  fails it immediately.

## Alternatives considered

- **An offline checker over a block device**, as `fsck` traditionally
  is. Left out: it needs the read path without a mount, which is a
  second entry into the buffer cache and the encryption layer, and the
  crash suite -- the best source of broken filesystems this tree has --
  mounts its images anyway. Named as future work.
- **A streaming checker** that makes several passes instead of holding
  a bitmap. Rejected for now: the bitmap is one bit per block and the
  multi-pass version costs several whole-filesystem reads to save it.
  The memory estimate is stated so the trade can be revisited on a
  filesystem large enough to need it.
- **Repairing `seen_not_alloc` by setting the bit.** Rejected: the block
  may already be allocated to something else, and the filesystem is
  mounted while the pass runs.
- **An operator interface**, whether a maintenance system call or a
  procfs node. Left out with its reasons under "Where it is called
  from": it needs a mount identity, a procfs mounts hierarchy and a
  reference rule for a file held open across an unmount, none of which
  exist. The inventory gets a row.
- **An on-disk orphan list**, which would make most of the `orphan`
  class impossible rather than merely findable. Deliberately left out
  and named: it is a format change (version 9) with its own recovery
  path at mount, and building the checker first is what tells us whether
  the list works. The inventory gets a row.
- **The checksum algorithm id in the metadata header** -- the second
  clause of the row this report closes the first of. `struct cfs_mhdr`
  (`cosmofs_format.h:76-83`) has no algorithm field; metadata is CRC32C
  by implication, while a per-inode `csum_algo` selects the data
  algorithm. The checker verifies metadata exactly as the tree does and
  is indifferent to which algorithm that is; naming it in the header is
  a format change for a future algorithm, and the row keeps it. After
  this unit that row reads: "no checksum algorithm id in the metadata
  header; no on-disk orphan list".
