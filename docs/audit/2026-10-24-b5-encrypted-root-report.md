# An encrypted root (roadmap 1.2 B5)

Date: 2026-10-24. Base: `usr8-pkg11-build7` at `0805c67c` (PR #446,
itself on `main` at `78afb0b8`): B5 builds on #446's `/run` and ESP
changes, so its PRs are stacked on it. PR #447 (the kernel) and PR #448
(user space and the acceptance test, stacked on #447).

## Scope and baseline

Roadmap 1.2 B5: `cosmo-install --encrypt`, a passphrase at boot, the
encrypted-file cap lifted (CFS-10), metadata authenticated with a keyed
checksum (CFS-7). Before it, cosmofs could encrypt (format 7: per-file
ChaCha20 keys from a master key, data and directory blocks sealed with
Poly1305), but its key arrived through a firmware channel only a test
used, its metadata was CRC32C (recomputable by anyone with the disk), and
an encrypted file stopped at 64516 blocks (252 MiB).

The owner's decisions, as built: the key hierarchy (a random master key
in up to eight slots, Argon2id or a key file, AEAD-sealed), the keyed MAC
in every metadata checksum domain as a format version, the two-level
checksum index, a boot that unwraps in user space and hands the key to
the kernel, and an installer that asks twice.

## What changed (PR #447)

The design is `docs/kernel-services/filesystem/cosmofs/design.md`,
"Format version 14"; invariants CF30-CF32.

- **The metadata MAC (CFS-7).** On a version-14 pool with a key block
  (`cfs_super_is_mac`), the eight bytes each domain keeps its CRC32C in --
  `cfs_mhdr`'s `crc` and `csum_algo`, the superblock's and the label's
  `crc` and `pad` -- hold a 64-bit BLAKE2b tag keyed by
  BLAKE2b(master, "cosmofs metadata MAC v1"), over a domain byte and the
  block. Every read checks the tag and nothing else; a CRC'd superblock of
  a pool whose key is loaded is refused. `kernel/security/blake2b.c` is
  new (keyed and plain), host-tested against Python's hashlib.
- **The key block, version 2.** The pool's uuid and eight slots (`struct
  cfs_keyslot`, 96 bytes): kind, Argon2id's t, m and lanes, a salt, the
  master key sealed with ChaCha20-Poly1305, the tag. The associated data
  is the uuid, the index, the kind, the parameters and the salt
  (`cfs_keyslot_aad`), so a slot moved, re-kinded or weakened does not
  open. The kernel never opens a slot.
- **The key table and `/dev/fsctl`'s `KEY_*` (new ops 21-24, a 224-byte
  command).** `KEY_LOAD` keeps a master key only after a superblock slot
  and the key block verify under it; a writable mount takes and holds it,
  its destruction wipes its copy and the last release wipes the entry; a
  maintenance mount holds nothing. `KEY_INFO` hands out a pool's slots
  unverified (they authenticate themselves); `KEY_SLOT` writes a slot
  with the mount's master key and never empties the last; `KEY_UNLOAD`.
  `CHECK_DEV` of a locked pool says `COSMO_FSCTL_DEV_F_LOCKED`; a mount
  says `-ENOKEY`.
- **`/dev/blkctl` `FORMAT_KEYED`.** One device or a mirror, the label,
  the pool's uuid (the slots' associated data names it, so user space
  chooses it), the master key and the slots; the kernel MACs everything
  it formats and loads the key.
- **The two-level checksum index (CFS-10).** A file whose checksums pass
  the one-level index's span gets a `CFS_KIND_CSUMIDX2` root whose entry
  0 is the old index; the checker, the scrub and the reachability walk
  follow both shapes. A plain pool is raised to 14 for it and keeps
  CRC32C; one below 11 or with a version-1 key block keeps `-EFBIG`.
- **A native `getrandom`** (`SYS_getrandom` 106, at most 256 bytes, after
  seeding): the native personality had only `AT_RANDOM`.
- **CFS-31, found and fixed.** `cfs_members_load` read the member table
  raw and checked its magic, kind and address only: a table damaged in
  place was used on a plain pool, and a forged one on an encrypted pool.
  It now checks the pool's seal. Its own commit (`8521fc69`), so the
  probe's `--old` is that commit's `cosmofs_member.c` reversed.
- The format marker is `cosmofs-format-max=14`; `cosmo-upgrade` already
  refuses a set whose kernel reads less than the root's format.

## Tests (PR #447)

- `cosmofs-encrypted-root` (`cosmofs_keytest.c`): keyed formats refused
  for a bad slot, no slot and no uuid; a pool made at 14 that mounts at
  once; a sparse file with a block at 70000 through a `CSUMIDX2`, checked
  clean with one two-level tree walked; slot writes with the wrong key
  refused and the last slot kept; locked after the unmount (`-ENOKEY`,
  `LOCKED`), a wrong key refused, the right one kept across the check;
  each domain forged with its CRC32C recomputed and refused -- the inode
  map's root (mount error -5), the member table and the key block (the
  check's mount falls back to the older root: flags `0x11`), both
  superblock slots (-5, `SUPER_NONE`), member 1's label (-19), the far
  file's middle index block (the read refused, the check counting it
  unreadable); a plain pool's member table damaged in place refused
  (CFS-31); a plain pool's file past 516128 blocks raised to 14 with
  CRC32C kept. 576 ms on x86-64, 650 ms on AArch64 (before the review
  fixes below).
- Host: `test_crypto` gains BLAKE2b (RFC 7693 and keyed hashlib vectors);
  `test_cosmofs` the format-14 layout, `cfs_super_is_mac`, the slot's
  associated data and nonce, the two-level span.
- `tools/cfs31-probe.py [--old] [--arch]`:

  | arch | fixed | `--old` |
  |---|---|---|
  | x86_64 | PASS: the damaged member table refused (make test passes) | PASS: a plain pool's member table damaged in place was ACCEPTED (cosmofs-encrypted-root FAIL) |
  | aarch64 | PASS, the same | PASS, the same |

## Validation (PR #447)

The full matrix on `ed8a72ca`, every step exit 0 with its own PASS line:

| step | x86_64 | aarch64 |
|---|---|---|
| analyze | clean (7 baselined, 0 unexpected) | clean |
| debug `make test`, `QEMU_SMP=1` / `2` / `4` | PASS 149.9 / 158.1 / 175.7 s | PASS 148.7 / 167.5 / 182.6 s |
| `test-smp2` | PASS 159.1 s | PASS 173.9 s |
| `test-chaos` | PASS 161.4 s | PASS 177.9 s |
| `test-harness-retry` | PASS 170.6 s | PASS 174.1 s |
| `test-crash` | PASS 136.4 s | PASS 142.3 s |
| `BUILD=release test` | PASS 18.5 s | PASS 21.5 s |
| release `test-install` | PASS 30.7 s | PASS 46.9 s |
| release `test-mirror` | PASS 203.5 s | PASS 260.7 s |
| release `test-upgrade` | PASS (pkgtx 135.9 s, be 443.9 s) | PASS (pkgtx 153.2 s, be 541.9 s) |

The network harness passed in every debug boot. `make host-test`,
`host-test-fetch` and `fuzz` (whose cosmofs target now links BLAKE2b)
passed; `make reproducible`: yes. The first run of this matrix was
stopped after its first probes, when the keyed format was found to need
the pool's uuid from user space (`ed8a72ca`); this is the run on that
commit.

## Review fixes (PR #447)

Codex found three defects in this PR's own code (none on main), fixed
in `9b25c98c`:

- `csum_level` released the block whose copy-on-write failed and left
  the caller's pointer set, so `csum_block_at` released the mid-level
  index a second time: a panic (`cfs_buf_put`'s KASSERT) where the
  write should have been `-ENOMEM`. The pointer is now NULL on every
  failure. A seam, `FI_COSMOFS_CSUM_COW`, fails an index block's copy.
- `scrub_csum` read each index block through the cache before verifying
  its copies; on a writable mirror that read repaired a bad first copy,
  and the scrub neither counted nor reported it. Copies are verified
  first, the root as either index kind.
- A failed allocation for an index's children skipped the tree without
  stopping the scrub; it now stops it with `-ENOMEM`, as `tally` does.
  No test: the allocation is not reachable by a seam without counting
  the scrub's other allocations.

`cosmofs-encrypted-root` gains a test for each of the first two, and
each was run without its fix on x86-64:

| test | with the fix | without it |
|---|---|---|
| the far block rewritten, the mid block's copy failing | sync -12 (hits 1), retry 0 | `KERNEL PANIC: assertion failed: b->refs > 0` (`cfs_buf_put`) |
| a mirror's mid block, first copy damaged, scrubbed | 1 bad, 1 repaired, 0 unrecoverable | 0 bad, 0 repaired: FAIL |

With the fixes, debug `make test` passes on x86-64 (162.7 s, the
self-test 940 ms) and AArch64 (176.3 s, 963 ms); release `make test`,
`make fuzz`, `make host-test`, and release `test-fsck` (238.1 s /
278.8 s) and `test-mirror` (169.1 s / 224.1 s) pass on x86-64 /
AArch64.

## What changed (PR #448)

User space, on #447's kernel. The design is `docs/userland/design.md`
(init's unlock, `cosmo-install --encrypt`, "cosmo-key"); invariant U16.

- **The slots, sealed in user space.** `userland/lib/argon2.c` is
  Argon2id (RFC 9106) over the kernel's BLAKE2b; `keyslot.c` seals the
  master key with ChaCha20-Poly1305 (the kernel's `chacha20_seal`) under
  a key from a passphrase (Argon2id, 64 MiB, three passes, one lane) or
  a 32-byte key file, the slot's associated data and nonce as #447 fixed
  them. `secret.c` reads a passphrase with echo off and wipes it;
  `fskey.c` wraps `/dev/fsctl`'s `KEY_*`.
- **`cosmo-key`** (`userland/system/cosmo-key.c`): `list`, `unlock
  [--boot]`, `add`, `change`, `remove SLOT`, `bench`. An unlock tries
  slot 0's key file on the ESP (`\cosmo.key`) when there is one, then
  asks `passphrase for <pool>:` up to three times; the third wrong one
  exits 3. It prints how long Argon2id took.
- **init** runs `cosmo-key unlock --boot` on the root device before its
  check and its mount when the pool is locked; exit 3 restarts the
  machine with nothing mounted. The prompt is on every console.
- **`cosmo-install --encrypt [--key-file]`** asks for the passphrase
  twice before it writes anything, draws the pool's uuid and master key
  (`getrandom`), seals slot 0 (or the key file in 0 and the passphrase
  in 1, the file written to each ESP) and formats with `FORMAT_KEYED`,
  one disk or a mirror with one key.
- `fsck.cosmofs` and the boot report say `LOCKED`; `blkid` says
  `ENCRYPTED="yes"`; libc has `ENOKEY` and `EKEYREJECTED`;
  `cosmo-upgrade`'s format gate names the encrypted root.

## The acceptance test (PR #448)

`make test-encrypt` (`tests/boot/encrypt_test.py`, CI job `encrypt`,
release builds), stages: **install** (disk A with `--encrypt`, the
harness typing the passphrase twice; B and B2 a mirror with
`--key-file`; a mismatched second passphrase writes nothing),
**wrong** (three wrong passphrases: the machine restarts with nothing
mounted), **boot** (one wrong, then the right one; a marker written,
slot 1 added), **plaintext** (no disk holds the marker), **forge**
(each domain on a clone of disk A, the block altered and its CRC32C
recomputed: superblock, inode map root, member table, key block, a
spare's label), **slot2** (slot 1's passphrase; 1 GiB of incompressible
data written, its sha256 taken, the check clean with a two-level tree
walked), **readback** (after a reboot, the same sha256), **keyfile** (B
and B2 boot with no prompt; B2 alone by its own ESP's copy). Its last
stage runs `test-snapshot`, `test-fsck`, `test-mirror`, `test-pkgtx` and
`test-be` again with every install `--encrypt` and the passphrase typed
at every boot.

| | x86_64 | aarch64 |
|---|---|---|
| test-encrypt | PASS in 1660.0 s | PASS in 1979.9 s |
| Argon2id 64 MiB, t=3: sealing (install) | 2.05 s | 0.76 s |
| Argon2id: unlocking at boot | 1.06 s | 0.81 s |
| 1 GiB written / read back | 1275 s / 248 s | 1614 s / 207 s |
| forge | all five refused | all five refused |
| test-snapshot, encrypted | PASS 140.5 s | PASS 160.7 s |
| test-fsck, encrypted | PASS 273.5 s | PASS 310.2 s |
| test-mirror, encrypted | PASS 219.3 s | PASS 251.9 s |
| test-pkgtx, encrypted | PASS 140.4 s | PASS 162.8 s |
| test-be, encrypted | PASS 515.4 s | PASS 519.7 s |

These runs were on `7e259e9a` (#448 before the rebase onto #447's
review fixes, which change only the kernel's checksum-index walk and
scrub). Argon2id's cost is about a second on both: the parameters are
stored in each slot, and `cosmo-key change --passes/--memory` raises
them. The x86-64 seal at install is slower than the unlock (a cold
first touch of 64 MiB in the live system); AArch64 under TCG is not the
slower architecture for Argon2id here.

test-fsck's `superblock` class is the one class that behaves
differently on an encrypted root: the harness's forged slot names
another pool, which cannot be authenticated, so it is left out and the
root mounts clean; the test asserts that.

**CFS-32, found by this test.** The first 1 GiB file was compressible
(`dd` of a repeated pattern): LZ4 makes one run per 32 KiB record, and
past `CFS_MAX_EXTENTS` runs (about 128 MiB) every commit failed with
`-EFBIG` until memory ran out. Reproduced on a plain pool at 150 MB, so
it is not B5's; backlog CFS-32 (correctness). The test writes
incompressible data.

## Validation (PR #448)

The full matrix on `a1106611`, every step exit 0 with its own PASS line
except one, rerun:

| step | x86_64 | aarch64 |
|---|---|---|
| analyze | clean (7 baselined, 0 unexpected) | clean |
| debug `make test`, `QEMU_SMP=1` / `2` / `4` | PASS 159.4 / 164.0 / 165.5 s | FAIL (`signal-stop` check 9; rerun PASS 154.1 s) / PASS 170.4 / 176.7 s |
| `test-smp2` | PASS 159.3 s | PASS 161.1 s |
| `test-chaos` | PASS 172.5 s | PASS 177.3 s |
| `test-harness-retry` | PASS 170.6 s | PASS 175.2 s |
| `test-crash` | PASS 136.3 s | PASS 138.9 s |
| `BUILD=release test` | PASS 18.2 s | PASS 21.7 s |
| release `test-install` | PASS 33.0 s | PASS 46.6 s |
| release `test-mirror` | PASS 169.9 s | PASS 226.9 s |
| release `test-upgrade` | PASS (be 430.7 s) | PASS (be 533.4 s) |
| release `test-snapshot` | PASS 129.1 s | PASS 142.7 s |
| release `test-fsck` | PASS 241.3 s | PASS 309.2 s |

The network harness passed in every debug boot. `make host-test` (with
`test_keyslot` and `tools/cosmofs_key.py --self-test`),
`host-test-fetch` and `fuzz` passed; `make reproducible`: yes.

The AArch64 `QEMU_SMP=1` failure is backlog LX-1's, a fourth sighting in
`docs/testing/flakes.md`: the child (pid 268) exited with status 7
before its parent (pid 267) with status 9, a fast exit hiding an
unreported continue (`child_event_locked` checks `EXITED` before
`CONTINUED`). #448 changes no kernel code. The rerun on the same commit
passed (load average 5 before it, 1.5 after).

## Backlog

Struck: CFS-7 and CFS-10 (#447), CFS-31 (found and fixed in #447).
Added: CFS-32 (above); out of B5's scope, as the roadmap lists them:
CFS-33 (encrypting an existing root in place), CFS-34 (per-user keys),
BOOT-9 (TPM-sealed keys), MEM-10 (encrypted swap); CFS-35 (a block put
back to an older version of itself verifies: the MAC binds the address
and the pool, not the generation); BOOT-4 notes that the MAC is not a
verified root.

With #448, roadmap 1.2 B5 is done, and direction B with it.
