# cosmofs write ordering and a verified upgrade from v1.0.3 — 2026-10-17

Blocker fixes before v1.0.4, outside the 1.1 roadmap under AGENTS.md's
correctness rule: a data-loss defect present in every release, and the
unverified tool the first upgrade from v1.0.3 runs. Base: main
`63c39d3a` (S7 merged). Branch `integrity-v104`.

## Before

| What | Before |
|---|---|
| A block's checksum vs. its mapping | `data_write_block` stored the new block's checksum, then called `set_extent`; `record_write` stored a record's checksums, then its `set_extent`. A failed mapping left the old data under the new checksum (inventory 17) |
| A record the plain fallback dropped | `drop_range`, then eight `data_write_block`s; a failure partway left the rest of the record as holes in the open map, which the retry of the one dirty page then committed |
| Release integrity | `SHA256SUMS` unsigned; the first upgrade from v1.0.3 ran a downloaded `cosmo-upgrade` nothing had verified |
| Untested `cosmo-upgrade` paths | the rollback refusal when neither loader trusts the other set's key; the second-run lock |

## Part 1: CF16, the checksum stays its block's

`data_write_block` and `record_write`'s compressed path now keep the
entries they are about to replace (`csum_save`: the CRC, or the
authenticated entry on an encrypted pool; `-ENOENT` means nothing was
mapped there) and put them back when `set_extent` fails, or, for a
record, when any later step fails (`csum_restore`). A put-back lands in a
CSUM block this transaction already copied; if it fails anyway the
transaction is abandoned. The order stays checksum-then-mapping, because
undoing a mapping can itself fail and can need to re-map a block that
`set_extent` has already queued free.

`set_extent` gains a test seam at its top, before it changes anything:
`FI_COSMOFS_EXTENT_NOMEM` and `FI_COSMOFS_EXTENT_FBIG` (debug builds
only; one load per kind when unarmed, no lock, no read-modify-write).
`cosmofs_test_verify_file` reads every block below a file's size through
the map and checksums of its open inode, which is what the next commit
would publish.

### `cosmofs-extent-fail`

Each case writes 16 pages, commits, rewrites, and arms one failure for
the test thread's write-back. Two kinds × four layouts:

| Layout | Fails | Then |
|---|---|---|
| plain over plain | the fifth `set_extent` | every mapped block verifies; the retry writes the pages left dirty; a remount reads the new contents, no checksum failure, a clean scrub |
| record over record | the record's own mapping | as above |
| one page that makes a record incompressible | the third plain block after `drop_range` | the transaction is abandoned; after a remount the committed contents read back whole |
| plain over plain, encrypted pool | the fifth | as the first (authenticated entries) |

On the tree before the fix every case failed for its stated reason (a
throwaway run that let all eight finish, x86-64, debug): the three
non-dropped layouts at `bad == 0` (`inode 2 block 4: no copy matches its
checksum` for the plain case, exactly the injected block), the dropped
layout at the read-back (the retry returned 0 and committed holes).

### The probe

`tools/cosmofs-extent-fail-probe.py`, debug `make test`, tree `a91090d2`:
`--old` reverse-applies the whole fix commit (`97a77e7f`, its five cosmofs
sources) and keeps the seam and the self-test (commit `4996bfee`).

| Arch | fixed | `--old` |
|---|---|---|
| x86-64 | PASS (cosmofs-extent-fail ok) | PASS (FAIL: `bad == 0`) |
| AArch64 | PASS | PASS |

## Part 2: the scrub, CF17

Every other cosmofs write path was read for the same shape: a derived
value (checksum, pointer, extent, size, link count, entry, bitmap bit,
deadlist entry, superblock field) stored where a commit could publish it
before the operation it describes had succeeded, with no undo and no
`cfs_fail`. The rule the code already stated (design.md, "Errors": a
later commit publishes whatever the in-memory structures hold) is now
invariant CF17: a failed mutation leaves them consistent, or abandons the
transaction. No site needed an on-disk format change.

| Site | Stored early | Fails after | Verdict |
|---|---|---|---|
| `data_write_block` | the block's checksum | `set_extent` | **fixed** (CF16) |
| `record_write`, compressed path | the record's checksums | a later block's write or seal, `set_extent` | **fixed** (CF16) |
| `record_write`, plain fallback | `drop_range` of the record | a plain block's write | **fixed**: `cfs_fail` (the unwritten blocks would be holes) |
| `truncate_record` | `drop_range` of the record | a surviving block's write | **fixed**: `cfs_fail` |
| `extents_store` | `in->direct` rewritten, earlier chain blocks filled | a chain block's get, CoW or allocation | **fixed**: `cfs_fail` (runs missing or overlapping, and the caller frees what the new map displaced) |
| `set_extent`, `drop_range` | displaced blocks freed only after the map is stored | -- | safe; their store failure is `extents_store`'s |
| `cfs_truncate_blocks` | extents queued free before `extents_store` | `extents_store` | safe now: `extents_store` abandons |
| `inode_sync` (13 calls: `dir_write_block`, `cfs_create_common` for mkdir, `cfs_unlink_common`, `cfs_rename`, `cfs_writepage`, `cfs_writepages`, `cfs_truncate`, `cfs_vnode_setattr`, `cfs_vnode_sync`) | a block mapped and its predecessor queued free, an entry, a link count | `cfs_inode_write` (`-ENOMEM`, metadata `-ENOSPC`, `-EIO`) | **fixed**: `inode_sync` abandons on failure (the slot would name a block the next commit releases) |
| `cfs_rename` rollback | the source entry removed from odir | `inode_sync(odir)` inside `dir_remove_slot` | **fixed** by `inode_sync`; the rollback no longer logs "namespace restored" for an abandoned transaction |
| `cfs_create_common` | `next_ino`, `inode_count`; an nlink-1 slot | `cfs_inode_write`, `dir_add` | **fixed**: the number given back (`cfs_inode_discard`), the slot cleared |
| `cfs_symlink` undo | the checksum index and leaf `data_write_block` grew | `data_write_block`, `cfs_inode_write`, `dir_add` | **fixed**: `cfs_csum_free` in the undo, the slot cleared |
| mkdir's `dir->nlink++` | in-memory link count | `inode_sync(dir)` | safe: the entry exists and the count is right; `inode_sync` abandons on failure anyway |
| `csum_block_at` | a new index or leaf linked | the leaf's allocation | safe: an empty index or leaf means no entries |
| `cfs_csum_free` | tree freed, `csum_root = 0` | the caller's inode write | safe; the caller's write is `inode_sync` or an abandoning path |
| `cfs_buf_cow` | the old block freed and the parent slot set after every failure point | -- | safe |
| `cfs_buf_new` | the allocated bit | `buf_alloc` | safe: the block goes back deferred |
| `inode_block_at` | L1/L0 copied, an empty L0 or inode block linked | a later get, CoW or allocation | safe: equivalent copies, empty blocks |
| `cfs_inode_alloc` / `cfs_inode_discard` | `next_ino`, `inode_count` | -- | safe |
| `cfs_evict` | -- | truncate, slot write | safe: `cfs_fail` on both |
| orphan replay at mount | -- | -- | safe: the mount is refused |
| checker orphan repair | extents queued free by `cfs_truncate_blocks` | it, or the zeroing slot write | **fixed**: `cfs_fail` (its own comment named the hazard) |
| checker link-count repair | -- | `cfs_inode_write` | safe: CoW of equivalent blocks |
| `commit_bitmap` | `reserved[]` blocks allocated | the reservation fixpoint, `commit_member_bitmap`, `cfs_members_store` | **fixed**: the unused reservations given back deferred (a leak the next commit made durable) |
| commit after `cfs_snapshot_fill` | deadlist entries appended | `orphan_fill`, `freelog_fill`, `pool_write`, labels, flush, `super_write` | **fixed**: `cfs_fail` when the fill appended (a retry appended the same blocks again) |
| commit, free/orphan record failure | -- | -- | safe: the reservation given back, buffers cleaned |
| commit, `pool_write` / `super_write` failure | `free_root`, `orphan_root` naming unpublished records | the retry | safe: the retry's release frees them exempt |
| `cfs_snapshot_create` | the list copied | before the entry is stored; a failed commit sets `failed` | safe |
| `cfs_snapshot_delete` | settle frees | `snap_abandon` on every later failure | safe |
| `cfs_members_store` | CoW | -- | safe |
| `cfs_read_repair`, `cfs_verify_all` | a copy rewritten in place with verified bytes | -- | safe: the same contents. No member add, remove or resilver code exists |
| `cfs_keys_rotate` | key root copied, then filled | -- | safe |
| scrub | read-only apart from the repair above | -- | safe |

The scrub was a read of the tree at `63c39d3a` plus Part 1, by a
read-only agent, each finding then checked against the code here before
it was fixed (C: `cfs_writepage` queues the old block free in
`data_write_block` before `inode_sync`; H: `commit_bitmap` frees
`reserved` on failure; I: `cfs_snapshot_fill`'s own comment calls a
repeated append "the corruption this unit exists to prevent"). Only the
`set_extent` sites have a test; the rest are checked by review
(inventory 17).

## Part 3: a verified upgrade from v1.0.3

v1.0.3's only Ed25519 verifier is `pkg`: no `cosmo-verify`, no OpenSSL.
It checks a `.cpk`'s trailer against `/etc/pkg/keys`, where the release
image put `release-2026.pub`, and `pkg install -n FILE.cpk` does that and
writes nothing (no lock, no record). BusyBox `tar` is there to unpack the
checked file. So:

- `scripts/mkrelease.py` signs each set's `SHA256SUMS` with the build's
  key (`SHA256SUMS.sig`: the 88-byte `COSMOSIG` trailer of
  `scripts/modsign.py` alone, `sign-detached`) and packs
  `cosmoos-ARCH-cosmo-upgrade.cpk` (`sbin/cosmo-upgrade`,
  `sbin/cosmo-verify`, version from the build id) with `pkgbuild.py
  pack`, signed with `PKGSIGN_KEY`.
- The release workflow's `publish` signs the joined `SHA256SUMS` with the
  release key and checks it against `tools/keys/*.pub` before
  publishing; the notes start with `docs/releases/<tag>.md` when there is
  one.
- `cosmo-upgrade DIR` checks `SHA256SUMS.sig` against the ring and the
  four set files against `SHA256SUMS` before anything else reads them
  (`userland/system/relsig.c`, which also took over its key ring, and a
  SHA-256 for user mode); a missing signature, a key outside the ring, a
  bad signature or a file that does not match is exit 3, nothing written.
- `cosmo-verify FILE SUMS SIG` (new, in the image) is the same check for
  one file.
- `tools/fetch-release.py` checks `SHA256SUMS` before using any file:
  against `SHA256SUMS.sig` and `tools/keys/*.pub`, or, for v1.0.3, the
  SHA-256 pinned in the script (`6459827d...`, what GitHub serves,
  checked with `gh release download`).
- The guide's Upgrading page gives the v1.0.3 operator the exact
  commands: `pkg install -n ./cosmoos-ARCH-cosmo-upgrade.cpk`, then `tar
  -xf` of the checked file, then `./sbin/cosmo-upgrade /tmp/up`.

## Part 4: the two untested paths

`test-upgrade` gains a boot, `hold`, between `upgrade` and `new`: the host
replaces `\EFI\BOOT`'s loader with the build's loader whose release-key
ring entry is a throwaway key's, so it starts the build's set but not
v1.0.3's, and `\cosmo\prev`'s loader (v1.0.3's) does not trust the
build's key. `cosmo-upgrade --rollback` is refused (`neither ... nor
...`), exit 3. The lock: a first run takes it, then opens each `/etc/pkg/keys/*.pub`;
one of them (`lk.pub`) is a link to a FIFO in `/tmp`, since cosmofs has
no FIFOs, and the test's write to it returns only
once the run has opened it, after binding the lock. The run then blocks on
a FIFO in place of its `kernel.elf`, lock held, and a second run is
refused, exit 3. (The first version probed with a second run in a loop,
which could take the lock before the first bound it: the matrix's x86-64
`test-upgrade` on `fee69346` failed so, the background run refused with
exit 3 and the probe failing on its missing directory.) The ESP is unchanged and the host puts the loader back.

The refuse boot gains: v1.0.3's `pkg` refusing the package before the
build's key is trusted, checking it after, and refusing one with a byte
changed; `cosmo-verify` passing a release file and refusing a changed
kernel.elf; `cosmo-upgrade` refusing a changed kernel.elf, a changed
`SHA256SUMS` and a set without `SHA256SUMS.sig`. The older refused
variants now carry a `SHA256SUMS` signed by a throwaway variant key the
boot trusts for the while, so `make test-upgrade UPGRADE_SET_TAG=v1.0.4`
runs the same test against a published set with no private key.

## Tests

`make test-upgrade` (release builds, `fee69346`), first run on each:

| Boot | x86-64 | AArch64 |
|---|---|---|
| install | 8.1 s | 11.7 s |
| refuse | 17.7 s | 22.6 s |
| written, loader | pass | pass |
| upgrade (`cosmo-upgrade` itself) | 2.6 s | 3.3 s |
| hold | 8.1 s | 11.8 s |
| new | 10.0 s | 14.0 s |
| back | 8.5 s | 12.7 s |
| forward | 5.2 s | 9.1 s |
| whole test | PASS 85.2 s | PASS 120.1 s |

The first x86-64 run did not boot: a tuple added to a list in the test's
variant table (`TypeError`), fixed before the run above.

`test_relsig` (host): SHA-256 against FIPS 180-4's examples (empty,
`abc`, the two-block message, a million `a`), a signature vector made with
`scripts/modsign.py sign-detached` from RFC 8032 TEST 1's key (verified;
unknown key; one digit changed; wrong size; bad magic), and the lookup
(exact name, not a prefix; listed twice; malformed; not hex).

### Mutations

Each in a throwaway worktree of `fee69346`, release build, `test-upgrade`
on x86-64:

| Mutation | Caught by |
|---|---|
| M6: `set_check_sums` returns 0 at once (no signature, no digests) | refuse: no `SHA256SUMS: unknown signing key ec28a00bf331f50b` line |
| M7: the rollback refusal removed (`if (false && ...)`) | hold: `cosmo-upgrade --rollback` exit 0, want 3 (it rolled back, set first) |
| M8: `hold_lock` returns 0 without binding | hold: no `another cosmo-upgrade is running` line |

M8 was run again after the lock check changed (`8e870b58`): caught, no
`another cosmo-upgrade is running` line. An intermediate version, the
ring's link still in place for the second run, caught it only by a hang
at the boot's deadline (the unlocked second run waited on the FIFO); the
link is now removed first.

## Validation

The AGENTS.md matrix on `fee69346`, each step's first run:

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test` | PASS | PASS |
| `fuzz` | PASS | PASS |
| `analyze` | clean (7 baselined, 0 unexpected) | clean (7 baselined, 0 unexpected) |
| debug `test`, `QEMU_SMP=1/2/4` | 139.4 / 161.8 / 161.6 s | 143.9 / 172.9 / 162.8 s |
| `test-smp2` | 158.2 s | 151.8 s |
| `test-chaos` | 147.6 s | 164.4 s |
| `test-harness-retry` | 158.3 s | 163.0 s |
| release `test` | 19.9 s | 21.3 s |
| `test-busybox` | 54.1 s | 59.6 s |
| `test-net-login` | 79.5 s | 92.5 s |
| `test-install` | 27.5 s | 41.3 s |
| `test-upgrade` | FAIL: the lock check | FAIL: the lock check |

Both `test-upgrade` failures were the test's (above): on x86-64 the
probe loop took the lock first; on AArch64, which ran after the first fix,
`mkfifo` in `/etc/pkg/keys` was refused (`Operation not supported`:
cosmofs has no FIFOs). With the ring's entry a link to a FIFO in `/tmp`,
removed before the second run (`8e870b58`, a change to the test alone),
`test-upgrade` passes: 86.7 s on x86-64, 115.6 s on AArch64. The rest of
the matrix was not repeated.

Branch CI: recorded in the PR.
