# NEXT SUBSYSTEM — a working directory's name is the path the walk took

> Constitution §68 report. Takes up the deferred-work inventory's row
> (section 3) "`chdir` through a symbolic link publishes the link's
> spelling with the target's vnode", found in review of the dirfd unit
> (`docs/audit/next-subsystem-dirfd.md`), and a second defect the probe
> for it found on its first run: `chdir("..")` from a procfs process
> directory is `ENOENT`.

## Problem

A process's working directory is two things published together: a
referenced vnode, which every relative path resolves from, and a string,
which `getcwd` answers and which the next relative `chdir` normalises
against (P27). The two are meant to name the same directory. Through a
symbolic link they do not.

`chdir_inner` (kernel/process/process.c) computes the string by
`path_normalize(base, path)` -- a lexical operation on the old name and
the argument -- and the vnode by `vfs_lookup(cwd, path)`, a walk that
follows links. The normalisation does not know links exist. So after
`chdir("/tmp/clink")`, where `/tmp/clink` is a link to `/tmp/clp/deep`,
the vnode is `deep` and the string is `/tmp/clink`: the string still
reaches the right directory, but only by following the link again. The
next step is where it breaks. `chdir("..")` walks `..` from `deep` and
reaches `/tmp/clp`; the normalisation removes `clink` from the string
and publishes `/tmp`. From then on `getcwd` names a directory the
process is not in, and every later relative `chdir` normalises against
the wrong name. The process's *resolution* is still right -- relative
paths start from the vnode -- so what goes wrong is everything that uses
the name: `getcwd`, a shell's `pwd`, a child spawned with the inherited
name, a program that builds an absolute path from `getcwd` and opens it.

The same lexical naming is in three places, not one:

- `chdir`, at both doors (`process_chdir` serves the native call and
  `LX_chdir`);
- a spawn request's `cwd` (kernel/process/spawn.c:210), which
  normalises the requested name against the parent's and looks it up
  separately;
- a directory file's recorded name, which `fchdir` publishes (P31). The
  dirfd unit made this one safe by refusing: `file_set_dir_path` walks
  the lexical name again with no link allowed and records it only if it
  reaches the same vnode, so a directory opened through a link has no
  name and `fchdir` answers it `-ENOENT`. That is coherent, and it is a
  refusal of something Linux does.

The inventory row said the coherent answer "needs either a dentry cache
or parent pointers". This report finds a third way, cheaper than either
(Design §1).

### Measured

`tools/chdir-link-probe.py` injects the same scenario at both doors --
native in `init`'s self-test after its own working-directory checks,
Linux in `lxtest` -- with `/tmp/clp/deep` a directory and `/tmp/clink` a
link to it:

1. `chdir("/tmp/clink")`
2. `chdir("..")`

After each step it prints the published name and whether `stat(".")` and
`stat(name)` agree on the inode; after step 1, whether the name is
itself a directory (`lstat`) or reaches one only through a link. It also
tries `chdir("/proc/self")` then `chdir("..")` at the native door. The
same on both architectures, debug, both boots passing with the probe
applied:

```
CLPROBE native link cwd=/tmp/clink same=1 physical=0
CLPROBE native dotdot cwd=/tmp same=0
CLPROBE native proc-dotdot rc=-1 errno=2
CLPROBE linux link cwd=/tmp/clink same=1 physical=0
CLPROBE linux dotdot cwd=/tmp same=0
```

`same=0` is the defect: after `..`, `getcwd` answers `/tmp` while the
process stands in `/tmp/clp`. `physical=0` is why: the name after step 1
is the link's spelling.

**The second defect.** `proc-dotdot rc=-1 errno=2`: a process in
`/proc/<pid>` cannot `chdir("..")`. The VFS resolves `..` by asking the
filesystem (`step`, and `base->ops->lookup(base, "..")`), and
`proc_pid_lookup` answers only `status` and `limits`; every other name,
`..` included, is `-ENOENT`. Its readdir lists `..` (with `/proc`'s inode
number), so a listing names an entry a walk cannot take. Neither the
probe's first draft nor the inventory knew this; it was found checking
whether every directory type answers `..`, which the design first
considered relying on (Alternatives, first entry).

The probe's first draft ran the native case *before* `init`'s relative
`rmdir("cwdtest")` and ended with `chdir("/")`, so that `rmdir` failed
and the boot with it: a probe fault, not the kernel's. It now runs after
those checks, and both boots pass with it applied.

### Why it matters

- **A name that lies is worse than none.** `getcwd` answering a
  directory the process is not in is silently wrong: a program that
  saves `getcwd()` and later opens `name + "/file"` opens a file in
  another directory. The kernel is the only party that knows the right
  answer; userland cannot repair it.
- **It compounds.** Each relative `chdir` normalises against the
  published name, so one link crossed early makes every later `..` wrong
  in the same direction.
- **Linux answers the physical path.** A Linux program that calls
  `getcwd` after `chdir` through a link gets the resolved directory's
  path. Shells that want the logical name keep `$PWD` themselves; the
  kernel's name is physical. This system's `sh` keeps no `$PWD` and
  prints `getcwd`, so `cd /tmp/clink; cd ..; pwd` prints the wrong
  directory today.
- **`fchdir` refuses what it should do.** The dirfd unit's refusal was
  the right stopgap and is now the only reason `fchdir` fails on a
  directory opened through a link -- which is how `nftw`-style walkers
  and `find -L` reach directories.
- **procfs has a directory you cannot leave by `..`.** Small, and
  reachable by any process that looks at its own `/proc/self`.

## Current implementation

**The walk.** `resolve` (kernel-services/vfs/vfs.c) walks a path from a
`start` vnode, or from the caller's root for an absolute path. It calls
`walk_parent` for every component but the last; each intermediate
component is a `step` (a `lookup_in` with the mount crossing, or the
`.`/`..` rules, which stop at the process's root and leave a mount root
through its mountpoint). A component that is a link is replaced by the
link's target followed by the rest of the path (`walk_expand`); an
absolute target restarts at the caller's root, a relative one continues
from the directory the link was found in. `resolve` then handles the
last component the same way, following a link there with
`RESOLVE_FOLLOW`. **Nothing records which components were actually
traversed.** The walk knows at every step exactly which directory it is
in and by what name it got there; the knowledge is dropped.

**The names.** `chdir_inner`: one snapshot of the old cwd and its name,
`path_normalize(base, path)`, `vfs_lookup(cwd, path)`, then
`cwd_publish(vn, newpath)`. Spawn (spawn.c:200-222): the same pair on the
parent's snapshot. Directory files (P31): the opener normalises
`base + path` and calls `file_set_dir_path(f, abs)`, which walks `abs`
again from the root with `no_links` set and stores it only if the walk
reaches `f->vn`.

**procfs.** `/proc` is one mount: `proc_root_ops` (lookup of `self` and
of decimal pids, readdir) and `proc_pid_dir_ops` (lookup of `status` and
`limits`, readdir of `.`, `..`, `status`, `limits`). `self` is a
directory resolved to the caller at lookup, not a link.

## Design

### 1. The walk keeps the name it took

`struct walk` gains an optional **traversed name**: a buffer of
`VFS_PATH_MAX` the caller provides (`name`, NULL when not wanted). When
present, the walk maintains in it the absolute name, relative to the
caller's root, of the directory it is currently in:

- **Seeded** with the start's name for a relative walk (the caller
  supplies it), and `/` for an absolute one.
- A component that resolves to something **other than a link** is
  appended (`/` + the component), except `.`, which appends nothing, and
  `..`, which removes the last component (and at `/` stays at `/`).
- A **link** is never appended. Its expansion continues as the walk
  already does: an **absolute** target resets the name to `/` (the walk
  restarts at the caller's root), a **relative** one keeps the name it
  has -- the directory the link was found in, which is exactly where the
  walk continues.
- The **last component** in `resolve` follows the same rules: appended
  if it is not followed as a link, and for a followed link the name is
  reset or kept as above and the loop continues.

The rules are the walk's own rules, applied to a string. `..` removing
the last component is correct *because* the name has no links in it: it
is made only of components the walk entered as directories, so the
directory before the last component is the one `..` reaches -- including
at a mount root, where `..` leaves through the mountpoint, whose name is
the component removed. It stops at the process's root because the walk
does, and a name can never climb above it: an absolute link restarts at
that root, and `..` there stays.

Filesystem rules the walk does not know about are taken as the walk
takes them. A cosmofs `.snapshots/<name>/dir` is entered by name and so
named; its `..` rules (a snapshot's root goes up to `.snapshots`,
`.snapshots` to the live root) are the lexical ones, because
`.snapshots` is found only at the filesystem's root. procfs's `self`
is entered by name, so a name through it says `self` (Risks).

The result is **only as good as the seed**. A relative walk from a
directory whose name is stale -- an ancestor renamed since it was
published, P27's standing gap -- produces a name under the stale prefix.
This unit does not change that gap and does not widen it: every seed is
either `/`, the published cwd name, or a directory file's recorded name,
and after this unit all three are made by this rule.

Length: a traversed name longer than `VFS_PATH_MAX - 1` makes the walk
fail `-ENAMETOOLONG` when a name was asked for; a walk that asked for
none is unaffected. `chdir` already fails that way when the normalised
name is too long; spawn likewise; a directory file simply records no
name (fchdir then refuses it, as it does a base with none).

### 2. One entry point, three publishers

`vfs_lookup_named(start, startname, path, &vn, name, n)`: a `vfs_lookup`
that also returns the traversed name of what it reached. `startname` is
the name of `start` (ignored for an absolute path). A relative walk
whose start has no name (`startname` NULL or not absolute) fails
`-ENOENT` when a name is asked for -- the case of a directory file that
recorded none.

- **`chdir`**: `chdir_inner` becomes one `vfs_lookup_named(cwd, base,
  path)` on the snapshot and publishes the name it returns.
  `path_normalize` leaves `chdir`.
- **Spawn**: the same, on the parent's snapshot.
- **Directory files** (both doors' opens): `file_set_dir_path(f, start,
  startname, path)` does a named lookup of the same `path` from the same
  base and records the name if the lookup reaches `f->vn`. The check
  stays -- a rename between the open's walk and this one could make them
  reach different directories, and then no name is recorded, as now --
  but what it checks changes: not "does the lexical name, walked with no
  link, reach this directory" but "the physical name of what this path
  reaches". The `no_links` flag on the walk is removed; nothing else uses
  it.

The two doors' opens already hold what this needs (the base vnode and
its name from `at_base`, or the cwd snapshot at the native door) and
pass it instead of a pre-normalised string.

### 3. What `fchdir` then does

A directory opened through a link has a name -- the physical one -- and
`fchdir` publishes it. The dirfd unit's `-ENOENT` for "no coherent name"
remains only for the cases that genuinely have none: a directory whose
name would not fit, one whose base had no name, one renamed between the
open and the naming walk. `lxtest`'s dirfd block checks the new answer:
the `/tmp/lxdlink` directory `fchdir`s and `getcwd` answers `/tmp/lxdir`;
the `/tmp/lxdeep/..` directory answers `/tmp/lxdir` too, not `/tmp`.

### 4. procfs's `..`

`proc_pid_lookup` answers `..` with the mount's root (`dir->mnt->root`,
referenced) -- the directory its own readdir already names with that
inode number. `.` is handled by the VFS before any filesystem sees it.

### 5. The invariant

**P32. A published directory name is the path the walk took.** The name
`chdir` publishes, the one a spawned child is given, and the one a
directory file records for `fchdir` are each the traversed name of the
walk that reached the vnode (`vfs_lookup_named`): made only of components
entered as directories, links replaced by where they led, `..` removing
a component, relative to the caller's root. So the name, walked again
with no link, reaches the same directory -- until a rename moves it,
which P27 already says. P27's wording ("swapping vnode and normalised
string together") changes to name the traversed name, and P31's "refuses,
`-ENOENT`, a directory with no coherent name" narrows to the cases §3
lists.

### 6. The §70 gate

**Correctness.** One rule in one place (the walk), used by all three
publishers; the rule is the walk's own. procfs gains the `..` its
listing already claims.

**Concurrency.** The name is private to the walk, a buffer the caller
owns; no lock is added and none is held longer. A concurrent rename can
make the name stale (P27), never torn: it is built from components as
the walk entered them.

**Ownership and lifetime.** The caller owns the buffer; the walk writes
it and never keeps it. A directory file owns its recorded name as today.

**Security.** A name reveals nothing the process could not learn: it is
made of the components the process named and the targets of links it
followed, each of which `readlink` would show it, and it cannot rise
above the process's root (§1). A process confined to a root sees names
relative to it, as today.

**Failure.** `-ENAMETOOLONG` for a traversed name that does not fit,
when one is asked for; otherwise what the walk answers today. procfs's
`..` cannot fail.

**Performance.** A walk that asks for no name does one pointer test per
component. A named walk appends each component into a buffer the caller
already has: `chdir`, spawn with a `cwd`, and directory opens, all of
which already did a `path_normalize` that this replaces. The directory
open's second walk (the naming lookup) is the same second walk
`file_set_dir_path` does today.

## Affected files

| file | change |
| --- | --- |
| kernel-services/vfs/vfs.c | `struct walk` gains the traversed name (buffer, length, capacity); `walk_parent`, `resolve` and the link expansion maintain it; `vfs_lookup_named`; `file_set_dir_path` takes a base, its name and the path; `no_links` removed |
| kernel/include/kernel/vfs.h | `vfs_lookup_named`; `file_set_dir_path`'s new signature |
| kernel/process/process.c | `chdir_inner` publishes the traversed name |
| kernel/process/spawn.c | the child's `cwd` name from a named lookup |
| kernel/syscall/native.c, compat/linux/syscalls.c | the directory-open sites pass base, name and path |
| kernel-services/filesystem/procfs/procfs.c | `proc_pid_lookup` answers `..` |
| kernel-services/vfs/vfstest.c, kernel/process/proctest.c | the tests below |
| userland/init/init.c, tests/linux/lxtest.c | the door checks below; the dirfd block's `fchdir` expectations |
| docs | P32, P27 and P31 reworded; the VFS and process API docs; the Linux API rows for `chdir`, `fchdir`, `getcwd`; testing docs; the inventory row struck; README Status |

## APIs

### New

```c
/* vfs_lookup, and the traversed name of what it reached: made of the
 * components entered as directories, links replaced by where they led,
 * `..` removing a component, relative to the caller's root (P32).
 * `startname` names `start` and is ignored for an absolute path; a
 * relative walk from a start with no name is -ENOENT. -ENAMETOOLONG when
 * the name would not fit `n`. */
int vfs_lookup_named(struct vnode *start, const char *startname, const char *path,
                     struct vnode **out, char *name, size_t n);
```

### Changed

```c
/* Record the traversed name of `path` from `start` (named `startname`)
 * as `f`'s directory name, if that walk reaches f->vn. */
void file_set_dir_path(struct file *f, struct vnode *start, const char *startname, const char *path);
```

### Existing, relied on

`path_normalize` stays (other callers, and its own test table);
`cwd_publish`, `process_cwd_snapshot`, `at_base` unchanged.

## Migration plan

One PR. The walk change first (a walk with no name asked for behaves
exactly as today, which the whole suite checks), then the three
publishers, then procfs, then the tests and the dirfd block's changed
expectations. No on-disk or ABI change: `getcwd` still returns a string,
it is now the right one.

## Tests

| test | door | checks | mutation it must catch |
| --- | --- | --- | --- |
| `vfs-lookup-named` (new, vfstest) | kernel | a table of (start name, path) → name: plain components; `.`; `..` including at `/`; an absolute link mid-path; a relative link mid-path; a link as the last component; a chain of two links; `link/..` (the physical parent, not the lexical one); a trailing slash; a component crossing into a mount and `..` back out of it; `-ENAMETOOLONG`; a relative walk from a start with no name. Each result also walked again with `vfs_lookup` and compared by vnode | `..` not removing a component; an absolute link not resetting the name; a link's own name appended |
| `init` working-directory checks (extended) | native | `chdir` through a link to a deeper directory: `getcwd` is the target's path; `chdir("..")`: its parent's; `chdir("link/..")`: the physical parent; `stat(".")` and `stat(getcwd())` agree after each; `chdir("/proc/self")` then `chdir("..")` succeeds and `getcwd` is `/proc`; a child spawned with `cwd` naming the link is given the physical name | publishing the lexical name again; procfs `..` removed |
| `lxtest` (extended, and the dirfd block updated) | Linux | the same `chdir` sequence at the Linux door; `fchdir` of the directory opened through `/tmp/lxdlink` succeeds and `getcwd` answers `/tmp/lxdir`; the `/tmp/lxdeep/..` directory answers `/tmp/lxdir` | the directory-open name reverting to the lexical one |
| `process-spawn` (extended) | kernel | a spawn whose `cwd` is a relative path through a link gives the child the traversed name | spawn keeping `path_normalize` |

Each mutation is run alone, with the runner confirming each boot booted.

## Benchmarks

None needed: the named walk replaces a `path_normalize` of the same
string at each site, and an unnamed walk gains one pointer test per
component. The build reports `chdir`'s cost before and after only if
the boot's timing summary moves.

## Risks

- **`/proc/self` names the caller.** A process in `/proc/self` has the
  name `/proc/self`, which is coherent for it; a child that inherits the
  name and its parent's vnode holds a name that, walked by the child,
  reaches the child's own directory. This is the only name component in
  the tree whose meaning depends on who walks it. Not changed here; the
  build records whether any test inherits such a cwd, and the report's
  follow-up is to have procfs's `self` behave as the link it stands in
  for (which would make the rule of §1 name it by pid).
- **Staleness is unchanged, and now visible in a new place.** A
  directory file's name was never recorded for a link-reached directory;
  now it is, and it goes stale on a rename like every other name (P27).
- **`fchdir`'s answer changes** from `-ENOENT` to success for directories
  opened through a link. Anything that relied on the refusal: only the
  dirfd unit's own tests, which this unit updates.
- **Shells that want logical names.** `sh`'s `cd link; cd ..` now lands
  where the directory is, as a Linux kernel's `chdir("..")` does; a shell
  that wants the logical behaviour keeps `$PWD`, which `sh` does not do.

## Alternatives considered

- **Name a directory by reading its parent**: climb with `..` and, at
  each level, readdir the parent for the entry whose inode is the
  child's -- the classic userland `getcwd`. Every directory type has a
  readdir and a vnode carries its inode number, so it nearly works. It
  fails on the two filesystems with special names: cosmofs's
  `.snapshots` is found by name only and readdir deliberately does not
  list it, so nothing inside a snapshot could be named; and procfs's
  process directories do not answer `..` (the second defect, found
  here). It also costs a readdir per level, under the rename lock.
- **A dentry cache or parent pointers**: the inventory row's suggestion.
  Either is a large change to the VFS's ownership model for a name the
  walk already knows.
- **Refuse `chdir` through a link**, as the dirfd unit did for `fchdir`:
  breaks every `cd` into a linked directory.
- **Keep the logical name, and make `..` logical too**: `chdir("..")`
  would then have to walk the *name's* parent instead of the vnode's,
  which is a second meaning of `..` that no other call has and that
  Linux does not have.
