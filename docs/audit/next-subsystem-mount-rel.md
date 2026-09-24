# NEXT SUBSYSTEM — mount and unmount name what the caller's path names

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **The guard test's second path is `d/..`, not `.`.** §2 had the
>    second unmount use `.` from the mount's root. The mutation making
>    `vfs_umount_at` ignore its start **passed** that test: `.` resolved
>    from the global root reaches the root filesystem, which is refused
>    `-EBUSY` too ("the root filesystem stays") -- the guard's errno from
>    the wrong check. The test now puts a directory `d` in the mount and
>    unmounts `d/..` from its root: it names that root only from there,
>    and from anywhere else it is `-ENOENT`. With it, the mutation fails
>    the test on `second.rc == -EBUSY`.
> 2. **`init`'s `umount(".")` from inside the mount proves nothing on
>    its own**, for the same reason: before the fix it was `-EBUSY` as
>    the root filesystem, after it as a busy mount. It stays, commented
>    as such; the claim is carried by the inode checks around it (which
>    directory the mount covers, and that the relative unmount uncovers
>    it).
> 3. **An `irq-route` failure** (`hits >= 5`) in one mutation boot -- a
>    mutation of `vfs_umount_at`, which no interrupt path calls -- is that
>    test's known load-sensitive count; recorded in
>    `docs/testing/flakes.md` as its second sighting.
>
> **The mutations**, each applied alone on x86-64, each boot confirmed
> booted:
>
> | # | mutation | what failed |
> | --- | --- | --- |
> | 1 | the guard removed | `vfs-umount-once`: the second unmount waited in the drain (`second_answered`, after the 2 s bound) |
> | 2 | `sys_mount` resolving from the root again | `init`: the mount covered `/mrel`, not `/tmp/mrel`, and every check after it |
> | 3 | `sys_umount` resolving from the root again | `init`: `umount("mrel")` failed, `/tmp/mrel` stayed covered |
> | 4 | `vfs_umount_at` ignoring its start | **passed `vfs-umount-once` at first** (item 1); with `d/..`, `second.rc == -EBUSY` fails (`-ENOENT`); `init` as row 3 |

> Constitution §68 report. Found while taking up the deferred-work
> inventory's row (section 3) "`vfs_umount2`'s one-unmount-at-a-time
> guard is unfired by any test ... reachable by a relative path resolved
> from inside the mount". The row's premise is false: **no relative
> mount or unmount target is resolved from inside anything.** `mount`
> and `umount` resolve every target from the caller's root, ignoring
> the working directory. This report fixes that, which makes the row's
> path real, and then fires the guard through it.

## Problem

Every native path call resolves a relative path from the caller's
working directory (P27): `open`, `stat`, `lstat`, `mkdir`, `mknod`,
`unlink`, `rmdir`, `rename`, `symlink`, `readlink`, `chdir`, and spawn's
program, `cwd` and `root`. Two do not. `sys_mount` and `sys_umount`
(kernel/syscall/native.c) copy the target and hand it to `vfs_mount`
and `vfs_umount2` (kernel-services/vfs/vfs.c), which look it up with
`vfs_lookup(NULL, path)` -- a NULL start, which the walk treats as the
caller's root. So a relative target is resolved as if it began with
`/`.

The system tools pass their argument straight through
(`userland/system/mount.c`, `umount.c`), so this is where a user meets
it: in `/tmp`, `mount none m ramfs` mounts on `/m` if there is one --
covering a directory the user did not name -- and fails `ENOENT` if
there is not, with `/tmp/m` in front of them. `umount m` the same way.

The Linux personality has neither call (`mount` and `umount2` are not in
its table), so this is the native door only.

### Measured

`tools/mount-rel-probe.py` injects into `init`'s self-test, after its
own mount checks: a process in `/tmp`, with both `/tmp/mrel` and
`/mrel` present, mounts a ramfs on `mrel`, then unmounts `mrel`, then
removes `/mrel` and mounts on `mrel` again. Which directory a mount
covers is read from the inode each name reports before and after. The
same on both architectures, debug, both boots passing with the probe
applied:

```
MRPROBE mount rc=0 covers_tmp_mrel=0 covers_root_mrel=1
MRPROBE umount rc=0 tmp_mrel_mounted=0 root_mrel_mounted=0
MRPROBE absent mount rc=-2
```

The first line is the defect: the caller asked for the `mrel` beside it
and the mount covered the one under `/`. The second is its mirror: the
unmount of `mrel` removed that same mount, the wrong one by the
caller's reading, so the pair looks consistent to anyone who only
checks return codes. The third: with `/mrel` gone, `ENOENT`.

**The inventory row, re-read.** It says the guard "is reachable by a
relative path resolved from inside the mount, which does not traverse
the mountpoint". Today `umount(".")` from inside a mount resolves `.`
from the root, reaches the root filesystem, and is refused `-EBUSY` by
the "the root filesystem stays" check -- never the guard. The path the
row names exists only once unmount honours the working directory.

### Why it matters

- **A privileged call acting on a directory the caller did not name.**
  Mounting covers a directory's contents until the unmount; covering the
  wrong one hides files from every process on the machine. It needs
  privilege, which is why nobody has been hurt by it, and why the
  mistake is expensive when it happens: the caller is the one process
  trusted to get it right.
- **It is the one exception to P27**, which says every path call
  resolves from the working directory. An exception nobody wrote down
  is a trap for the next reader of both.
- **The guard it hides is untested.** The one-unmount-at-a-time check
  (`mnt->unmounting`) protects the namespace links from two unmounts
  interleaving across the drain, and nothing has ever run it.

## Current implementation

**The calls.** `sys_mount(source, target, fstype, flags)`: privileged,
flags checked, `get_path(target)`, the block device found by name,
`vfs_mount(target, fstype, bd, flags)`. `sys_umount(target, flags)`:
privileged, flags checked, `vfs_umount2(target, flags)`.

**The VFS.** `vfs_mount(path, ...)` looks `path` up from NULL, requires
a directory, mounts, links the mount into the caller's namespace.
`vfs_umount2(path, flags)` looks `path` up from NULL, requires the
result to be a mount's root (`-EINVAL`) and not the root filesystem
(`-EBUSY`), then under `g_mounts_lock` and the mountpoint's lock:
drops only this namespace's view if others still see the mount; **else
refuses `-EBUSY` if `mnt->unmounting` is already set** (the guard);
else sets `unmounting` (so `follow_mount` turns new walkers away),
drains the maintenance passes with `g_mounts_lock` dropped, counts the
namespaces again, checks references and tears down.

**Why the guard cannot fire today.** A second unmount must find the
mount while the first is in its drain. By absolute path it cannot:
resolving the path crosses the mountpoint, and `follow_mount` refuses
a mount that is unmounting (`-EBUSY` while resolving). By relative path
from inside the mount it would not cross the mountpoint -- but relative
paths do not start inside anything, because of this report's defect.

## Design

### 1. Mount and unmount take a start

`vfs_mount_at(start, path, fsname, bdev, flags)` and
`vfs_umount_at(start, path, flags)` resolve `path` from `start` exactly
as every other VFS entry point does (NULL: the caller's root). The
existing `vfs_mount(path, ...)` and `vfs_umount2(path, flags)` become
their NULL-start wrappers, so the kernel's own callers -- boot's
`/proc`, the device and fault tests, all with absolute paths -- are
unchanged.

`sys_mount` and `sys_umount` pass the caller's working directory,
referenced for the call (`process_cwd_get()`, P29), and put it after.
That is the whole fix at the door. Confinement is unchanged: a relative
walk from the cwd is bounded by the caller's root exactly as an
absolute one is.

### 2. The guard, fired

With §1, a process whose working directory is a mount's root reaches
that mount by `.` without crossing its mountpoint, which is the path the
inventory row describes. A kernel test drives it deterministically:

1. Mount a ramfs at `/tmp/um`; look up its root (a reference the test
   holds) and acquire a maintenance pass on it (`vfs_mount_acquire` by
   its id), so an unmount will wait in its drain.
2. A kernel thread unmounts `/tmp/um` by absolute path. It sets
   `unmounting` and waits for the pass. The test waits until it reads
   `unmounting` set (under the mountpoint's lock) -- the order is
   enforced by that observation, not a sleep.
3. The test unmounts `vfs_umount_at(root, ".")` -- from inside (**as
   built, `d/..`**: `.` from the wrong start also answers `-EBUSY`;
   banner item 1) -- and
   requires `-EBUSY` **while the pass is still held**: the guard answers
   at once. It runs in a second thread with a bound, because without
   the guard it would block in the same drain, and the test must be
   able to say so rather than hang.
4. The test drops its root reference and releases the pass; the first
   unmount completes `0` and the mount count is back where it started.

The guard's mutation -- the check removed -- makes step 3 block in the
drain instead of answering, and then two unmounts run the teardown of
one mount.

### 3. The invariant

P27 already says "every path system call" resolves from the working
directory; it was not true of these two. It gains the sentence that
names them and the test that holds them to it. No new invariant.

### 4. The §70 gate

**Correctness.** Two calls brought under the rule every other path call
follows; the guard they uncover gets its first test.

**Concurrency.** Unchanged: the cwd is referenced for the call like
every other door's, and the unmount's locking is untouched. The guard
test's ordering is enforced by observing `unmounting`, not by timing.

**Ownership and lifetime.** The door takes and puts one cwd reference
per call. A mount holds no reference to the walk's start.

**Security.** Both calls stay privileged. A relative target is bounded
by the caller's root as an absolute one is, so a confined privileged
caller can mount only where it could already name.

**Failure.** Unchanged, except that a relative target now fails for the
reasons its own directory gives (`ENOENT` for a name that is not there,
`ENOTDIR`) rather than the root's.

**Performance.** One reference taken and dropped per mount call.

## Affected files

| file | change |
| --- | --- |
| kernel-services/vfs/vfs.c | `vfs_mount_at` and `vfs_umount_at`; `vfs_mount` and `vfs_umount2` wrap them with a NULL start |
| kernel/include/kernel/vfs.h | the two declarations |
| kernel/syscall/native.c | `sys_mount` and `sys_umount` pass the referenced cwd |
| kernel-services/vfs/vfstest.c | `vfs-umount-once` (the guard) |
| userland/init/init.c | the relative mount and unmount checks |
| docs | P27; the VFS API (`vfs_mount_at`, `vfs_umount_at`) and syscall table rows; testing docs; the inventory row (marked taken up by this report; the implementation rewrites it to what is built and strikes the superseded text); README Status |

## APIs

### New

```c
/* vfs_mount / vfs_umount2, resolving `path` from `start` as every other
 * entry point does (NULL: the caller's root). */
int vfs_mount_at(struct vnode *start, const char *path, const char *fsname, struct blkdev *bdev, unsigned flags);
int vfs_umount_at(struct vnode *start, const char *path, unsigned flags);
```

### Existing, relied on

`vfs_mount`, `vfs_umount2`, `vfs_umount` (now wrappers, same behaviour);
`process_cwd_get`; `vfs_mount_acquire` / `vfs_mount_release` for the
test's held pass.

## Migration plan

One PR: the two `_at` functions and the wrappers (no behaviour change
for the kernel's callers), then the two syscalls, then the tests and
documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `init` mount checks (extended) | from `/tmp`, with `/tmp/mrel` and `/mrel` both present: `mount("none", "mrel", "ramfs")` covers `/tmp/mrel` (its inode changes) and not `/mrel`; `umount("mrel")` uncovers it; with `/mrel` absent the relative mount still succeeds; from inside the mount (`chdir` into it), `umount(".")` is `-EBUSY` -- the caller's own cwd holds the mount -- and after `chdir("..")`, `umount("mrel")` is `0` | the syscalls passing NULL again |
| `vfs-umount-once` (new, vfstest) | §2: a second unmount from inside, while the first waits in its drain, is `-EBUSY` at once, with the pass still held; the first then completes `0`; the mount count returns | the guard removed |

Each mutation is run alone, with the runner confirming each boot booted.

## Benchmarks

None: one reference per mount call.

## Risks

- **Scripts that relied on a relative target meaning absolute.** Any
  caller that wrote `mount none mnt ramfs` meaning `/mnt` now mounts
  beside itself. Nothing in the tree does: the one script that mounts,
  `userland/etc/rc.test` (`mount nvme0n1 /tmp/snapmnt cosmofs`, and its
  `umount`), and every `cosmo_mount` in `init` use absolute targets,
  which are unchanged.
- **The guard test holds a pass.** A bug in the test's ordering would
  leave the first unmount waiting forever; the thread running the second
  unmount is bounded, and the test releases the pass on every exit.

## Alternatives considered

- **Make `sys_mount` absolutise the path** (normalise against the cwd's
  name, then call the old functions): a second meaning for a relative
  path, lexical, which is the defect the cwd-name unit just removed
  from `chdir` (P32). Resolving from the vnode is the rule every other
  call uses.
- **Refuse relative targets (`-EINVAL`)**: coherent, and a refusal of
  what every other path call accepts, for no reason the call has.
- **Test the guard without the fix**, by calling `vfs_umount2` from a
  kernel test with some other way into the mount: the row asks for the
  path a program can take, and that path does not exist until §1.
