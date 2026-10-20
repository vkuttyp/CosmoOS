# Snapshots and rollback

The installed system's root is a cosmofs filesystem, and cosmofs keeps
**snapshots**: the whole tree as it was at one moment, kept while the
live tree moves on, at the cost of only the blocks that have changed
since. `cosmo-snap` takes them, lists them with what each one keeps,
shows what changed, **rolls the system back** to one -- for example
after an upgrade or a change to `/etc` that left the system broken --
and makes writable **clones** of them.

The steps below are the ones `make test-snapshot` runs on an installed
system (the CI job `install`, on both architectures), with the output it
requires. The guide's own runner cannot do this -- a rollback reboots --
so these blocks are shown as text.

## Taking a snapshot

```text
cosmo$ cosmo-snap create base
cosmo-snap: created base (id 1, uuid 61331b51-50c4-49fb-917c-508ee9c374b0, generation 214)
```

A snapshot's name is up to 31 characters, without `/`. Its id and uuid
are its own for good: a deleted snapshot's id is never handed out again.

The first snapshot taken on a filesystem raises it from format 11 to
format 12, the format that gives snapshots this identity, and the kernel
says so:

```text
[ INFO] cosmofs: format version 11 -> 12 (snapshot identity, 0 snapshot(s)); a kernel that reads format 11 at most (v1.0.7 and older) cannot mount this pool
```

From then on a release older than the one that raised it cannot mount
the root, so `cosmo-upgrade` will not switch to one ([Upgrading](upgrading.md)).

## Looking at snapshots

```text
cosmo$ cosmo-snap list
cosmo-snap: /: format 12
cosmo-snap: /: 176848 KiB free of 195564 KiB
NAME                    ID CREATED                   REFER     UNIQUE UUID                                 FLAGS
base                     1 2026-10-20T08:50:55Z     13568K        64K 61331b51-50c4-49fb-917c-508ee9c374b0 -
```

`list` leaves out the snapshots programs take for you -- `pkg`'s
transactions (`pkg-*`, [Packages](packages.md)) and `cosmo-upgrade`'s
boot environments (`be-*`, [Upgrading](upgrading.md)) -- and says how
many it left out; `cosmo-snap list --all` shows them too.

`REFER` is everything the snapshot's tree holds; `UNIQUE` is what only
it holds -- what deleting it would give back. Both come from walking every
tree, so `list` takes longer the more snapshots and files there are.

A snapshot is a read-only directory under `/.snapshots`, which `ls /`
does not show:

```text
cosmo$ cat /.snapshots/base/snapdemo/edit.txt
one
```

`diff` names what changed between two snapshots, or between one and the
live tree (`-`): `+` added, `-` removed, `M` changed.

```text
cosmo$ cosmo-snap diff base -
M /root/.ash_history
- /snapdemo/drop.txt
M /snapdemo/edit.txt
+ /snapdemo/new
+ /snapdemo/new/n.txt
cosmo-snap: 5 change(s) from base to -
```

`cosmo-snap delete base` deletes one. It is refused while anything
inside it is open, and while a rollback keeps it (below).

## Rolling back

```text
cosmo$ cosmo-snap rollback good
cosmo-snap: rollback to good pending; the tree it replaces is kept as snapshot pre-rollback-1792505455
cosmo-snap: rebooting to apply it
init: restart requested
```

A rollback does not change the running system. It takes a snapshot of
the tree as it is now, `pre-rollback-<seconds>`, and records that the
next boot should run `good` instead; then it reboots. That boot makes
`good` the live tree before anything else runs:

```text
[ INFO] cosmofs: vda2: rolled back: the live tree is snapshot 'good' (generation 220); the tree before it is snapshot 'pre-rollback-1792505455', held until this boot completes; 14 block(s) released, 3 deadlist entr(ies) live again
...
init: rc exited with status 0
init: boot complete: released the rollback hold on /
```

The tree you rolled back from is kept as `pre-rollback-<seconds>`. If the
rolled-back system is not what you wanted, roll back to that one. It
cannot be deleted until a boot after the rollback has completed (`/etc/rc`
ran to the end), so a rollback to a tree that does not boot can always be
undone from the live image. Until then it is also the only snapshot you
can roll back to: a rollback to any other is refused, because it would
let go of the one way back.

What you write after `cosmo-snap rollback` -- with `--no-reboot`, or on
another filesystem before its next mount -- is in neither snapshot: the
switch replaces the live tree with the target, and those writes go.

`cosmo-snap rollback --no-reboot good` records the rollback and leaves
the reboot to you; `cosmo-snap rollback --cancel` withdraws a pending
one (its safety snapshot stays). `cosmo-snap -m /mnt/data rollback good`
rolls back another cosmofs filesystem, at its next mount: unmount it and
mount it again.

## Clones

A clone is a snapshot you can write to: it starts as the snapshot's tree
and shares its blocks, and what you change in it is its own. It lives
under `/.snapshots` too:

```text
cosmo$ cosmo-snap clone cl-origin work
cosmo-snap: clone work of cl-origin (id 7) is at /.snapshots/work
cosmo$ echo changed > /.snapshots/work/cl/f
cosmo$ cat /cl/f /.snapshots/cl-origin/cl/f /.snapshots/work/cl/f
orig
orig
changed
```

`cosmo-snap list` marks it `clone-of:<id>`. The snapshot it came from
cannot be deleted while the clone exists; `cosmo-snap delete work`
deletes the clone and gives back what only it held. A clone cannot be
rolled back to, cloned, or have a file renamed into it from another tree.

## What happens if the machine stops

At every moment the disk names one live tree and at most one pending
rollback, so a power cut leaves something consistent: before the
rollback's request reached the disk, nothing changed; after it, the next
boot rolls back; during that boot's switch, the boot after it does the
switch again from the start; after it, the rolled-back system. `make
test-snapshot` stops the machine at each of those points and checks the
filesystem with `fsctl check` after each.
