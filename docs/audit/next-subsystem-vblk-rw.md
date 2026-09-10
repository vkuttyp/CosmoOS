# NEXT SUBSYSTEM — a writable root: virtio-blk writes and flush, so the guest can persist

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the write side of the guest's virtio-blk device —
`VIRTIO_BLK_T_OUT` and `VIRTIO_BLK_T_FLUSH` over a backing file the owner
opens read-write, so a guest can change its disk and make the change
durable.**

## Problem

A guest now has a disk, but it is read-only. `next-subsystem-vblk.md`
built the device deliberately as read-only: it offers `VIRTIO_BLK_F_RO`,
and a `VIRTIO_BLK_T_OUT` that arrives anyway is completed as
`VIRTIO_BLK_S_UNSUPP`. That was enough to reach the milestone that unit
aimed at — a root filesystem to mount, so the kernel gets past
`Unable to mount root fs` into userspace — because reaching userspace
reads a disk, it does not write one.

A system that cannot write its disk cannot do the next thing a system
does: keep anything. A guest that boots to a shell cannot create a file,
a log cannot record a line, a package cannot be installed, an `fsck`
cannot repair. The read-only root is a demonstration that the kernel
runs; a writable root is the difference between a demonstration and a
machine.

The device is one request type short of it. The virtqueue walk, the
transport, the interrupt, the hostile-input discipline and the work
bounds are all built and tested; a write is the same walk with the data
moving the other way — out of guest memory, into the file — and a flush
is a request that carries no data at all.

## Current implementation

**The device serves reads only.** `userland/system/vblk.c`'s `serve_one`
handles `VIRTIO_BLK_T_IN`: it reads the request header, walks the data
descriptors — each of which **must be device-writable** (`VQ_DESC_F_WRITE`),
because a read fills them — serves the sectors from the disk through the
`disk_read` callback into guest memory through `write_guest`, and writes
the status byte. Any other request type sets `unsupported`, and the
request completes as `VIRTIO_BLK_S_UNSUPP` with no data moved.

**The backing file is opened read-only.** `vmctl`'s `--disk FILE` opens
the file `O_RDONLY` and computes the capacity from its size; `vio_disk_read`
is an `lseek`+`read`. There is no `disk_write` anywhere: the `struct
vblk_io` callback set is `read_guest`, `write_guest`, `disk_read`, and
nothing writes the file.

**The device advertises itself read-only.** `vio_reg` answers
`DeviceFeatures` with `VIRTIO_BLK_F_RO` (bit 5) in the low word and
`VIRTIO_F_VERSION_1` (bit 32) in the high word, and offers no other
optional feature. A conforming Linux driver reads the RO bit and mounts
`/dev/vda` read-only; it never submits a write, and the kernel's own
write-back is suppressed for the device.

**The data plane is direction-agnostic underneath.** The virtqueue walk,
the used ring, the SPI the owner raises through the guest's distributor,
the per-request and per-notification work bounds, the atomic publish, and
the exhaustive hostile-ring refusals are all independent of which way the
data moves. The read/write direction lives entirely in `serve_one` (the
`VQ_DESC_F_WRITE` check on data buffers, and the `disk_read`/`write_guest`
pair). This is the seam a write plugs into.

## Why it matters

A read-only root reaches userspace; a writable root is what userspace is
*for*. The whole point of running a kernel that was written for the
architecture and not for the hypervisor is to run its programs, and a
program that cannot write is a screenshot. The immediate uses:

- **A guest that keeps state across boots.** A filesystem mounted
  read-write, a file written and still there next time.
- **A real distribution's boot.** Many init systems remount the root
  read-write and write to `/run`, `/var`, `/tmp` early; a strictly
  read-only root stalls or degrades them. A writable root is the
  difference between "the kernel reached userspace" and "the userspace
  came up."
- **The device's durability contract, stated honestly.** A disk that can
  be written raises a question a read-only disk never does: when is a
  write *safe*? Answering it — `VIRTIO_BLK_F_FLUSH`, and a flush the
  owner honours with `fsync` — is the substance of this unit, and it is
  the same discipline the host filesystem's commit path already lives by.

It also completes the device honestly. The vblk report listed writes as
the first deliberately-deferred item; deferring them was right for that
milestone, but a device that refuses the request its own transport most
naturally carries is unfinished. This unit finishes it.

## Proposed design

### 1. Read-write is opt-in, because writing the owner's file is not free

A read-write disk is a new flag, not a change to `--disk`. `--disk FILE`
keeps its meaning exactly: open `O_RDONLY`, offer `VIRTIO_BLK_F_RO`,
refuse writes. A new `--disk-rw FILE` opens the file `O_RDWR` and presents
a writable device. The reason to split them rather than always open
read-write is data safety: a guest that writes a file the owner meant to
keep is silent corruption, and the owner should have to say so. The
mode is a property of how the owner opened the file, carried on `struct
vio` (the `disk_fd` plus a `writable` flag), not something the guest can
negotiate its way into.

### 2. The device offers a writable disk, and a flush

When the disk is writable, `vio_reg`'s `DeviceFeatures` drops
`VIRTIO_BLK_F_RO` and adds `VIRTIO_BLK_F_FLUSH` (bit 9), keeping
`VIRTIO_F_VERSION_1`. `VIRTIO_BLK_F_FLUSH` is not optional dressing: it is
how the device tells the guest that writes may be buffered and that
`VIRTIO_BLK_T_FLUSH` is the barrier that makes them durable. Without that
bit a conforming driver assumes every completed write is already on
stable storage (write-through), which the host page cache does not
guarantee — so a writable device that does **not** advertise flush would
be lying about durability. Offering flush and honouring it is the correct
pairing; offering neither and fsync-ing every write is the alternative
(§Alternatives), rejected for cost.

### 3. `serve_one` learns the write direction

The walk is unchanged; the data step branches on the request type read
from the header:

- **`VIRTIO_BLK_T_IN` (read):** as today — data descriptors must be
  device-writable, and each is filled from the disk (`disk_read` →
  `write_guest`). `used_len` is the bytes read plus the status byte.
- **`VIRTIO_BLK_T_OUT` (write):** data descriptors must be
  device-**readable** (a write buffer is not writable by the device), and
  each is read from guest memory and written to the disk (`read_guest` →
  `disk_write`). No data is returned to the guest, so `used_len` is just
  the status byte. A write is refused (`-1`, the hostile-ring path) if a
  data descriptor is marked `VQ_DESC_F_WRITE` — the driver got the
  direction wrong — exactly mirroring the existing "a read buffer must be
  writable" check.
- **`VIRTIO_BLK_T_FLUSH`:** carries no data descriptors (only header and
  status). The device calls `disk_flush`; the status is `OK` on success,
  `IOERR` on failure.
- **On a read-only disk:** `T_OUT` and `T_FLUSH` remain
  `VIRTIO_BLK_S_UNSUPP`, unchanged — a read-only device advertises no
  flush and serves no write.

Every bound the read path grew in review applies to the write path
unchanged, because they live above the direction branch: the descriptor
index bound, the chain-loop bound, the per-request `VBLK_REQ_MAX_BYTES`
cap (now on bytes read *from* the guest as well as written to it), the
per-notification work ceiling, the available-ring backlog refusal, the
atomic publish (advance `used_idx`/`last_avail` only after both used-ring
writes land), and the rule that a fault returns `-1` and is never counted
as progress. A write past the end of the disk is an I/O error, not a
file that grows: `disk_write` is bounded to `capacity_sectors` the same
way `disk_read` is, so a guest can never make the backing file larger or
write outside it.

### 4. The callbacks the device reaches the disk through

`struct vblk_io` gains two callbacks beside `disk_read`:

- `disk_write(ctx, off, buf, len)` — write `len` bytes at byte offset
  `off`; `0` ok, `<0` an error (bounded by the caller to the capacity, so
  a short or out-of-range write is the device's fault to prevent, not the
  callback's to discover).
- `disk_flush(ctx)` — make prior writes durable; `0` ok, `<0` an error.

In `vmctl` these are `lseek`+`write` and `fsync` of the `O_RDWR` file. On
a read-only disk they are absent (`NULL`), and `serve_one` never reaches
them because `T_OUT`/`T_FLUSH` are refused before the data step. The host
test supplies an in-memory disk whose `disk_write` records into the same
array `disk_read` serves from, so a write-then-read round-trips.

### 5. The durability story, and what a crash costs

Writes go to the file through the host page cache; `VIRTIO_BLK_T_FLUSH`
is an `fsync`. A guest filesystem that journals (ext4, xfs) issues a
flush at each commit, so its on-disk state is always consistent up to the
last completed flush — exactly the guarantee the flush feature promises.
A crash of the owner (or the host) between a write and the next flush
loses the unflushed writes, which is what `VIRTIO_BLK_F_FLUSH` tells the
guest to expect and what its journal is designed around. This is the
same contract the host filesystem's own commit path lives by (a root is
durable when its blocks are stable *and* flushed), stated for the guest's
disk. The honest gap, recorded in the design doc: the test harness cannot
kill the owner between a write and a flush and inspect the file, so the
crash-consistency of the interruption is reasoned about, not executed —
the same limit the storage units noted for the replay harness.

### 6. The milestone

Two, one gated and one demonstrated:

- **Gated, in the harness:** a guest writes a sector, flushes, reads it
  back, and gets the bytes it wrote (`el2-virtq-device-rw`); and the
  device-side walk writes an in-memory disk and refuses hostile write
  rings (`test_vblk_dev`). Neither needs Linux or a large guest.
- **Demonstrated, reproducible:** a stock Linux mounts `/dev/vda`
  read-write, writes a file, `sync`s, powers off, and the change is
  present in the backing file when the owner re-reads it — the same
  `QEMU_MEM=2G`-and-a-root-image shape the read path's Linux
  demonstration has, not a CI gate.

### 7. Deliberately out of scope

- **Discard and write-zeroes** (`VIRTIO_BLK_T_DISCARD`,
  `WRITE_ZEROES`). Thin-provisioning hints; a correct device may ignore
  them, and this one does not advertise them, so a guest never sends
  them.
- **A copy-on-write overlay** (writes to a scratch file, base image kept
  read-only). Useful for protecting a golden image, but it is a policy on
  top of the write path, not the write path; the host filesystem already
  snapshots, and this belongs to the owner's file management, not the
  device.
- **Multiple queues, indirect descriptors, `RING_EVENT_IDX`, a barrier
  richer than flush.** Same deferral as the read unit; one queue and the
  simple descriptor path still carry a writable root.
- **Committing a writable image.** Still someone's binary; the test
  creates its own scratch file (`mkstemp`) and removes it, so nothing
  large or foreign enters the tree.
- **A writable disk by default.** `--disk` stays read-only; writing is
  `--disk-rw`, because turning a disk writable without being asked is how
  data is lost.

## Affected files

- `userland/system/vblk.h` — `disk_write`/`disk_flush` in `struct
  vblk_io`; the write request types and status already exist
  (`VIRTIO_BLK_T_OUT`); add `VIRTIO_BLK_T_FLUSH` and `VIRTIO_BLK_F_FLUSH`.
- `userland/system/vblk.c` — the direction branch in `serve_one`; the
  flush request; the `disk_write` capacity bound.
- `userland/system/vmctl.c` — `--disk-rw`; open `O_RDWR`; the `writable`
  flag on `struct vio`; `vio_disk_write`/`vio_disk_flush`; the
  feature-register change (drop `F_RO`, add `F_FLUSH`) when writable.
- `tests/host/test_vblk_dev.c` — write round-trip, flush, and hostile
  write cases.
- `tests/hv/aarch64/guest_vblk*.c` — the C guest gains a write+flush+read
  path (a new guest or an extended one), plus `hvtest.c`'s
  `el2-virtq-device` inline device modelling the write side.
- `docs/kernel-services/virtualization/design.md`,
  `docs/kernel-services/virtualization/testing.md`, `README.md` — the
  writable-root Status entry and the new test rows.

## New APIs

No new system calls: writes reach guest memory and the disk through the
same `cosmo_vm_mem_*` and the owner's own file, and the interrupt through
the `cosmo_vm_raise_spi`/`_lower_spi` the read unit added.

```c
/* userland/system/vblk.h */
#define VIRTIO_BLK_T_FLUSH   4u
#define VIRTIO_BLK_F_FLUSH   9u          /* the device buffers; T_FLUSH makes durable */

struct vblk_io {
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    int (*disk_read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    int (*disk_write)(void *ctx, uint64_t off, const void *buf, uint32_t len);  /* NULL if RO */
    int (*disk_flush)(void *ctx);                                               /* NULL if RO */
    void *ctx;
    uint64_t capacity_sectors;
    uint64_t max_bytes_per_call;
};
```

`vmctl` gains `--disk-rw FILE` (mutually exclusive with `--disk`), opening
the file `O_RDWR` and presenting a writable device.

## Migration plan

1. **The device-side write path, on the host first.** Add `disk_write`/
   `disk_flush` to `vblk_io`; branch `serve_one` on the request type; bound
   `disk_write` to capacity. Prove it with `test_vblk_dev`: a write
   round-trips through an in-memory disk, a flush is observed, and hostile
   write rings (a write buffer marked writable, or a data buffer outside
   guest RAM) are refused with no out-of-bounds access, while a write past
   the disk completes with `VIRTIO_BLK_S_IOERR` (a full request, not a
   refusal, exactly as a read past the disk does) — each proved by
   reintroducing the bug.
2. **The transport in `vmctl`.** `--disk-rw`, `O_RDWR`, the `writable`
   flag, `vio_disk_write`/`vio_disk_flush`, and the feature-register
   change so a writable disk advertises `F_FLUSH` and not `F_RO`. A
   read-only `--disk` is byte-for-byte unchanged.
3. **The guest test.** A C guest that negotiates the writable device,
   writes a sector, flushes, reads it back, and reports the bytes;
   `el2-virtq-device-rw` in the harness with a test-backed writable disk.
4. **The Linux demonstration.** Documented and reproducible under
   `QEMU_MEM=2G` with a writable root image: mount read-write, write a
   file, sync, power off, and confirm the change in the backing file.
   Not a CI gate, for the same reason the read path's Linux boot is not.
5. **Docs and the Status entry**, and the full verification chain.

## Tests

- `test_vblk_dev` (host): a `T_OUT` request reads guest data and writes
  the in-memory disk; a following `T_IN` of the same sector returns the
  written bytes. A `T_FLUSH` calls `disk_flush`. Hostile: a write whose
  data buffer is marked `VQ_DESC_F_WRITE` (wrong direction) is refused; a
  write past the disk is `IOERR`; a write buffer outside guest RAM is
  refused with no out-of-bounds read; and — unchanged — a read-only
  device (no `disk_write`) still refuses `T_OUT` as `UNSUPP`.
- `el2-virtq-device-rw` (kernel): a real guest driver negotiates the
  writable device, writes a known sector, flushes, reads it back, and the
  test requires the bytes to match — so a device that dropped the write
  but reported success would not pass.
- The read-only path's tests are unchanged and must stay green: a
  `--disk` device still advertises `F_RO`, refuses writes, and the
  diskless boot is untouched.

## Benchmarks

Not a performance unit, but a writable disk invites a throughput
question the read path did not: a guest that writes a large file and
flushes measures the owner's write+fsync cost. A micro-benchmark (write N
sectors, one flush, time it) records the order of magnitude and confirms
the per-notification work ceiling does not serialise a legitimate write
batch into one-request-per-kick. No absolute target; the number exists so
a later regression is visible.

## Risks

- **Data loss through an unintended write.** A guest writing a file the
  owner meant to keep. Mitigated by making read-write opt-in (`--disk-rw`)
  and never the default; the owner states intent.
- **A false durability promise.** A writable device that does not honour
  flush lets a guest believe a write is safe when it is in a cache. The
  design ties writable to advertising `F_FLUSH` and honouring it with
  `fsync`; the two are not separable.
- **A hostile write escaping the disk.** A `T_OUT` whose disk offset or
  length would leave the file, or grow it. `disk_write` is bounded to
  `capacity_sectors` exactly as `disk_read` is; a write past the end is an
  I/O error, and the file never grows.
- **Direction confusion.** A device that reads a write buffer as if it
  were a read buffer (or vice versa) either faults or moves data the
  wrong way. The direction branch enforces the descriptor writability
  each request type requires, and the guest test writes-then-reads so a
  wrong direction fails the byte compare, not merely the flags.
- **Review churn on the same surface.** The read path took seven review
  rounds on partial-failure and notification edges. The write path
  reuses that now-hardened machinery, so the new surface is small (the
  direction branch and the `disk_write` bound); the risk is treating the
  reused code as automatically correct rather than re-checking that a
  write faults and publishes exactly as a read does.

## Alternatives considered

- **Write-through: fsync every write, do not advertise flush.** Correct
  durability with no flush feature, but every write pays a synchronous
  `fsync`, which a journalling guest — already issuing its own flushes at
  commit — turns into a double cost. Rejected: `F_FLUSH` lets the guest's
  filesystem decide when durability is worth the wait, which is the whole
  reason the feature exists.
- **Always open the disk read-write.** Simpler (one `--disk`), but it
  turns every attached disk into something a guest can corrupt without
  the owner asking. Rejected for the opt-in split.
- **A copy-on-write overlay so the base image is never written.** Protects
  a golden image, and is genuinely useful, but it is a file-management
  policy the owner can build on top of `--disk-rw` (a scratch overlay
  file), not a property the device needs. Deferred to keep the device the
  minimal writable disk.
- **An in-kernel virtio-blk with writes.** Rejected for the same reason
  the read unit rejected it: the backing store is a file, and the kernel
  has no business holding a guest's disk. Writes do not change that — if
  anything they strengthen it, because a write is exactly the operation
  that must stay in the owner that owns the file.
