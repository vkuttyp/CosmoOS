# NEXT SUBSYSTEM — the blocks nobody can reach

Date: 2026-09-15. Tree: `main` at 15a19c0 (after PR #142, the symlink
unit). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: a structural checker for cosmofs — a pass that walks the
live tree and every snapshot, counts what is reachable, and compares it
with what the filesystem says about itself: the allocation bitmaps, the
link counts, the inode map, and the superblock's own totals.**
**Built: PR #144 (2026-09-15).** The design below is as proposed; the
sections "As built" and "As run" record what the build changed and
measured. Differences from the plan, each found by building rather than
reading:

1. **Every crash strands space, not only an unlinked-but-open file.**
   The report predicted one leak class from the missing orphan record.
   The crash suite found a second and much broader one on its first run:
   a block freed during a transaction keeps its bitmap bit until the
   commit *after* the one that made the new root durable
   (`cosmofs_core.c`, "the old generation's blocks are free" -- the
   frees dirty chunks for the next commit). A block the old root still
   names cannot be freed before the new root lands, so the ordering is
   correct and the stranded space is its price. Measured: 162 of 199
   replayed prefixes leaked, the worst 18 blocks, 1912 in all.

   **Closed by the unmount-leak unit (PR #148).** "The ordering is
   correct and the stranded space is its price" was half right: the
   ordering is correct and the price was avoidable. Format version 9
   gives the root a record of what it freed, written before the root and
   replayed at mount, so the ordering is unchanged and the space comes
   back. The measurement above is what this unit found; the number after
   it is zero.
2. **The crash suite cannot assert "clean"** -- *at the time this was
   written*. Following from 1, the assertion was *no finding a crash
   cannot explain*: every class empty except leaked blocks, whose count
   was recorded and whose reclaim was proved on the first eight leaking
   prefixes -- bounded because a repair costs two more full passes and
   the suite exceeded its 8-second budget at 199 of them. **The
   unmount-leak unit restored the stronger assertion**: every replayed
   prefix is clean, the per-prefix leak total is asserted to be zero,
   and the weakened form is gone rather than left beside the strong one.
3. **A directory's link count has three parts**, and the first version
   counted two: the entry in its parent, its own self-reference, and one
   per subdirectory. cosmofs stores none of them as on-disk entries, so
   the checker has to know the rule rather than count what it reads.
4. **The deferred free list has to be claimed.** Blocks awaiting release
   at the next commit are allocated and unreachable by construction, so
   without claiming them every copy-on-write rewrite looked like three
   leaks on a filesystem nobody had touched.
5. **A snapshot's `alloc_root` is the member table** it was taken with,
   from format version 4 on, not an allocation index
   (`cosmofs_snap.c`, `snap_alloc_root`). Reading it by the wrong kind
   reported a sound snapshot as unreadable.
6. **The ordinary inode reader hides exactly what the pass looks for.**
   `cfs_inode_read` reports a slot with no links as absent, which is
   right for a lookup and wrong here, so link counts and orphan facts
   come from the map walk and repair clears the whole slot -- including
   the inode number, or the next pass finds the same orphan again.
7. **A test hook must not resolve a path.** The hook took one at first,
   which put a VFS symbol in `cosmofs_core.c` and broke the fuzz
   harness's link, that harness building the filesystem without a VFS.
   It takes an inode number: a filesystem-level hook has no business
   resolving names, and it has to work on a filesystem whose directories
   are the broken part.
8. **Two repairs can fight.** The orphan repair lowers the inode count
   as it clears a slot and the leak repair raises the free count, so a
   counter repair that writes back "what the walk counted" puts the
   pre-repair total back over the repair that just ran. Each counter is
   repaired only if the comparison found *that* counter wrong. The bug
   was invisible until the inode count was compared at all, which is the
   argument for comparing both totals rather than the easy one.
9. **One test hook, not eight.** `cosmofs_test_corrupt` takes a named
   corruption, so the list of ways to break a filesystem lives in one
   place. Eight of the ten classes are produced there on purpose; the
   two that are not are named in "As built" and in the inventory.
10. **Repair is an argument from absence, so it needs a sure walk.**
    Every repair the pass makes says "nothing reaches this". When the
    walk had to skip something, that means "this pass did not get
    there", and freeing those blocks destroys a live file.
    `cosmofs-check-partial` proved it on the first run after it was
    written: one unreadable directory block reports every file named
    inside it as an orphan with leaked blocks. Repair now runs only on a
    sound walk and reports `repair_refused` otherwise. Two more cases
    reach the same trap without anything being unreadable: an entry the
    walk skips for a bad type, and an entry whose inode number was
    overwritten, both of which leave a live inode unreferenced.
11. **The eight names are a diagnosis, not a work list.** The orphan and
    link-count repairs walked the report's capped name list, so a
    filesystem with nine orphans kept one while the report said
    repaired. Both walk the maps now.
12. **A slot's own number cannot be taken on trust.** The walk reaches
    an inode by position; a slot that disagrees had its blocks claimed
    while its accounting was dropped by maps sized on `next_ino`.

Differences 10, 11 and 12 came from the pull request's review rather
than from a test, which is the honest record: the tests that now assert
them were written after a reader pointed at the code and asked what
happens when the walk is wrong.

This report closes the first clause of the inventory's §3
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
- ***live***: the **live generation** claims this block -- any claim,
  not only file and directory content: the inode map's three levels,
  the allocation index and its chunks, an inode's extent blocks and
  checksum tree, the member table, the snapshot list, and every data
  block an extent names. Only this map detects duplication, and only
  within itself. Two live claims on one block is the corruption
  whichever structures made them: two files sharing an extent, an
  extent block that is also somebody's data, a bitmap chunk that is
  also an inode block. A block in *live* that a *snapshot* also holds
  is not in question at all, which is the whole point of the split.

Each structure is visited once, so a second *live* claim always means
two structures and never one structure twice: the deadlist chains are
walked in the snapshot step alone (the fixed-structure step walks the
snapshot *list*, not the lists hanging off it), and a block reached
twice through the same chain is a cycle, reported as `chain_cycle`
rather than silently followed.

The walk:

1. **The fixed structures**: both superblock slots, the member table,
   the inode map's three levels, the allocation index and its chunks,
   the snapshot list and every deadlist chain. Each block is marked in
   *seen*; a block already there goes in *dup*.
2. **Every inode** in the map with `ino != 0`: its inode block (from
   the map walk), its extent chain, its checksum-tree blocks, and every
   data block its extents name. **As built the extents are claimed but
   not separately validated**: an extent that overlaps another within
   the same inode, or one out of `lblk` order, is caught only if it
   makes two claims on one block (`dup`) or a claim outside the pool
   (`dir_bad`). An ordering fault that does neither is a wrong file, not
   a wrong filesystem, and the pass does not report it. Named in the
   inventory.
3. **Every directory**, from the root inode down, reading blocks
   through an inode-shaped `dir_read_block`: each entry's `namelen`,
   its `type` against the mode nibble of the inode it names, and that
   the inode is allocated. **A name repeated inside one directory is not
   detected as built**: it needs a set of the names in the directory,
   and the pass has maps of numbers rather than of strings. A repeated
   name that points at two different inodes shows up as a wrong link
   count; one that points at the same inode does not show up at all.
   Named in the inventory.
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
| `dup` | the live generation claims one block twice — two files sharing an extent, or metadata overlapping metadata or data (a block shared with a *snapshot* is not this) |
| `nlink_wrong` | an inode's `nlink` differs from the entries that name it |
| `orphan` | an inode allocated and unreachable (`nlink == 0` with blocks, or `nlink != 0` with no name) |
| `dangling_entry` | an entry naming a free or out-of-range inode slot |
| `dir_bad` | a malformed entry, a repeated name, a type that disagrees with its inode |
| `counter_wrong` | `free_blocks` or `inode_count` disagreeing with the walk, one finding each. **Not** `next_ino`: it is a high-water mark, not a total, and a filesystem that never reuses a number is entitled to any value above the highest slot in use |
| `chain_cycle` | a metadata chain (extent, deadlist, snapshot list) that revisits a block |
| `unreadable` | a metadata block that could not be read: the answer is partial |

The report carries a count per class and the first `CFS_CHECK_NAMES`
(8) offenders of each with enough to find them (an inode number, a
block, a parent and a name), because "1 leak" is a fact and "inode 41's
second extent" is a diagnosis.

### What it repairs, and what it refuses

`COSMOFS_CHECK_REPAIR` is off by default; the pass is read-only and
holds no transaction. With it on, and only with it on, the checker
repairs exactly four classes, each of which has one right answer.

**And only when the walk is sure.** Every one of those four is an
argument from absence, so a walk that had to skip something -- an
unreadable block, a malformed entry, an entry naming a free slot, a
chain that cycles -- makes "nothing reaches this" mean "this pass did
not get there". Repair then does nothing and says `repair_refused`. This
is not a caution added for tidiness: `cosmofs-check-partial` reports
every file named inside a broken directory block as an orphan whose
blocks are leaked, and repairing that image would destroy them.

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
repair as a repair bug.

**As built there is no `remaining` field**, which the design asked for.
It would have been the same number the second pass reports, so the tests
run the second pass and compare the classes rather than trust a count
the first pass wrote about a filesystem it had just changed. Each class
carries `count`, `repaired`, and the first eight offenders.

**One repair per thing repaired**, which is what makes `repaired`
comparable with `count`. The counters were where this broke: the orphan
repair lowers the inode count as it clears a slot, so a counter repair
that wrote back "what the walk counted" restored the pre-repair total
over the repair that had just run. Each counter is repaired only if the
comparison found that counter wrong.

### Where it is called from

Two callers, and no new system call:

1. **The self-tests**, as the scrub is called, through
   `cosmofs_check` in `kernel/include/kernel/cosmofs.h`.
2. **The crash suite**, in `check_prefix` after the mount succeeds and
   before the unmount (`cosmofscrash.c:344`, and the repair proof at
   `:374`): every replayed prefix of the *existing* workload must be
   structurally sound. This is the unit's sharpest test in both
   directions -- it asserts the filesystem's crash behaviour and it
   asserts the checker does not cry wolf on an image that is merely
   mid-history. **As built the assertion is not "no finding of any
   class"** but "no finding a crash cannot explain": leaked blocks are
   counted and their reclaim proved, everything else must be empty. The
   reason is difference 2 at the top of this report, and it is a fact
   about the filesystem rather than a concession by the test.

   The unlinked-but-open workload is deliberately **not** added to that
   suite, because a prefix taken after its unlink *must* show an orphan
   and the generic assertion would then have to be weakened for every
   prefix of every workload. It gets its own test
   (`cosmofs-crash-orphan`), which asserts the opposite: that the orphan
   is there, that it is the only finding, and that repair reclaims
   exactly the blocks the file held. It is called
   `cosmofs-check-orphan-crash`, with the checker's tests rather than
   the crash suite's, because the checker is what it is about.
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

*Ownership and lifetime.* The maps are the pass's own, allocated at
entry and freed at exit; nothing outlives the call. As built there are
six rather than three: `seen` and `live` over block numbers, `reach` and
`alive` over inode numbers, and two counts per inode -- the links the
entries make, and the links the inodes claim. `alive` and the second
count are what difference 6 above forced, because an inode with no links
is invisible to the ordinary reader and is exactly what the pass looks
for. The pass takes `fs->lock` for its duration, as the scrub does,
so no mutation interleaves with the walk and the answer describes one
moment.

*Concurrency.* Holding `fs->lock` across a whole-filesystem walk is what
the scrub already does, and it is the reason the pass is a diagnostic
rather than something a file server runs hourly. The report says how
long it took so a reader can see what it costs.

*Memory.* Two bits per block and one 32-bit count per inode as
designed; as built, four bits per block-or-inode number and two counts,
which is the same order and twice the constant: `total_blocks / 4` bytes
plus `inode_count * 4`. That is bytes for the
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

*Security.* Nothing in this unit is reachable from userland: the pass is
called by the self-tests and the crash suite, and both of those also
call repair, on filesystems they made themselves. The whole file is
under `CONFIG_DEBUG`, so a release build contains neither. No syscall, no procfs node, no ioctl, so the unit adds no
privilege surface. When the operator interface arrives it will have to
decide who may read block and inode numbers of a mounted filesystem and
who, if anyone, may repair one; this report deliberately does not.

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
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | new: the pass, the two maps, the ten classes, the four repairs |
| `kernel-services/filesystem/cosmofs/cosmofs_internal.h` | the inode-shaped directory read, shared with `cosmofs.c`; `cfs_bitmap_test` and `cfs_inode_read_raw` |
| `kernel-services/filesystem/cosmofs/cosmofs.c` | `dir_read_block` split so the checker and the VFS path share one reader |
| `kernel/include/kernel/cosmofs.h` | `struct cosmofs_check_report` (with `repair_refused`), `cosmofs_check`, the flags, and `cosmofs_test_corrupt` with its nine named corruptions |
| `kernel-services/filesystem/cosmofs/cosmofscrash.c` | the check in `check_prefix`, and the unlinked-but-open workload |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | the eight checker tests; the nine manufactured faults reach them through one hook, `cosmofs_test_corrupt` |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| docs | `docs/kernel-services/filesystem/cosmofs/design.md` (the structural check, what a post-crash image may have, and the stale future-work line that still promised one) and `architecture.md` (the non-responsibility now names only the *offline* checker); README Status, `docs/README.md`, the inventory (the struck row, plus new rows for the operator interface, the on-disk orphan list, and the two finding classes no test manufactures) |

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
    uint64_t counted_free;    /* free blocks the walk counted, not the ones claimed */
    uint64_t bytes_allocated; /* the chunked maps, so the cost is visible */
    bool partial;             /* something was unreadable: the answer is incomplete */
    bool clean;               /* every class empty */
    bool repair_refused;      /* repair asked for, and the walk was not sure enough */
    uint64_t elapsed_ns;
};

/* Under the mount's own lock; -ENOMEM rather than a walk it cannot finish.
 * Debug builds only (`CONFIG_DEBUG`) **as this unit left it**, because
 * the release build had no caller: there was no operator interface to a
 * mount's maintenance passes. That was the inventory row this unit left
 * behind, and the fsctl unit closed it -- the gate is gone and
 * /dev/fsctl is the caller (docs/audit/next-subsystem-fsctl.md). */
int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);

/* Test hook: break the filesystem in one named way, so that a finding
 * is one a test produced on purpose. Eight of the ten classes are
 * manufactured here; `chain_cycle` is not, and nor are four of the six
 * places `dir_bad` is reported from. `what` returns the block or inode
 * it touched, which is what the check must name back. */
enum cosmofs_corruption {
    COSMOFS_CORRUPT_LEAK, COSMOFS_CORRUPT_FREE_IN_USE, COSMOFS_CORRUPT_CROSSLINK,
    COSMOFS_CORRUPT_NLINK, COSMOFS_CORRUPT_ORPHAN, COSMOFS_CORRUPT_DANGLING,
    COSMOFS_CORRUPT_DIRENT, COSMOFS_CORRUPT_COUNTER,
};
int cosmofs_test_corrupt(struct mount *mnt, enum cosmofs_corruption kind,
                         uint64_t ino, uint64_t *what);   /* 0 where no case needs one */
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
   **As built, eight of the ten.** `chain_cycle` is reported and not
   manufactured, because a cycle needs a hook that writes structure
   rather than flipping a field; and of the six places `dir_bad` is
   reported from, two are fired by a test and four are not. Both gaps
   are inventory rows. See "As built" below.
4. **The crash suite.** `cosmofs_check` in `check_prefix`, asserting no
   finding of any class over every prefix of the existing workload; and
   `cosmofs-crash-orphan`, a separate workload that unlinks a file while
   it is open and crashes, asserting the orphan is found, is the only
   finding, and is reclaimed by repair. This step is where the unit's
   own bug -- the permanent leak -- becomes a test. **As built the
   assertion is weaker and the reason is difference 2:** no finding a
   crash cannot explain, leaked blocks being the one class a crash
   explains. The separate workload is `cosmofs-check-orphan-crash`.
5. **Repair.** The four repairable classes, the refusals, and the
   re-run that proves a repair produced a clean filesystem. **As built
   there is a fifth rule the plan did not have:** repair runs only on a
   walk sure of its reachability, and reports `repair_refused`
   otherwise. Difference 10.
6. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; steps 4 and 5 also run the release
build; step 5 runs `make test-crash`.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `cosmofs-check-clean` | a formatted, populated, synced filesystem reports `clean`, `partial` false, every class zero; `blocks_seen + free_blocks == total_blocks`; `inodes_seen == inode_count` | mark one extra block in *seen*: the arithmetic assertion fails |
| `cosmofs-check-leak` | with a block's bit set and nothing referencing it, `alloc_not_seen.count == 1` and names that block; with repair, the bit is cleared and a second pass is clean | skip the bitmap comparison: the count is zero |
| `cosmofs-check-crosslink` | two inodes' extents naming one block give `dup.count == 1` naming it; an inode's extent naming a block the allocation index already claims gives the same, so metadata counts too; repair refuses and the filesystem is unchanged | track duplication over content only: the metadata case does not fire |
| `cosmofs-check-nlink` | an inode whose `nlink` is one too high is reported with its number; repair sets it to the counted value and re-checks clean | count entries without counting a directory's own `..`: every directory reports a wrong `nlink` and the test fails on the count |
| `cosmofs-check-orphan` | an inode with `nlink == 0` and allocated blocks is reported; repair frees the blocks and zeroes the inode; the free count rises by exactly the blocks it held | skip inodes with `nlink == 0`, as the scrub does: nothing fires |
| `cosmofs-check-dangling` | a directory entry naming a free inode slot is reported with the parent and the name; repair refuses | validate only the entry's shape: nothing fires |
| `cosmofs-check-free-in-use` | a block an inode's extent names, with its bit cleared in the bitmap, gives `seen_not_alloc.count == 1` naming it; repair refuses and says why | compare only in one direction (allocated-not-seen): nothing fires |
| `cosmofs-check-dirbad` | an entry with a `namelen` past the record, a `type` that disagrees with its inode's mode nibble, and a name repeated in one directory each give `dir_bad`, named by parent and offset | check the entry's inode but not its shape: two of the three do not fire |
| *as built* |  of these three the type mismatch is manufactured, in `cosmofs-check-faults`; the over-long `namelen` is not, and the repeated name is not *detected* at all, which is its own inventory row | |
| `cosmofs-check-counters` | a superblock whose `free_blocks` is one too high and whose `inode_count` is one too low gives `counter_wrong.count == 2`; repair writes the counted values and the second pass shows that class empty | take the counters as the truth rather than the walk: nothing fires |
| *as built* |  exactly this, in `cosmofs-check-faults`. `next_ino` is not compared, being a high-water mark rather than a total | |
| `cosmofs-check-snapshot` | a filesystem with a snapshot holding blocks the live tree has freed is **clean**; deleting the snapshot and re-checking is still clean | omit the snapshot walk: every held block is reported as a leak, which is the false positive this test exists for |
| `cosmofs-check-partial` | with a metadata block made unreadable, `unreadable.count == 1`, `partial` true, and the pass still finishes and reports every other class | abort at the first unreadable block: the pass returns early and the other classes are empty |
| *as built* |  a directory block rather than any metadata block, and the test asserts one thing more than the design asked for -- that repair refuses, because this image is exactly the one a repair would destroy | |
| `cosmofs-replay` (extended) | every replayed prefix is structurally sound: `cosmofs_check` reports clean after each mount, over every prefix the suite already replays | leave a freed block's bit set in the commit path: some prefix reports a leak |
| *as built* | not clean but *no finding a crash cannot explain*, because every crash strands blocks -- difference 1. The leak count is recorded and the reclaim proved on the first eight leaking prefixes | `no-crash-check`, and the assertion that it measured something |
| `cosmofs-crash-orphan` | the workload unlinks a file that is still open and crashes; the replayed image has an inode with `nlink == 0` and blocks; the checker finds it, repair reclaims it, and the free count returns to what it was before the file existed | none needed: this is the defect, and the test is its proof. The bug-proof is the *repair* -- disable it and the space stays gone |
| *as built* | `cosmofs-check-orphan-crash`, with the checker's tests rather than the crash suite's | `repair-keeps-ino` |
| *added after review* | `cosmofs-check-many-orphans` (twelve orphans, eight named, all twelve repaired in one pass) and `cosmofs-check-slot-identity` (a slot whose number is not its position) | `repair-first-eight`, `slot-number-trusted` |

There are **ten** finding classes: the eight structural ones above plus
`chain_cycle` and `unreadable`. The plan gave each of them a test that
manufactures it. **Eight of the ten ended up with one.** `chain_cycle`
did not, because a cycle needs a corruption hook that writes structure
rather than flipping a field. `dir_bad` is reported from six places and
two of them are fired: a type that disagrees with its inode, and a slot
whose number is not its position. The four that are not: a block pointer
outside the pool's range, a snapshot member table whose count does not
fit its block, an over-long `namelen`, and a directory reached from two
parents. Both gaps are rows in
`docs/audit/2026-09-deferred-work-inventory.md`, and the same split is
stated in `architecture.md`'s interfaces table and above the corruption
enum in `kernel/include/kernel/cosmofs.h`.

**Vacuity, named in advance.** `cosmofs-check-clean` asserts the
arithmetic (`seen + free == total`), not merely "no findings": a checker
that walked nothing would report clean and fail this. Every fault test
asserts the count is *exactly one* and the offender is named, so a
checker that reported everything as broken would fail them all.
`cosmofs-check-snapshot` is the control for the whole snapshot step. And
the crash suite's extension asserts the checker on images that are
*supposed* to be sound, which is the only way to find out whether it
cries wolf.

### As built

**Thirteen named tests became eight, because the faults became one
hook.** `cosmofs_test_corrupt` takes the corruption by name, so the nine
manufactured faults are nine calls rather than nine fixtures. Seven of
them are sub-cases of one test; the other two have tests of their own,
because what they assert is a repair's behaviour rather than a class
firing. What the design named, and where it is:

| the design's test | as built |
| --- | --- |
| `cosmofs-check-clean` | `cosmofs-check-clean`, unchanged |
| `cosmofs-check-leak` | `cosmofs-check-leak`, unchanged: the one class with a repair worth its own test |
| `-crosslink`, `-nlink`, `-orphan`, `-dangling`, `-free-in-use`, `-dirbad`, `-counters` | `cosmofs-check-faults`, one sub-case each, every one asserting the class fires, the count is exactly what the fault made it, the offender is named, and repair either fixes it or refuses |
| `cosmofs-check-snapshot` | `cosmofs-check-snapshot`, unchanged |
| `cosmofs-check-partial` | `cosmofs-check-partial`: a directory block broken off the mount, the block named, the report marked incomplete, the pass proved to reach its last phase, and repair proved to refuse |
| -- | `cosmofs-check-many-orphans` and `cosmofs-check-slot-identity`, neither in the design: both exist because a review found the defect they assert |
| `cosmofs-replay` (extended) | extended, but asserting *no finding a crash cannot explain* rather than clean -- see difference 2 above |
| `cosmofs-crash-orphan` | `cosmofs-check-orphan-crash`, the same test under the checker's name |

**What still has no test that manufactures it.** `chain_cycle` is
reported from four places (the inode map's chain, the snapshot list, a
deadlist, an inode's block chain) and no test makes one: a cycle needs a
metadata block written with a pointer back to itself, which is a
corruption hook that writes structure rather than flipping a field, and
none of the nine does. The bound it enforces (`CFS_CHECK_MAX_CHAIN`,
4096) is what stops the pass hanging on one, and that bound is exercised
by nothing. `dir_bad` is reported from six places and two are fired by a
test -- a type that disagrees with its inode, and a slot whose number is
not its position. The other four are not: a block pointer outside the
pool's range, a snapshot member table whose count does not fit its
block, an over-long `namelen`, and a directory reached from two parents.
**Both are named in the inventory rather than left in a comment.**

### As run

The unit (PR #144, 2026-09-15), on both architectures:

| run | result |
| --- | --- |
| `make test` (x86-64, AArch64) | 281 self-tests pass, including the eight checker tests |
| `cosmofs-check-clean` | 23 blocks seen, 489 free, 5 inodes, 2 directories; `seen + free == total` (35 ms) |
| `cosmofs-check-leak` | one leaked block found by number and given back; a second pass is clean (41 ms) |
| `cosmofs-check-faults` | seven manufactured faults, each found by name: four repaired, three refused. The counter case breaks both superblock totals in opposite directions and asserts two findings and two repairs (229 ms, seven fixtures rather than seven walks) |
| `cosmofs-check-snapshot` | a snapshot's held blocks are neither leaks nor cross-links, before and after its deletion (39 ms) |
| `cosmofs-check-orphan-crash` | inode 4 survives its unlink with its blocks; repair reclaims them and the free count returns exactly (58 ms) |
| `cosmofs-check-partial` | directory inode 2 unreadable, 3 blocks stranded by the names that went with the block, report marked incomplete, final comparison still reached, **and repair refused** (35 ms) |
| `cosmofs-check-many-orphans` | twelve orphans, eight named, all twelve repaired in one pass and none left (21 ms) |
| `cosmofs-check-slot-identity` | a slot whose number is not its position is named by position, marks the answer incomplete, and repair refuses (16 ms) |
| `cosmofs-replay` | 199 prefix images mounted and checked; **162 leaked blocks a crash stranded, worst 18, 1912 in all**, each reclaimed and clean afterwards; 4.5-5.3 s across runs. **Superseded by the unmount-leak unit (PR #148)**: the suite now asserts every prefix is clean and the total stranded is zero |

The four leak numbers are **identical on x86-64 and AArch64**, which is
the evidence that they describe the filesystem and not the host: the
same workload, the same 199 prefixes, the same blocks stranded in the
same places. A number that moved between architectures would have been
about scheduling.

**Bug-proofs, as run** (each injection alone, then reverted):

| injection | result |
| --- | --- |
| `phantom-claim`: claim a block nothing owns | 6 tests fail, `cosmofs-check-clean`'s arithmetic among them |
| `no-bitmap-compare`: never compare allocated against seen | 2 fail; no leak is ever found |
| `no-free-in-use`: drop the other direction | `cosmofs-check-faults` fails |
| `dup-content-only`: no duplication domain | `cosmofs-check-faults` fails on the cross-link |
| `nlink-no-self`: forget a directory's self-reference | 5 fail |
| `orphan-hidden`: skip inodes with no links, as the scrub does | 13 fail |
| `no-snapshot-walk`: walk no snapshots | `cosmofs-check-snapshot` fails on `snapshots_seen` |
| `snapshot-is-live`: put a snapshot's claims in the live domain | `cosmofs-check-snapshot` fails: sharing becomes corruption |
| `no-pending-claim`: do not claim the deferred frees | 2 fail: ordinary rewrites look like leaks |
| `no-crash-check`: stop asking in the replay suite | `cosmofs-replay` fails on having measured nothing |
| `repair-keeps-ino`: leave the number in a cleared slot | 2 fail: the next pass finds the same orphan |
| `abort-on-unreadable`: stop comparing once anything is unreadable | `cosmofs-check-partial` fails: the pass never reaches its last phase |
| `one-counter-only`: compare the free count and not the inode count | `cosmofs-check-faults` fails: one finding where two totals are wrong |
| `counter-blind-repair`: write both counted totals back regardless | `cosmofs-check-faults` and `cosmofs-check-orphan-crash` fail: the repair undoes itself |
| `repair-ignores-doubt`: repair an incomplete walk | 3 fail, `cosmofs-check-partial` among them |
| `repair-first-eight`: the name list as a work list again | `cosmofs-check-many-orphans` fails: eight repaired of twelve |
| `slot-number-trusted`: believe a slot's own number | `cosmofs-check-slot-identity` fails: nothing is reported |

Two of those seventeen passed at first and both were fixed rather than
excused. `no-crash-check` passed because removing an assertion cannot
fail a test with no other claim on it, so `cosmofs-replay` now asserts
that it measured something; `no-snapshot-walk` broke the build instead
of reaching the test, so it empties the snapshot list inside the walk
instead of deleting the call.

## Benchmarks

- **The pass** on a 512-block filesystem: 23 blocks seen, a few
  milliseconds, reported per run in `elapsed_ns`.
- **The crash suite** went from about 3 s to 8804 ms with a check *and*
  a repair on all 199 prefixes -- past its 8-second budget, which is the
  number the report asked for. Checking every prefix and proving the
  reclaim on the first eight brings it to 4.5-5.3 s, so the assertion is
  kept on every image and only the proof is sampled. The spread is the
  host's, not the test's: it is the same 199 images every run.
- **The maps** are six chunked allocations (seen, live, reachable,
  alive, links, link counts), reported in `bytes_allocated`: 24 KiB for
  the test disks. The `kmalloc` ceiling of 4 MiB is what forced the
  chunking; a single allocation would have capped the checker at a
  128 GiB filesystem. Chunking moves that ceiling rather than removing
  it -- the array of chunk pointers is one allocation, so 4 MiB of
  pointers reach about 64 TiB -- which is far enough that the next
  limit met is the time a whole-filesystem walk holds the mount's lock,
  not the memory it asks for.

## Risks

- **A false positive on a crash-consistent image.** The likeliest
  failure, because the checker's rules and the commit's rules have to
  agree about blocks in flight -- deferred frees (`fs->pending_free`,
  `cosmofs_core.c:379-395`) and the deadlist in particular. Mitigated by
  running it over every replayed prefix from step 4, where a
  disagreement shows up as a failing test rather than as an operator's
  bad afternoon. **This risk materialised three times** and the step
  caught all three: the deferred free list unclaimed, a snapshot's
  `alloc_root` read as the wrong kind of block, and a directory's link
  count short by its own self-reference. Each looked like corruption on
  a filesystem that was perfectly sound.
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
