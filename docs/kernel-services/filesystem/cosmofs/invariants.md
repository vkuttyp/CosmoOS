# cosmofs: invariants

Rules cosmofs keeps on disk and in memory. Each has a **Check** (what
verifies it today) and, where honest, a **Gap**. Changing a rule means
changing this file, `design.md` and the code together.

Most of cosmofs's rules were numbered before this file existed, in the
VFS's list, and keep their numbers there. Section 1 indexes them; it
does not restate or renumber them. Section 2 numbers, with the prefix
`CF`, the rules that `design.md` and the audit reports state in prose
but that no invariants file carried.

## 1. Rules numbered elsewhere

| Rule | Where | In one line | Checked by |
|---|---|---|---|
| V2 | `docs/kernel-services/vfs/invariants.md` | every committed root describes a completely valid filesystem | `cosmofs-crash`, `cosmofs-replay`, `cosmofs-fallback` |
| V3 | same | a block a committed root references is never overwritten | review, `cosmofs-crash` |
| V4 | same | blocks freed in a transaction are allocatable only after its root is durable | `cosmofs-ops`, review |
| V5 | same | every block read from the pool is verified before use | `cosmofs-format`, `cosmofs-crash`, `cosmofs-csum`, `cosmofs-badmap`, `fuzz_cosmofs` |
| V6 | same | inode numbers are never reused | review |
| V15 | same | file data reaches cosmofs only through `writepage`; a hole costs nothing | `cosmofs-ops`, `cosmofs-holes` |
| V16 | same | every mutation writes its inode through | `cosmofs-ops` |
| V17 | same | commit on `vfs_sync`, `fsync`, unmount and the writeback thresholds | `cosmofs-fsync`, `cosmofs-writeback`, `cosmofs-format`, `cosmofs-ops` |
| V18 | same | the pool is the only thing cosmofs addresses | `pool`, review |
| V19 | same | a full disk can still delete and commit (the metadata reserve) | `cosmofs-reserve` |
| V20 | same | a block-layer queue-full answer never reaches cosmofs | `blk-queue` |
| V21 | same | a snapshot's blocks are never handed out while it exists, and are handed back when it stops needing them | `cosmofs-snapshot`, `cosmofs-snapshot-remount` |
| V22 | same | a snapshot is read-only and history is reachable only by name | `cosmofs-snapshot`, `SNAPTEST`, `test_cosmofs` |
| V23 | same | a snapshot somebody is reading is not deleted (`-EBUSY`) | `cosmofs-snapshot` |
| V24 | same | a block is named by the member that holds it; a run lies on one member | `cosmofs-pool2`, `cosmofs-v3`, `cosmofs-badmembers`, `pool`, `test_cosmofs` |
| V25 | same | a mirror is read with a checker; a copy that missed a commit is not mirrored | `cosmofs-mirror`, `cosmofs-mirror-stale` |
| V26 | same | a compressed record is one object; its checksums are of the disk bytes; a record its rewrite cannot compress is dropped whole before plain blocks replace it (S7) | `cosmofs-compress`, `test_lz4`, `fuzz_lz4` |
| V27 | same | what is encrypted is authenticated; what is not is stated | `cosmofs-crypt`, `test_chacha20` |
| V30 | same | a mount named by id stays alive for a pass; unmount waits | `vfs-mount-id`, `vfs-mount-pin` |
| V31 | same | a mount listing is the caller's own namespace | `fsctl-list` |
| V36 | same | nothing writes raw sectors to a mounted device (incl. `vfs_format`) | see that file |
| F4 | `docs/verification/invariants.md` | crash consistency under every prefix of the write stream | `cosmofs-replay` |
| S18 | `docs/kernel/security/invariants.md` | no master key, salt or nonce is drawn from an unseeded pool | `cosmofs-crypt` |

Two of those entries are behind the code (section 4): F4's gap says
block-bitmap and space accounting are not checked, and V22 says a
snapshot id is never reused while the filesystem lives.

## 2. The CF rules

**CF1. Every metadata block names itself, and a block that does not is
refused before its contents are used.** `struct cfs_mhdr` carries
`CFS_MHDR_MAGIC`, the kind, the generation that wrote it, its own DVA
(`blkno`), a checksum and, from version 11, `csum_algo`.
`cfs_mhdr_fault_of` (`cosmofs_core.c`) answers in a fixed order --
`MHDR_MAGIC`, `MHDR_BLKNO`, `MHDR_KIND`, `MHDR_ALGO`, `MHDR_CRC` -- so an
algorithm this build cannot verify is named as such and not reported as
damage; a zero `csum_algo` (a pre-v11 image) and `CFS_CSUM_CRC32C` both
mean CRC32C. The superblock and the member label are read before
anything can be parsed and keep a fixed CRC32C. A block whose generation
equals the open transaction may be changed in place; any other is copied
first (`cfs_buf_cow`). Check: `cosmofs-metadata-csum-id` (a sealed block
is `MHDR_OK` with `csum_algo == CFS_CSUM_CRC32C`; a zero algorithm still
verifies; an unknown algorithm with a valid CRC is `MHDR_ALGO`; a flipped
byte is `MHDR_CRC`; the algorithm word is inside the checksum),
`cosmofs-badmap`, `cosmofs-fallback`, `fuzz_cosmofs`, and
`test_cosmofs` for the header's size and the field's offset.

**CF2. Nothing allocates after the bitmap fixpoint.** `commit_bitmap` is
the one pass that makes the on-disk bitmap agree with memory. Every
block a commit needs -- the free record, the orphan record, the copies of
the snapshot list and deadlists, the member table and every member's
allocation index -- is reserved or copied before it; blocks reserved and
not used are listed in the free record as free. A dirty chunk with no
reserved destination fails the transaction instead of being written to
DVA 0. A block allocated after the fixpoint would be reachable and free
in the published root (`seen_not_alloc`). Check:
`cosmofs-freelog-accounted`, `cosmofs-orphan-reserved`,
`cosmofs-snap-reserved` (each runs the structural check on the published
root), `cosmofs-freelog-chain` (the over-reserved slack is listed),
`cosmofs-replay` (every prefix image must check clean).

**CF3. A root records what its transaction freed, and a mount finishes
the job.** From version 9, `free_root` heads a `CFS_KIND_FREELOG` chain
of `struct cfs_dead_block` (506 numbers per block) naming every block the
transaction released whose bit the root's bitmap still sets, except
blocks a snapshot holds. `freelog_replay` applies it at mount, before
the bitmap is used for allocation, and does **not** clear it; the next
commit releases the previous chain directly (exempt from the snapshot
filter, since no snapshot's tree reaches a record). A record block that
does not read, a chain longer than the bound, or a `count` above
`CFS_DEAD_PER_BLOCK` refuses that root with `-EIO` (the mount may fall
back to the other slot); it is never treated as empty. Check:
`cosmofs-unmount-leak`, `cosmofs-freelog-format`,
`cosmofs-freelog-supersede`, `cosmofs-freelog-not-held`,
`cosmofs-freelog-snapshot`, `cosmofs-freelog-idempotent`,
`cosmofs-freelog-reuse`, `cosmofs-freelog-chain`,
`cosmofs-freelog-rollback`, `cosmofs-freelog-malformed`, and
`cosmofs-replay` (the stranded-block total over every prefix must be
zero).

**CF4. A root records the inodes it still owes, and the replay deletes
only what has no name.** From version 10, `orphan_root` heads a
`CFS_KIND_ORPHAN` chain of inode numbers whose last name is gone while a
reference holds them. The set lives in memory (`fs->orphans`) and each
commit writes it whole; an unlink and an eviction inside one transaction
cancel and write nothing. `orphan_replay` reads each inode raw, skips a
slot that is already empty (so a repeated replay changes nothing and
`inode_count` is never taken twice), **refuses** an inode with
`nlink != 0` (logged, left for the checker), and fails the mount if
releasing an inode fails after its truncate began. It never touches the
parent of a removed directory. An unreadable, over-long or over-full
record refuses that root, as in CF3. Check: `cosmofs-orphan-crash`,
`cosmofs-orphan-cancels`, `cosmofs-orphan-dir`, `cosmofs-orphan-rename`,
`cosmofs-orphan-idempotent`, `cosmofs-orphan-reserved`,
`cosmofs-orphan-supersede`, `cosmofs-orphan-rollback`,
`cosmofs-orphan-suspect`, `cosmofs-check-orphan-crash`, and the held
handle in `cosmofs-replay`'s workload. Gap: a writeback commit can land
between `cfs_unlink_common` dropping `cfs->lock` and the eviction, and
writes a record for an inode about to go; the next commit retires it.

**CF5. The snapshot list and every deadlist are copy-on-write, and no
block is on two deadlists.** `cfs_super.snap_root` alone names the list,
so a change to a `CFS_KIND_SNAPLIST` or `CFS_KIND_DEADLIST` block copies
it first (`snap_cow`, `DL_COPY`) and the superseded block is freed
exempt. A deadlist entry is appended before the root, inside the
reservation of CF2. A deletion releases blocks and clears the entry that
names them in one transaction; a failure once releasing began abandons
the transaction (CF7). Check: `cosmofs-snap-cow` (the list's head block
changes across one commit), `cosmofs-snap-unmount`,
`cosmofs-snap-nogrow`, `cosmofs-snap-reserved`, `cosmofs-snap-rollback`;
`cosmofs-replay` calls `cosmofs_test_deadlist_dups` on every prefix and
requires zero duplicates **and** a non-zero number of entries examined.
Gap: `cosmofs_check` claims deadlist entries as non-live, so a
duplicated entry is not one of its findings; only the replay suite's
count sees it.

**CF6. Whether a snapshot holds a freed block is decided once.**
`cfs_snapshot_fill` takes one verdict per pending free and the free
record, the deadlist append and the release loop all read it; the
release loop after the root only clears bits. Check:
`cosmofs-snap-onewalk` (walks of the snapshot list minus verdicts, read
from `cosmofs_test_snap_walks` and `cosmofs_test_snap_verdicts`).

**CF7. An abandoned transaction is never published.** `cfs_fail` sets
`fs->failed` once; `cfs_commit_ex` then returns it, the vnode operations
that test it (lookup, create, unlink, the page writers and others)
answer `-EIO`, `cosmofs_check` and
`cosmofs_scrub` refuse, and unmount drops the transaction. A commit that
fails after reserving record blocks gives the reservation back and
publishes no root. `cfs_reset_root` (the older-slot fallback) clears the
pending, exempt and orphan lists and `failed`, because they belong to
the abandoned attempt and not to the root about to load. Check:
`cosmofs-freelog-rollback`, `cosmofs-orphan-rollback`,
`cosmofs-snap-rollback` (each injects `-EIO` through a
`cosmofs_test_fail_*` hook and checks the next mount sees the previous
root with nothing leaked), `fault-blk` (device errors injected under a
cosmofs workload; a forced unmount drops whatever was abandoned and the
remount shows a committed root whose files all read). Gap: a release
build has no fault injection, so these paths run only in debug boots.

**CF8. No autonomous committer exists before the mount is live.** The
replay runs without `cfs->lock`, which is sound only while no other
thread is in the filesystem. `note_dirty` starts the `cfs-wb` thread
only once `fs->mount_done` is set; `cosmofs_mount` starts it at the end
if the replay left the transaction dirty, and unmount and `cfs_destroy`
join it before freeing. The thread takes `mount.sync_lock` with
`mutex_trylock` and never commits an unmounted mount. Check:
`cosmofs-mount-no-early-writeback` (the replay's dirty marks are
non-zero and none found a running thread; read through
`cosmofs_test_mount_writeback`).

**CF9. A field a version added is read and written only at that
version or later.** `free_root` from 9, `orphan_root` from 10,
`key_root` from 7, the member table from 4; below those versions the
words are reserved zeros and are ignored even if non-zero. Symlink
creation is refused below version 8 (`-EOPNOTSUPP`). The mount never
rewrites the superblock's version. Check: `cosmofs-freelog-format` (a
non-zero `free_root` written by `cosmofs_test_poison_free_root` into a
version-8 image is ignored), `cosmofs-symlink-version`, `cosmofs-v3`.
Gap: compression is not gated. `cfs_create_common` gives every new
regular file `CFS_COMPRESS_LZ4` on any version, so a version-2 to 5 image
written by this kernel can hold version-6 records; `cosmofs-v3` writes
such an image and passes because this kernel reads it.

**CF10. An extent is valid before it maps anything, and an inode's runs
ascend and do not overlap.** `extent_valid` (`cosmofs.c`): logical and
physical counts non-zero; a compressed record covers at most
`CFS_RECORD_BLOCKS` (8) logical blocks in no more physical blocks, with
algorithm `CFS_COMPRESS_LZ4`; both ends are valid DVAs on one member;
`lblk + count <= 2^32`. Runs are sorted by `lblk` and never overlap; a
gap is a hole. At most `CFS_MAX_EXTENTS` (4096) runs are loaded per
inode. Check: `cosmofs-badmap` (two swapped direct runs re-sealed: both
reads `-EIO`), `cosmofs-check-extent-order`,
`cosmofs-check-extent-overlap` (two runs over one `lblk` at different
blocks), `cosmofs-check-chain-cycle` (the extent chain's own bound),
`test_cosmofs` (`cfs_map_block`), `fuzz_cosmofs`.

**CF11. An inode slot holds the inode its position means.** Inode `i`
lives in slot `i % 15` of the inode block at imap position `i / 15`; the
slot's `ino` must equal `i` and `i` must be below `next_ino`. A slot with
`ino == 0` or `nlink == 0` is absent to the ordinary read
(`cfs_inode_read`: `-ENOENT`). Check: `cosmofs-check-slot-identity` (a
slot that disowns itself is named, the report is partial and repair is
refused), `test_cosmofs` (index arithmetic).

**CF12. The checker repairs only from a complete walk, and calls a
filesystem clean only after one.** `report_clean` requires every class
empty and `partial` false. With `COSMOFS_CHECK_REPAIR`, only
`alloc_not_seen`, `orphan`, `nlink_wrong` and `counter_wrong` are
repaired, from the full maps; anything the walk had to skip sets
`repair_refused` and nothing is repaired. Every chain walk is bounded
(`CFS_CHECK_MAX_CHAIN` 4096, `CFS_CHECK_MAX_DEPTH` 64, the extent chain
by `CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2`) and duplicate-name
confirmations by `CFS_CHECK_MAX_DUP_CONFIRMS` (256) per directory. All
maps are allocated before the walk. Check: `cosmofs-check-clean`,
`cosmofs-check-leak`, `cosmofs-check-faults` (seven manufactured
faults; `nlink_wrong`, `orphan` and `counter_wrong` repaired,
`seen_not_alloc` and `dup` never, and nothing repaired once a dangling
or malformed entry is present),
`cosmofs-check-many-orphans` (twelve orphans, one pass, none left),
`cosmofs-check-partial` (an unreadable directory: partial, repair
refused, nothing it named freed), `cosmofs-check-snapshot` (a snapshot's
shared blocks are neither leaks nor cross-links), `fsctl-check`. Gap:
of the five walkers that can report `chain_cycle`, only the extent
chain's has a test; the deadlist, imap and directory-depth guards are
untested.

**CF13. A directory is a set of well-formed entries in a tree.** An
entry's `namelen` is at most 47 and fits its slot; no name appears
twice in one directory; the entry's type matches its inode's; a
directory is named from exactly one parent, and its `parent` field
names that directory (the root names itself). `.` and `..` are never
stored. The VFS validates names before a mutation (V12) and cosmofs
answers `-EEXIST` for an existing name. Check:
`cosmofs-check-namelen`, `cosmofs-check-dup-name` (with two names whose
hashes are asserted to collide), `cosmofs-check-two-parents`,
`cosmofs-check-faults` (`COSMOFS_CORRUPT_DIRENT`), `cosmofs-ops` (`..`
across a remount).

**CF14. Lock order is `vnode->lock` → `cfs->lock` → `mount->lock`, and
no cosmofs code waits for `mount.sync_lock`.** `cfs_sync_vnodes` runs
before `cfs->lock` is taken because `writepage` takes it;
`cfs_vnode_get` and `vnode_cache_any` take `mount->lock` under
`cfs->lock`; the writeback thread uses `mutex_trylock` on
`mount.sync_lock`. `cfs->lock` is held across pool I/O (commit, check,
scrub), which V32's rule about device operations does not cover: it
concerns the VFS's vnode lock around character devices. Check: lockdep
in debug builds (`lockdep_assert_held(&fs->lock, ...)` in
`cosmofs_sync_counted`), review.

**CF15. A `/dev/fsctl` command is one whole struct of one version, and
a failed command leaves no answer.** A write is exactly
`sizeof(struct cosmo_fsctl)`, `version == COSMO_FSCTL_VERSION`, and only
`COSMO_FSCTL_F_REPAIR` on `CHECK`; `PRIV_SYS_ADMIN` is required at open
and again at write. Each open file owns its result; a read returns all
of it or `-ERANGE`. The check classes keep their array indices across
versions. Check: `fsctl-check` (wrong version and wrong size `-EINVAL`,
`-EOPNOTSUPP`, `-ENOENT`), `fsctl-result-per-open` (two files, two
answers; an unused file reads zero bytes; a short buffer is refused),
`fsctl_selftest` in `init --selftest` (the tool lists, checks, scrubs
and repairs a real cosmofs), `unpriv_test` in `init` (an open as uid
1000 is `EACCES`, which is the node's mode answering before
`PRIV_SYS_ADMIN` is asked). Gap: the
refusal of an undefined flag bit is checked by review only.

**CF16. A mapped block's stored checksum is always the checksum of the
data at that block.** A block's checksum entry (`cfs_csum_put`, or
`cfs_aead_put` on an encrypted pool) is stored before the mapping that
makes the block readable (`set_extent`), so the entry it replaces is kept
first (`csum_save`) and put back when the mapping fails (`csum_restore`):
the old block is still mapped then, and keeps its own checksum. This holds
for a plain block (`data_write_block`) and for a compressed record's
physical blocks (`record_write`), whose entries are replaced together. A
put-back that fails abandons the transaction (`cfs_fail`). Before this
rule a failed mapping left the old data under the new checksum and every
read of it failed ("no copy matches its checksum"; inventory section 17,
after shell completeness S7). Check: `cosmofs-extent-fail` fails one
`set_extent` of a write-back with `-ENOMEM` and with `-EFBIG`
(`FI_COSMOFS_EXTENT_NOMEM`, `FI_COSMOFS_EXTENT_FBIG`) over plain blocks,
over a compressed record and on an encrypted pool, and reads every block
the open inode maps (`cosmofs_test_verify_file`);
`tools/cosmofs-extent-fail-probe.py --old` fails at `bad == 0` on both
architectures.

**CF17. A failed mutation leaves the in-memory state a commit could
publish consistent, or abandons the transaction.** The `Errors` rule of
`design.md` made concrete: whatever a failed operation changed in the
open inode, the dirty buffers or the superblock fields either describes
what is on disk and in the map, or the operation calls `cfs_fail` and no
commit runs again until unmount. The sites: `extents_store` failing after
it has started rewriting the map; `inode_sync` failing after the change it
describes (a block mapped and its predecessor queued free, an entry, a
link count); a write-back that fails partway, whose open inode maps the
blocks written before the failure and has queued the ones they replaced
free -- `cfs_writepage` and `cfs_writepages` store the inode on failure
too (`sync_after_write`), since a vnode released before the retry takes
an unstored map with it; a compressed record dropped by `record_write`'s plain
fallback or by `truncate_record` and then only partly rewritten (its
unwritten blocks would be holes); the checker's orphan repair after
`cfs_truncate_blocks` queued the frees; and a commit failing after
`cfs_snapshot_fill` added deadlist entries (a retry would add them again).
Undone instead: create and symlink give back the inode number and clear
the slot they wrote, and the symlink undo frees the checksum tree its
block grew; a failed commit gives back the bitmap blocks it reserved and
did not write. The scrub that found them is in
`docs/audit/2026-10-17-cosmofs-ordering-report.md`. Check:
`cosmofs-extent-fail` (a record dropped and then failing on its third
block: the transaction is abandoned and the committed record reads back
after a remount; before CF17 the retry committed holes; and the `evict`
case, a rewrite failing on its fifth block and released with its retry
failing too: the check is clean after the next commit and the four pages
written read back new; before, 6 blocks leaked and 6 were free under the
committed inode, `tools/cosmofs-partial-write-probe.py`). Since roadmap 1.2 B2
(CFS-6) `extents_store` and `inode_sync` no longer abandon the
transaction for an allocation: each takes what it needs before it
changes anything (`cfs_prep_meta`, `cfs_inode_prepare`; `design.md`,
"Allocate before the point of no return"), and `cfs_fail` stays behind
`inode_sync` only for a failure the preparation rules out. Check:
`cosmofs-nomem` (the chain block a file's eleventh run needs and the map
path a chown needs each fail once with `-ENOMEM`; the operation reports
it, the same operation succeeds again, commits, and reads back after a
remount); `tools/b2-defects-probe.py --case nomem --old` fails at
`retry == 0`. Gap: the checker, create, symlink, bitmap and deadlist
sites are checked by review, not by a test.

**CF21. A read-only mount writes nothing.** On a `MOUNT_RDONLY` mount
no commit publishes, no writeback thread starts, read repair and the
scrub write no copy, and a pending rollback is not switched to; the
free- and orphan-record replays run in memory only, and a repairing
check and a rekey are `-EROFS` (`design.md`, "Read-only mounts"; CFS-1).
Check: `cosmofs-ro-replay` (a pool with both records pending, mounted
read-only, checked and unmounted: every block's CRC is what it was, the
check is clean, repair is `-EROFS`; a writable mount after it writes the
replay). `tools/b2-defects-probe.py --case ro --old` fails at
`changed == 0`.

**CF22. A pool is written at its own format.** Nothing this kernel
writes needs a newer format than the pool's superblock says: no
compressed record below version 6 (`cfs_create_common`, `record_write`;
CFS-2), no symlink below 8, no record field below the version that
added it. Check: `cosmofs-version-compress` (a version-5 pool's new file
maps plain blocks; the same file at the current version is a record).
`--case compress --old` fails at `v5 == 0`.

**CF23. A check writes nothing it was not asked to, and a boot repairs
only what it may.** The check of an unmounted device mounts it read-only
unless asked to repair (CF21 then holds for it); the scrub writes no copy
without `COSMOFS_SCRUB_REPAIR`; a repair with `COSMOFS_CHECK_BOOT`
repairs only `alloc_not_seen`, `counter_wrong`, `snap_identity`,
`rollback_record`, `hold`, `orphan` and `nlink_wrong`, and nothing when
any other class was found or the walk is partial; `seen_not_alloc` is
repaired only with `COSMOFS_CHECK_OFFLINE`, which only
`cosmofs_check_dev` sets (design.md, "fsck and scrub as commands").
Check: `cosmofs-check-dev` (the read-only check changes no block's CRC;
a boot repair with a free block in use refuses and repairs nothing; the
offline repair repairs the leak and the free-in-use block, commits, and
the next check is clean), `cosmofs-scrub-modes` (a report-only scrub of
four bad copies leaves them for the next to count again; the repairing
one rewrites them), `test-fsck` (every class through the boot and the
live shell, on both architectures).

**CF24. Every chain walker is bounded.** Each loop over a chain of
blocks -- the snapshot list, a deadlist, an extent chain, the free and
orphan records, the directory tree's depth -- stops at its bound and
answers as for an unreadable block: the checker names `chain_cycle`, a
mount refuses the record, the snapshot filter holds the block it was
asked about. Roadmap 1.2 B2 bounded the four that were not
(`deadlist_settle`, `snapshot_holds`, `snaplist_insert`, the deadlist
handover in `cfs_snapshot_delete`; TEST-16). Check: `test-fsck`'s
`cycle-*` classes for the read-side walkers -- the checker's extent chain
and deadlist (`cycle`), the mount's snapshot list, free record and orphan
record (refused at their stage), the directory walk (`bad-entry`). Gap:
the four bounded by B2 are on write paths (a delete, a free, a new
snapshot), which a pool refused at boot never reaches; they are checked
by review.

**CF25. A copy that misses a write is recorded as having missed it.**
Every write the pool does not complete on a copy of a member -- the copy
not writable, or its write failed -- sets that block's region in the
copy's dirty bitmap, and a copy that leaves `ONLINE` starts from every
region written since the last durable root. The commit that writes the
copy's `COPYSTATE` record sets the regions of its own metadata and root
first, and rewrites the record if a copy's state changed while it ran;
the record a mount reads therefore names every region the copy lacks
relative to that root (`design.md`, "Format version 13"). Check:
`cosmofs-copy-bitmap` (a copy removed, files written and committed, the
pool remounted: the record's bitmap covers the region of every block
written after the removal, and the remount's record equals the one
before it); `tools/b3-copystate-probe.py --old` fails it.

**CF26. A pool with one good copy of a block keeps working; with none,
the I/O fails and only it.** A write succeeds when one `ONLINE` copy
took it; a read is served by any `ONLINE` copy; a copy that is gone
(`-ENODEV`), offline or faulted is never read, and the last `ONLINE`
copy is never faulted by a count. A block no copy can supply is `-EIO`
to that read; a write no copy took is `-EIO` and abandons the
transaction (CF17) -- neither panics. Check: `cosmofs-degraded`
(states, `/proc/mounts`, the threshold, both copies gone).

**CF27. A mount serves the newest root among the pool's member-0
devices, and attaches no lagging copy.** Whichever device the mount was
given; a device behind the root is not attached for reads. Check:
`cosmofs-copy-import`.

**CF28. A write made during a resilver reaches the copy being rebuilt.**
From the moment a copy is `RESILVERING` the pool writes every block to
it with the others (`pool_write`), and the resilver copies under
`cfs->lock`, so no block it has copied can change without that write;
only the root's write and the per-commit labels skip it, and its own
superblock slots or label are copied when it is complete. Check:
`cosmofs-resilver` (a file written mid-resilver reads back raw from
both devices); `tools/b3-copystate-probe.py --case resilver --old`
makes a resilvering copy unwritable and fails it.

**CF29. A resilver ends with the copy holding every allocated block, or
goes on after a reboot.** The walk covers every region the record names,
again if a pass ends with bits set; the target carries no superblock or
plain label until it is complete, so no mount takes it for a copy; the
cursor is recorded and a mount resumes there. Check: `cosmofs-resilver`
(every allocated block the same on both devices, the rebuilt one mounts
the pool alone), `cosmofs-resilver-resume` (an unmount mid-resilver,
the target found by its tag at the recorded cursor), `cosmofs-spare`,
`cosmofs-attach`.

**CF30. On a version-14 pool with a key block, no metadata is used
unless its tag verifies under the pool's MAC key.** Every block header,
superblock and member label of such a pool is sealed with a keyed
BLAKE2b tag in the bytes a CRC32C would occupy (`mhdr_seal_mac`,
`super_seal`, `label_seal`), and every read of one checks the tag and
nothing else: `csum_algo` is part of what the tag covers, so a block
re-sealed with a CRC is refused like any other forgery. The superblock,
not the block, decides (`cfs_super_is_mac`); a pool whose key this
kernel holds is refused if its superblock is not MAC'd. The member
table, read raw before the buffer cache can be used, is checked with
the same seal (before roadmap 1.2 B5 its CRC was not checked at all).
Check: `cosmofs-encrypted-root` (the inode map's root, the member table,
the key block, both superblock slots, a member label and a two-level
index's middle block, each forged with its CRC32C recomputed, refused);
`make test-encrypt`.

**CF31. The kernel keeps a pool's master key only after the key has
opened the pool, and only as long as a writable mount holds it.**
`KEY_LOAD` installs a key in the table only when a superblock slot of
that uuid and the key block it names verify under it
(`keyctl_dev`); a wrong key is `-EKEYREJECTED` and nothing is kept. A
writable mount copies the entry and holds it; its destruction wipes its
copy and the last holder's release wipes the entry (`cfs_destroy`,
`cfs_keytab_release`). A maintenance mount (the check on a device)
holds nothing, so the boot's check does not drop the key the root's
mount needs. A slot is written only with the mount's own master key,
and the last slot is never emptied (`keyctl_mount`). Check:
`cosmofs-encrypted-root` (wrong key refused; locked after the unmount;
`CHECK_DEV` says `LOCKED`; the check leaves the key loaded; slot writes
with the wrong key refused, the last slot kept).

**CF32. A file's checksums survive its index growing a level.** The
first checksum past the one-level index's span makes a `CSUMIDX2` root
whose entry 0 is the old index, unchanged, and raises a pool below 14
(a pool below 11, or with a version-1 key block, keeps the old
`-EFBIG`). The checker, the scrub and the reachability walk follow both
shapes, so a two-level tree is neither leaked nor unread. Check:
`cosmofs-encrypted-root` (a block past 64516 of an encrypted file and
past 516128 of a plain one read back, the check clean with one
two-level tree walked; the middle block forged is a refused read and an
unreadable block; its copy failing is a write-back's `-ENOMEM` and a
retry that commits; on a mirror, its first copy damaged is one bad copy
the scrub counts and repairs); `make test-encrypt` (a 1 GiB file).

**CF18. From format version 12 a snapshot's identity is its own: every
snapshot has a uuid no other snapshot of the filesystem has, and no id
is handed out twice, across remounts included.** The uuid is drawn when
the snapshot is taken (or when the raise from 11 converts an older
entry) and never changed, and redrawn while the list already has it, so
uniqueness does not depend on a seeded entropy pool; the list head's `max_id` is the highest id
ever used, every new id is above it and above every id on the list, and
the head is rewritten in the transaction that takes the snapshot. A pool
is raised from 11 to 12 only by a snapshot, in that snapshot's
transaction, and never below 11 (`design.md`, "Format version 12"). The
checker's `snap_identity` class names a version-12 snapshot with no
uuid, a shared uuid, or an id above the mark, and repairs each (a fresh
uuid; the mark raised). Check: `cosmofs-snap-identity` (a pool formatted
at 11 stays 11 until a snapshot, which converts a version-11 entry and
gives every snapshot a distinct uuid; an id is not reused after a delete
and a remount; a version-10 pool refuses identity and keeps its version;
a zeroed uuid is named and repaired); `test-snapshot`'s identity stage on
the installed root.

**CF19. A rollback never leaves the filesystem between trees.** At every
instant the superblock names exactly one live root and at most one
rollback record, both reached through committed transactions: a pending
record and the safety snapshot it names are published by one commit, so
a record whose safety snapshot is missing is never durable; the mount's
switch is one transaction, computed afresh from the root it read, so a
crash inside it leaves the pending record for the next mount to apply
from the start; and while a rollback is pending its target and its
safety snapshot cannot be deleted, nor the safety snapshot after it is
applied until the boot after it completes (BOOT_OK). After a switch no
deadlist names a block the live tree reaches (`design.md`, "Rollback").
Check: `cosmofs-rollback-atomic` stops the machine at every commit the
request crosses and requires neither or both with a clean checker
(`tools/b1-rollback-probe.py --old` commits the record alone first and
fails it on both architectures); `cosmofs-rollback` (request, cancel,
switch at a remount, hold until BOOT_OK, every snapshot deleted after
the switch with the filesystem clean -- the deadlists' repair -- and a
record with a bad checksum refused, named and repaired);
`test-snapshot`'s `rollback` and `crash` stages (a stop after the
record's commit, inside the switch through a `COSMOFS_STOP=1` kernel, and
after the hold's release, each followed by a clean `fsctl check`).

**CF20. A clone owns exactly the blocks its tree reaches and its
origin's does not.** A block a clone frees is kept when its origin's tree
reaches it and freed outright otherwise; no snapshot of the live tree is
asked about a clone's block, and a clone is never asked about the live
tree's; an origin outlives its clones; deleting a clone frees the
difference between the two trees and nothing else. A clone's writes are
published with its roots by the commit that makes them durable
(`design.md`, "Clones"). Check: `cosmofs-clone` (writes apart from the
snapshot and the live tree both ways, across a remount; the origin
refused deletion; the checker clean after the writes, after the clone's
deletion -- which gives back at least its own file -- and after the
origin's); `test-snapshot`'s `clone` stage on the installed root.

## 3. Rules stated in `design.md` and not checked

- The commit's flush before and after the root assumes the device honours
  `flush`; a device that did not negotiate one is taken to have no
  volatile cache (V2's gap).
- A member-0 device that missed only a root write cannot be told from a
  stale one; since roadmap 1.2 B3 its record says what it missed
  (CF25), and until a resilver it is left out (`design.md`, "The
  failure a checksum cannot see", "Format version 13").
- Space a snapshot keeps alive is not reported (V21's gap).

## 4. Where documents and code disagree

Found while writing this file; the code was read, nothing was changed.

- `docs/verification/invariants.md` F4 and `docs/verification/design.md`
  section 4 say the crash harness does not check block-bitmap
  consistency or space accounting. `check_prefix` in `cosmofscrash.c`
  runs `cosmofs_check` on every prefix image and fails a prefix that is
  not clean. `docs/verification/design.md` section 4 also names the
  harness `cosmofs-crash`; `cosmofscrash.c` registers `cosmofs-replay`.
- V22 and `design.md` ("Reading a snapshot") say a snapshot id is never
  reused while the filesystem lives. Below format version 12 that is
  per mount: `snap_max_id` (`cosmofs_snap.c`) starts from
  `fs->snap_max_id` and the ids on the list, so after the newest snapshot
  is deleted and the filesystem remounted, its id can be handed out
  again. The vnode cache is per mount, so the property V22 relies on
  still holds. From version 12 the list head carries the mark and the
  wording is true (CF18); a pool at 11 is raised by its first snapshot,
  so only a pool older than 11 still behaves the old way.
