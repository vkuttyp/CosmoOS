# An encrypted root (roadmap 1.2 B5)

Date: 2026-10-24. Base: `usr8-pkg11-build7` at `0805c67c` (PR #446,
itself on `main` at `78afb0b8`): B5 builds on #446's `/run` and ESP
changes, so its PRs are stacked on it. PR #447 (this part: the kernel).

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

## What is not done in this part

User space -- `cosmo-key`, the unlock at boot, `cosmo-install
--encrypt` -- and the acceptance test are PR #448.
