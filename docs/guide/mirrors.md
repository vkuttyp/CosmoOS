# Mirrors

A cosmofs pool can keep its blocks on two disks at once, so the system
keeps running when one of them fails or is pulled. `cosmo-install`
makes the installed root such a mirror. `cosmo-pool` says what the pool
is doing, replaces a disk, and adds a spare.

The steps below are the ones `make test-mirror` runs (CI job `mirror`,
on both architectures), and the output is from one of its x86-64 runs.
The test installs onto two disks, pulls one out of the running machine
(QEMU's `device_del`), adds a new one (`device_add`), and reboots from
the new pair. The guide's own runner cannot remove a disk from a running
system, so the steps are shown here as text.

## Installing onto two disks

From the live image, with two blank disks attached, name both. The
second must be at least as big as the first:

```text
cosmo$ cosmo-install vdc --mirror vdd
cosmo-install: vdc: esp sectors 2048-133119, root sectors 133120-524254
cosmo-install: vdd: esp sectors 2048-133119, root sectors 133120-589790
cosmo-install: copied 76 files in 14 directories
cosmo-install: installed on vdc: esp vdc1, root vdc2, partuuid 73de24d1-7820-4c2a-8ab7-59e099345149
cosmo-install: the root is mirrored on vdc2 and vdd2 (vdd's root partition, partuuid fe68c29e-752f-4ac3-ac21-12176260e757)
```

Each disk gets the same layout and its own EFI System Partition, so
either can boot the system. The root is one pool kept as two copies,
one on each disk's second partition, each the size of the first disk's.

## What the pool is doing

On the installed system (here the first disk is `ahci0p0`, the second
`vda`):

```text
cosmo$ cosmo-pool status /
pool / (cosmo-root): ONLINE, format 11, generation 17
  member 0 (48891 blocks):
    copy 0  ahci0p0p2  ONLINE
    copy 1  vda2       ONLINE
```

A copy is in one of five states:

- `ONLINE`: read and written.
- `MISSING`: its disk is gone.
- `OFFLINE`: taken out with `cosmo-pool offline`.
- `FAULTED`: it failed more than `fs.fault_errors` times (16 unless
  set).
- `RESILVERING`: being rebuilt from the others. It is written to, and
  not yet read from.

The pool is `DEGRADED` while any copy is not `ONLINE`.
`cosmo-pool status` exits 0 for a whole pool and 1 for a degraded one.

## A disk goes

When a disk fails or is pulled, the system keeps running on the other
copy. The kernel says so once, on the console:

```text
pci: pci:00:06.0: attention button: removing the device behind it
pci: pci:01:00.0 1af4:1042 removed
blk: vda2 removed
blk: vda removed
pool: cosmo-root: member 0 copy 1 (vda2) MISSING (-19): the member is degraded
```

From then on the pool says it too:

```text
cosmo$ cosmo-pool status /
pool / (cosmo-root): DEGRADED, format 11, generation 39
  member 0 (48891 blocks):
    copy 0  ahci0p0p2  ONLINE
    copy 1  vda2       MISSING     since generation 39, about 189 blocks to resilver errors: 0 read, 1 write, 0 checksum
cosmo$ grep ' / ' /proc/mounts
/dev/ahci0p0p2 / cosmofs rw,degraded 0 0
```

The pool records what the missing copy missed (`about 189 blocks to
resilver`). A disk that was pulled and comes back is caught up from that
record rather than rebuilt.

The first commit that has a copy to record raises the pool to format 13:

```text
cosmofs: format version 11 -> 13 (copy state): member 0 is degraded; a kernel that reads format 12 at most (CosmoOS 1.0.10 and older) cannot mount this pool
```

From then on, a CosmoOS older than 1.0.11 cannot mount the pool.

A system that boots with a copy missing still boots, and init says so:

```text
init: the root pool is degraded: a copy is missing or being resilvered (cosmo-pool status)
cosmo$ cosmo-pool status /
pool / (cosmo-root): DEGRADED, format 13, generation 97
  member 0 (48891 blocks):
    copy 0  ahci0p0p2  ONLINE
    copy 1  -          MISSING     needs a whole resilver
```

When that disk is attached again, it is found after the switch to the
disk root and resilvered whole. The pool cannot tell from its record
what a disk that was missing when the pool was mounted missed.

## Replacing it

A new disk, or a partition of one, replaces the missing copy. It must
be at least as big as the pool's member. A disk added to a running
machine is found, and its partitions read, without being asked:

```text
pci: pci:00:07.0: a device was added
blk: vda: 655360 sectors of 512 bytes (320 MiB)
pci: pci:00:07.0: 1 function(s) added behind it
part: vda registered after boot: 0 partition(s)
cosmo$ cosmo-pool replace 0:1 vda
cosmofs: cosmo-root: replacing member 0 copy 1 with vda
cosmo-pool: replace 0:1 vda: done
```

`0:1` is member 0, copy 1, as `status` numbers them. A copy whose disk
is still there can be named by its device instead (`vda2`).

The new copy is resilvered while the system runs: every block the pool
uses is copied to it, and every write meanwhile goes to it too. While
the resilver runs, the copy's line reads like this:

```text
    copy 1  vda        RESILVERING 41% (block 20100 of 48891, about 2100 to copy, 8192 blocks/s bound)
```

When it is done, the console says so and the copy is `ONLINE`:

```text
pool: cosmo-root: member 0 copy 1 (vda) ONLINE: resilvered; the member is whole
cosmofs: cosmo-root: vda resilvered as member 0 copy 1: 3645 blocks copied in 662 ms
```

A resilver goes at most `fs.resilver_rate` blocks a second, 8192
(32 MiB/s) unless set, for example with `sysctl fs.resilver_rate=4096`.
That keeps the system responsive. A reboot during a resilver resumes
from where it stopped.

When the resilver is done, a scrub reads every block on both copies:

```text
cosmo$ fsctl list
...
cosmo$ fsctl scrub 7
mount 7: 3638 blocks read, 356 inodes, 0 bad copies, 0 repaired, 0 unrecoverable, 56 skipped (1277 ms, bound 4096 blocks/s)
clean
```

`skipped` counts blocks still held by the open transaction; the next
scrub reads them.

The other commands:

- Replacing a copy that is still `ONLINE` keeps it serving until the new
  one is resilvered, then detaches it.
- `cosmo-pool attach ahci0p0p2 vdb` makes a single-disk pool a mirror
  the same way.
- `cosmo-pool detach vdb` goes back to one disk.
- `offline` takes a copy out, and `online` brings it back, resilvering
  what it missed meanwhile.

## A spare

A spare is a disk the pool holds ready. When a copy fails or its disk is
removed, the spare takes its place by itself and is resilvered:

```text
cosmo$ cosmo-pool add-spare vdb
cosmo-pool: add-spare vdb: done
cosmo$ cosmo-pool status /
pool / (cosmo-root): ONLINE, format 13, generation 62
  member 0 (48891 blocks):
    copy 0  ahci0p0p2  ONLINE
    copy 1  vda        ONLINE
  spares:
    vdb        90112 blocks
...
pool: cosmo-root: member 0 copy 1 (vda) MISSING (-19): the member is degraded
cosmofs: cosmo-root: spare vdb takes over member 0 copy 1 (was vda, MISSING)
pool: cosmo-root: member 0 copy 1 (vdb) ONLINE: resilvered; the member is whole
```

Once resilvered, the spare is a member like any other. `remove-spare`
gives back a spare that has not been used.

## Upgrades reach both disks

`cosmo-upgrade` writes the new release to the ESP of the disk the
system booted from (`ahci0p0p1` here, the first disk's; the second is
`vda`), then makes every other disk's ESP that one, sector
for sector: an upgrade, a `--rollback` and a `--repair` each end with a
line per disk.

```text
cosmo-upgrade: the ESP on vda1 is ahci0p0p1's (44032 sectors written)
```

So after an upgrade either disk boots the new release by itself. A
disk that was missing during an upgrade is named instead, and the ESP on
it keeps the release it had:

```text
cosmo-upgrade: 1 copy(ies) of the root are missing: their disks' ESPs are not updated; once they are back, cosmo-upgrade --repair brings them up to this one
```

When it is back, every boot says so until it is repaired (the pool
resilvers the root's copy by itself; the ESP is outside the pool):

```text
cosmo-upgrade: the ESP on vda1 is not ahci0p0p1's (its set 4360a4d314f4, the booted 05822dd894bd): run cosmo-upgrade --repair to bring it up to date
cosmo$ cosmo-upgrade --repair
cosmo-upgrade: the root and the boot set are the pairing \cosmo\be intends: nothing to repair
cosmo-upgrade: the ESP on vda1 is ahci0p0p1's (43776 sectors written)
```

`--repair` copies from the ESP the system booted from, so run it on the
disk that has the release you want. A copy that is a whole disk (one
`cosmo-pool attach` or `replace` gave a whole disk) has no ESP, and that
disk does not boot by itself; `cosmo-upgrade` says so.

## What it does not do

- A mirror is two copies, or up to four with `attach`. There is no
  parity (RAID-Z), and nothing moves blocks between members.
- Hot-removal and hot-add of disks work on PCIe (virtio and NVMe). A
  USB disk under a pool is not handled.
