# NEXT SUBSYSTEM — cosmofs's metadata blocks cannot say how they are checksummed

> Constitution §68 report. This PR adds the report and the probe
> (`tools/cosmofs-metadata-csum-probe.py`); the on-disk format change
> described under "Design" and the edits in "Affected files" are planned
> work that lands in the implementation PR that follows, gated on CI. As
> committed here, a metadata block records no checksum algorithm.

## Problem

Every cosmofs metadata block carries a 32-byte header, `struct cfs_mhdr`
(`cosmofs_format.h`), whose `crc` field is a CRC32C over the block with
that field taken as zero (`block_crc`, `mhdr_seal`, `mhdr_fault_of` in
`cosmofs_core.c`). The algorithm is hardcoded three ways over: `mhdr_seal`
writes the header's one spare word (`pad`) as **zero**, `block_crc` is
always CRC32C, and the verifier consults no field to choose. The
superblock (`cfs_super`) and the pool label (`cfs_label`) are the same
shape — a CRC32C over the block with the `crc` field zeroed.

Data blocks are **not** like this. Each inode's `csum_algo`
(`cfs_inode.csum_algo`) says how its data and directory blocks are
checksummed — `CFS_CSUM_CRC32C` or `CFS_CSUM_POLY1305`, an authenticated
tag over the block's ciphertext and the generation that wrote it. The
format's own comment says why the distinction is not cosmetic:

> A CRC says accident from intact; it says nothing about a deliberate
> change, because anyone can recompute it. (`cosmofs_format.h`, on
> `CFS_CSUM_POLY1305`)

So on an encrypted cosmofs the data is authenticated and the metadata —
the superblock, the block map, the inode blocks, the checksum index
itself — is only CRC'd, and there is no field even to declare that a
stronger algorithm was used. This is audit item **8.6** ("no checksum
algorithm id in the metadata header"), the one unstruck clause left in its
`docs/audit/2026-09-deferred-work-inventory.md` §3 row after fsck
(PR #144) and the deferred-free / orphan / deadlist units closed the rest.

### Measured

`tools/cosmofs-metadata-csum-probe.py` adds a self-test,
`cosmofs-metadata-csum-id`, that seals a metadata block and inspects the
header (one debug boot, x86-64):

```
CFSPROBE: sealed metadata header records algo 0 (the spare word); verifies 1, always CRC32C
CFSPROBE: header declaring algo 2 verifies 1 under CRC32C -- the field is inert
CFSPROBE: other-algorithm block verifies 0, one-bit-corrupt block verifies 0 -- same verdict
SELFTEST: cosmofs-metadata-csum-id ... ok (2 ms)
```

- **The writer records no algorithm.** After `cfs_mhdr_seal_raw` the spare
  word is 0; the only field that could name an algorithm is zeroed on
  every seal.
- **The field is inert.** Writing a would-be algorithm id (2,
  `CFS_CSUM_POLY1305`) into that word and recomputing a valid CRC leaves
  the block verifying — the verifier applies CRC32C regardless of the
  field, so it selects nothing and a reader cannot learn one was meant.
- **An algorithm change is indistinguishable from corruption.** A block
  whose checksum is computed by a different rule fails to verify with the
  *same* verdict as a block with one flipped bit: both are `-EIO`, both
  land in `mhdr_fault_of`'s `MHDR_CRC` ("content that changed after it was
  sealed"). Nothing tells the two apart.

## Why it matters

- **Integrity asymmetry on an encrypted filesystem.** The point of the
  per-inode `CFS_CSUM_POLY1305` path is that a data block cannot be
  forged: the tag needs the file's key. Every metadata block that frames
  those data blocks — the inode, the extent map, the checksum index, the
  allocation bitmaps, the superblock — is protected only by a CRC anyone
  can recompute, and the format has no way to say otherwise. The
  encryption authenticates the leaves and not the tree.
- **A format skew reads as damage.** `mhdr_fault_of` already separates a
  wrong magic, a wrong `blkno`, a wrong kind and a bad CRC, precisely
  because "the four are very different findings" (its own comment). A
  metadata block written by any other checksum algorithm is a fifth kind
  of finding that today folds into the fourth: mount and `cosmofs_check`
  would report an algorithm or format-version skew as corruption, the one
  diagnosis a reader must not confuse with a real bad block.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| metadata header | `cfs_mhdr` (`cosmofs_format.h`) | `magic`, `kind`, `generation`, `blkno`, `crc`, `pad`; `pad` is written 0 and read by nothing |
| seal | `mhdr_seal` (`cosmofs_core.c:42`) | `pad = 0`; `crc = block_crc(...)`, always CRC32C |
| verify | `mhdr_fault_of` / `mhdr_check` (`cosmofs_core.c:67`) | recomputes CRC32C; `MHDR_CRC` on mismatch, no algorithm class |
| super / label | `cfs_super_ok` (`cosmofs_core.c:104`), `label_crc` (`cosmofs_member.c`) | CRC32C over the block |
| data (the contrast) | `cfs_inode.csum_algo` | per inode: `CFS_CSUM_CRC32C` or `CFS_CSUM_POLY1305` |

## Design

### 1. The metadata header declares its checksum algorithm

`cfs_mhdr`'s spare `pad` word becomes `csum_algo`. `mhdr_seal` writes the
current algorithm (`CFS_CSUM_CRC32C`) and computes the checksum with it.
The field sits inside the CRC'd region, so it is itself covered by the
checksum it names.

### 2. Backward compatibility without a version at every verify site

Old images (format < 11) wrote `pad = 0` on every metadata block, and
those blocks are CRC32C. So the verifier maps **both** `0` (legacy) and
`CFS_CSUM_CRC32C` (1, explicit) to CRC32C; new writes set 1. A value the
build does not support is a new fault class, **`MHDR_ALGO`**, distinct
from `MHDR_CRC`. This needs no `sb.version` plumbed into `mhdr_fault_of`,
which several callers reach with only the block in hand
(`cfs_read_repair`'s `mhdr_ok` callback).

`CFS_VERSION` goes 10 → 11 to mark "writes `csum_algo` explicitly";
`CFS_VERSION_MIN` stays 2, so every older image still mounts.

`CFS_CSUM_NONE` is 0 for *data* (a block with no checksum); for a metadata
header 0 means legacy-CRC32C, because a metadata block always had a CRC.
The verifier's map encodes exactly that, and the two meanings never meet —
`csum_algo` is a header field, `cfs_inode.csum_algo` is an inode field.

### 3. The super and the label keep a fixed bootstrap algorithm

They are read before anything else — the mount reads the superblock to
learn the format, and the label to identify the member — so their
checksum algorithm cannot itself be something the block has to be parsed
to discover. They stay CRC32C by definition, documented as the format's
fixed bootstrap checksum (as ext4 fixes its superblock's checksum type).
Only `CFSM`-headed metadata carries a per-header `csum_algo`.

### 4. What this enables, and does not build

Authenticating metadata — a Poly1305 tag over metadata on an encrypted
filesystem — is the reason to have the field, and is **not** built here.
It cannot be unconditional: the master key is unwrapped from the
`CFS_KIND_KEYS` block, which is itself a metadata block verified by
`cfs_mhdr_ok` *before* `cfs_keys_unwrap` runs
(`cosmofs_crypt.c:153`), so at least the keys block must stay checkable
without the key. This unit makes the algorithm declared, read, dispatched
(CRC32C only for now) and an unknown value diagnosed distinctly; choosing
which metadata to authenticate, and threading the key to those reads, is a
unit of its own.

## Affected files

| file | change |
|---|---|
| `kernel-services/filesystem/cosmofs/cosmofs_format.h` | `cfs_mhdr.pad` → `csum_algo`; `CFS_VERSION` 11 |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `mhdr_seal` writes `csum_algo`; `mhdr_fault_of` dispatches on it and adds `MHDR_ALGO`; the mount/verify message names an unsupported algorithm |
| `kernel-services/filesystem/cosmofs/cosmofs_check.c` | a `cosmofs_check` finding/message for an unsupported metadata checksum algorithm |
| `docs/kernel-services/filesystem/cosmofs/design.md` | the metadata header declares its checksum algorithm; the super and label are the fixed bootstrap |
| `README.md` | Status entry |

## APIs

None. This is an on-disk format change: the metadata header's spare word
gains meaning and the format version becomes 11. No syscall or user ABI
changes; `cfs_mhdr_ok`'s signature is unchanged. The `mhdr_fault`
enumeration gains `MHDR_ALGO`, which is internal to cosmofs.

## Tests

| test | proves |
|---|---|
| `cosmofs-metadata-csum-id` | a sealed metadata block declares its algorithm and verifies; a block declaring an unsupported algorithm fails as `MHDR_ALGO`, distinct from a CRC mismatch's `MHDR_CRC`; a legacy block (`csum_algo == 0`) still verifies as CRC32C |

**Planned mutations** (each alone, both architectures, boot confirmed):
- the verifier ignoring `csum_algo` (always CRC32C): a block declaring an
  unsupported algorithm reads as `MHDR_CRC`, so the test distinguishing
  the two fault classes fails.
- `MHDR_ALGO` folded back into `MHDR_CRC`: the test asserting the distinct
  fault fails.
- `mhdr_seal` leaving `csum_algo` at 0 on a version-11 write: a legacy
  block and a new block become indistinguishable, which is benign for
  CRC32C but the test that a new block reads back algorithm 1 fails.

## Benchmarks

None. One field read and a branch per metadata verify; the checksum work
is unchanged.

## Risks

- **The algorithm field is inside the CRC.** A corruption that flips it to
  an unsupported value reads as `MHDR_ALGO` rather than `MHDR_CRC`. This is
  acceptable and is the point: both are "this block did not verify"; the
  message is more precise, not less. Recorded so the distinction is not
  read as a regression.
- **`csum_algo == 0` must map to CRC32C for a header, never to
  `CFS_CSUM_NONE`.** A metadata block always carried a CRC, so 0 is legacy
  CRC32C here; only for a data inode does 0 mean "no checksum". The
  verifier's map must encode this and a test must pin it, or an old image's
  metadata reads as unchecked.
- **The super and label staying CRC32C is a deliberate asymmetry**, not an
  oversight. Recorded so a future reader does not "fix" them to carry an
  algorithm field they cannot use before they are parsed.

## Alternatives considered

- **Version-gate the read** (`sb.version < 11` ⇒ CRC32C) instead of mapping
  `0 → CRC32C`. It needs the format version at every verify site, and
  several — `cfs_read_repair`'s `mhdr_ok` callback among them — have only
  the block. Mapping `0` and `1` to CRC32C keeps the rule local to the
  verifier and needs nothing threaded through.
- **Declare the metadata algorithm in the superblock only**, one field for
  the whole filesystem. Simpler to write, but a per-block header then
  cannot be verified without first reading the super, and paths that
  verify a block in isolation (repair, scrub) would have to carry the
  superblock's word. A per-header field self-describes each block.
- **Authenticate metadata now** (a Poly1305 tag over metadata blocks on an
  encrypted filesystem). Larger, and it cannot be unconditional: the key
  comes from a metadata block read before the key exists (§Design 4). The
  algorithm id is the prerequisite for it, and is the whole of this unit.
