# Checking and repairing

The installed system's root, and any other cosmofs pool, can be checked
two ways. The **check** asks whether the structures add up: whether the
blocks the files name are the blocks the allocator holds, whether every
inode has a name and every name an inode, whether the totals are right.
The **scrub** asks whether every block is still what was written: it
reads each one on every copy and compares it with its checksum, and on a
mirror puts a bad copy right from a good one.

The root is checked on every boot, before it is mounted. A pool that is
not mounted is checked with `fsck.cosmofs`; a mounted one with `fsctl`.

The steps below are the ones `make test-fsck` runs (the CI job `fsck`, on
both architectures), with the output it requires. They need a damaged
disk, which the guide's own runner cannot make, so they are shown as
text.

## At boot

Every boot of an installed system prints one line about its root, just
before init switches to it:

```text
cosmofs: root checked, 0 findings, 0 repaired
init: switched to the disk root ... (root=boot-disk)
```

What the check finds decides what the boot does. Bookkeeping -- blocks
allocated that nothing uses (`leaked`), a wrong total (`counter`), a
snapshot's identity, a stale rollback record or hold -- is repaired
without asking. What a crash leaves -- an inode no name reaches
(`orphan`), a wrong link count (`nlink`) -- is repaired too, and each
inode is named:

```text
class orphan count 1 repaired 1 logged
finding orphan inode 57 repaired
cosmofs: root: orphan inode 57 repaired
cosmofs: root checked, 1 findings, 1 repaired
```

Anything else is a question about which of two things is real -- two
files claiming one block, a name repeated in a directory, a chain of
blocks that loops -- and a boot does not answer it. It refuses the root,
names the class, and leaves you at the live system's shell with the pool
unmounted:

```text
class cross-linked count 1 repaired 0 refuse
finding cross-linked block 0:21877 left
cosmofs: root refused: cross-linked; vda2 is not mounted. From this shell: fsck.cosmofs --repair vda2
init: the root vda2 is not mounted; the live root stays
```

The kernel command line's `fsck=` changes this: `fsck=check` reports and
repairs nothing (the root still is not mounted when something is
refused), and `fsck=skip` does not check at all.

## A pool that is not mounted

From the live shell, `fsck.cosmofs --check` checks without writing a
byte, and `fsck.cosmofs --repair` repairs what has one right answer:

```text
cosmo$ fsck.cosmofs --repair vda2
fsck.cosmofs: vda2: format 12, generation 61, ...
class cross-linked count 1 repaired 0 refuse
finding cross-linked block 0:21877 left
class leaked count 1 repaired 1 unattended
finding leaked block 0:9874 repaired
fsck.cosmofs: vda2: 2 finding(s), 1 repaired, findings left
cosmo$ fsck.cosmofs --check vda2
fsck.cosmofs: vda2: format 12, generation 62, ...
class cross-linked count 1 repaired 0 refuse
finding cross-linked block 0:21877 left
fsck.cosmofs: vda2: 1 finding(s), 0 repaired, findings left
```

Its exit status says what happened: 0 nothing was found, 1 everything
found was repaired, 4 something is left, 8 the check could not run (the
device is mounted, for example). What `--repair` leaves needs a person:
which of two files keeps a block is not a question the filesystem can
answer. `--repair` writes only when it repairs something; then it
also applies a rollback waiting for the next mount ([Snapshots and
rollback](snapshots.md)), as any writable mount does.

## A mounted pool

`fsctl list` names each mounted filesystem by an id; `fsctl check ID`
checks one while it is in use, and `fsctl check ID --repair` repairs the
same classes a boot repairs. The output and the exit status are
`fsck.cosmofs`'s.

## Scrub and mirrors

`mkfs.cosmofs --mirror` makes a pool kept as two copies, one on each of
two disks. A block whose copy on one disk has rotted is read from the
other; a scrub finds the bad copies before a read needs them:

```text
cosmo$ mkfs.cosmofs --mirror vdb vdc
mkfs.cosmofs: made cosmofs mirrored on vdb and vdc
cosmo$ fsctl scrub 3
mount 3: 98 blocks read, 3 inodes, 16 bad copies, 0 repaired, 0 unrecoverable, 0 skipped (...)
cosmo$ fsctl scrub 3 --repair
mount 3: 98 blocks read, 3 inodes, 16 bad copies, 16 repaired, 0 unrecoverable, 0 skipped (...)
cosmo$ fsctl scrub 3
mount 3: 98 blocks read, 3 inodes, 0 bad copies, 0 repaired, 0 unrecoverable, 0 skipped (...)
clean
```

Without `--repair` the scrub only reports. It reads the live tree and
every snapshot's, each block once. A block that no copy holds correctly
is counted `unrecoverable`: the file it belongs to reads as an error at
that block, and the scrub's exit status is 4.

A scrub of a large pool reads at most `fs.scrub_rate` blocks a second
(4096, 16 MiB, unless set) and steps aside between tenths of a second, so
the programs using the pool keep going while it runs:

```text
cosmo$ sysctl fs.scrub_rate=1024
```
