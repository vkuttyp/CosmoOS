# NEXT SUBSYSTEM — a name that points somewhere else

Date: 2026-09-15. Tree: `main` at 30e86aa (after PR #140, the hardening
unit). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: symbolic links — a fourth vnode type, two vnode operations,
the path walk that expands them with a budget, the three system calls
that create, read and stat them without following, and both filesystems
storing them.** **Built: PR #142 (2026-09-15).** The design below is as
proposed; the sections "As built" and "As run" record what the build
changed and measured. Differences from the plan, each found by building
rather than reading:

1. **`vfs_open` became a loop rather than a special case.** The plan had
   the last link expanded and then resolved; that made
   `open("dangling", O_CREAT)` return `ENOENT` instead of creating the
   target, because the create branch had already run against the link.
   The whole resolution is now a loop that re-enters with the target
   path, so `O_CREAT`, `O_TRUNC` and the permission checks all apply to
   the target -- which is what each of those flags means -- and the
   function has one cleanup path, which the plan's shape did not (a
   permission failure returned while holding the link directory's
   reference and the walk's buffer).
2. **The last component's name is copied.** `walk_parent` returned a
   pointer into the path; once the path can live in the walk's own
   scratch, that pointer would not outlive it. The name is copied into a
   caller-supplied `VFS_NAME_MAX + 1` buffer instead, which kept every
   call site's code unchanged (`last` is an array rather than a pointer)
   and let the scratch be freed inside the walk.
3. **A link's creation ends when the entry is published.** The plan had
   `symlink` return the new vnode. cosmofs's last fallible step would
   then be fetching one, and failing there reports an error for a link
   that exists, so a retry meets `EEXIST`. The contract now allows
   `*out` to be NULL on success; cosmofs sets it and returns as soon as
   `dir_add` succeeds, and nothing fallible follows.
4. **`cfs_inode_discard`.** The inode allocator is a bump allocator with
   no free list, so the undo is a rollback of `next_ino` under the lock
   that made the allocation -- exact, because nothing else can allocate
   in between.
5. **`COSMO_ELOOP` had to be exported.** The user-visible errno list did
   not carry it, and a user-mode test of `O_NOFOLLOW` needs it.
6. **The `ls` change needed `lstat`, not a type hint.** `ls` passed
   `readdir`'s type through and stat'ed for the rest; a link's type has
   to come from `lstat` or the column shows what it points at.

This report is to close the first clause of
the inventory's §3 row "no symlinks in the VFS; no dentry cache (every
component calls the filesystem); no `(ino, generation)` identity; no
mount options string; no bind or overlay stacking" (audit 8.3). What
that row keeps is named at the end.

A path in this kernel names exactly one file. `walk_parent` resolves
each component with the filesystem's `lookup`, crosses a mount if the
result is one, and moves on
(`kernel-services/vfs/vfs.c:708-768,773-837`); the header says what that
means in four words -- "Follows mounts; no symlinks"
(`kernel/include/kernel/vfs.h:234`). There are three vnode types
(`vfs.h:32-36`: `VNODE_REG`, `VNODE_DIR`, `VNODE_CHR`), and a
filesystem cannot offer a fourth: `vnode_ops` has no `symlink` and no
`readlink` (`vfs.h:43-79`), the native ABI has no call that would reach
them, and `struct cosmo_stat` has no type to report
(`uapi/cosmo/syscall.h:509-514`, six `COSMO_DT_*` values, none a link).

The consequence reaches further than a missing call. The Linux
personality answers `readlink` and `readlinkat` with `-ENOSYS`
(`compat/linux/syscalls.c:1918-1920,1971`); `symlink` and `symlinkat`
are not in either table, so they are unhandled; `lstat` is *aliased to
`stat`* (`syscalls.c:1834-1836`), and `newfstatat` reads its flags word
and ignores `AT_SYMLINK_NOFOLLOW` -- the constant is defined
(`linux_abi.h:49`) and referenced nowhere else, so a Linux program that
asks not to follow a link is told about the target instead, silently.
`ls` has no `l` in its type column (`userland/coreutils/ls.c:11-19`) and
stats every name through the follow path. A package manifest has files
and directories and no third kind (`pkg/pkg.h:66-86`), so a package that
wants `/bin/sh` to be a name for something else has to copy it.

This unit makes a link a first-class node: `symlink(2)` and
`readlink(2)` and `lstat(2)` in the native ABI, `O_NOFOLLOW` in `open`,
the six Linux entry points below answered properly -- including
`AT_SYMLINK_NOFOLLOW`, which is read and dropped today -- `VNODE_LNK` in
both filesystems (a bounded string in ramfs,
the file's own bytes in cosmofs behind a format-version gate), and a
walk that expands a link with a budget, refuses a loop with `ELOOP` and
a long expansion with `ENAMETOOLONG`, and -- the part that matters for
this kernel's process roots -- restarts an absolute target at *the
calling process's* root, so a link cannot name its way out of a root the
way a leading slash already cannot (`vfs.c:783-788`).

## Problem

**There is no fourth type.** `enum vnode_type` has three members and
they alias the `COSMO_DT_*` values (`vfs.h:32-36`), so a new type is a
new value in the stable ABI (`COSMO_DT_LNK`, the next free number is 6)
and a new arm everywhere a type is switched on: `vnode_stat`
(`vfs.c:175-187`), the Linux mode and dirent conversions
(`compat/linux/convert.c:44-48,164-168`, both of which today fall
through to "regular file"), `ls`'s type column, and cosmofs's inode
decode (`cosmofs.c:814-815`), which maps anything that is not
`CFS_TYPE_DIR` to `VNODE_REG`.

**The walk has nowhere to put the expansion.** `step()` resolves one
component: `.` and `..` by hand, everything else through
`dir->ops->lookup` under `dir->lock` followed by `follow_mount`
(`vfs.c:708-768`). `walk_parent` loops over components and returns the
parent and the last name (`vfs.c:773-837`). Expansion is not one more
case in `step`, because a symlink rewrites *the rest of the path*: the
loop's state (`path`, `cur`) has to change, and the budget that stops
`a -> b -> a` has to live across components. The existing limits are a
1024-byte path (`VFS_PATH_MAX`, `vfs.h:24`), 255-byte components, and
40 components (`VFS_MAX_COMPONENTS`, `vfs.h:25`) -- the last already
returning `ELOOP` (`vfs.c:822-825`), and counting only non-final
components.

**Neither filesystem can store one.** ramfs builds a node's ops from its
type (`ramfs.c:41-64`, `ramfs_dir_ops` or `ramfs_file_ops`) and keeps
file bytes in the page cache, which for ramfs *is* the store
(`vfs.h:118`, `MOUNT_CACHE_IS_STORE`). cosmofs encodes the type in the
mode's top nibble (`cosmofs_format.h:123-126`: `CFS_TYPE_REG 1`,
`CFS_TYPE_DIR 2`), has no inline data at all -- every file's bytes go
through extents and the page cache, with LZ4 records for small ones
(`cosmofs_format.h:143-147`, `cosmofs.c:1352`) -- and has exactly one
spare `uint32_t` in its 256-byte inode (`cosmofs_format.h:244-262`).
Its directory entry carries a type byte and six spare bytes
(`cosmofs_format.h:337-343`).

**An older kernel would misread a link.** cosmofs mounts any superblock
version between `CFS_VERSION_MIN 2` and `CFS_VERSION 7`
(`cosmofs_format.h:18-19`, checked at `cosmofs_core.c:83,1052-1055`).
A link written as a new type nibble into a version-7 filesystem would be
read by a version-7 kernel as a regular file whose contents are the
target: not an error, a wrong answer.

## Current implementation

- **The walk.** `step` (`vfs.c:708-768`) and `walk_parent`
  (`vfs.c:773-837`), with `follow_mount` (`vfs.c:647`, depth cap 16) and
  `may_search` for the per-component permission check. Callers:
  `vfs_lookup` (`vfs.c:845`), `vfs_open` (`vfs.c:947`), and the mutation
  helper at `vfs.c:1288` that `unlink`, `rmdir`, `rename` and `mkdir`
  share.
- **Types and stat.** `enum vnode_type` (`vfs.h:32-36`), `vnode_stat`
  (`vfs.c:175-187`), `struct cosmo_stat` (`syscall.h:516-527`),
  `struct cosmo_dirent` with its type byte (`syscall.h:530-535`).
- **The native ABI.** `SYS_open 11` … `SYS_umount 22`
  (`syscall.h:32-43`), `SYS_COUNT 89` with 88 the highest used
  (`syscall.h:136-137`); `sys_stat` takes a path and a buffer and no
  flags (`native.c:458-471`); `sys_open`'s accepted flag mask is
  `ACCMODE|CREAT|EXCL|TRUNC|APPEND|DIRECTORY` and anything else is
  `-EINVAL` (the hardening unit's rule, `native.c`).
- **The Linux personality.** As quoted under Problem.
- **The filesystems.** ramfs (`ramfs.c:23-36,41-64,274-290`), cosmofs
  (`cosmofs_format.h`, `cosmofs.c:814-815,898,1344-1377,1593,1673`).
- **The tests.** `kernel-services/vfs/vfstest.c` (1311 lines; each test
  a `bool selftest_x(const char **reason)` with a line-naming `CHECK`,
  `vfstest.c:32-39`), `cosmofstest.c`, `cosmofscrash.c`, and the
  user-mode `fs_selftest` (`userland/init/init.c:48`).

## Why it matters

A symbolic link is the only thing in a filesystem that a program cannot
emulate. A missing `chmod` costs permissions; a missing link costs
*naming*: `/bin/sh` cannot be a name for `/bin/dash`, a package cannot
ship a versioned library with an unversioned name, and `/etc/localtime`
cannot point into a database. Every real userland this kernel is
growing toward -- the package manager in `pkg/`, a Linux root
filesystem, a second shell -- assumes links exist.

The Linux personality's state is worse than absent: `lstat` aliased to
`stat` and `AT_SYMLINK_NOFOLLOW` ignored are *wrong answers* to
questions a program asks specifically to avoid following a link. A
program that checks whether a path is a link before replacing it is
told "it is a regular file", and replaces the target. That is the shape
of a security bug in a program that is not wrong.

And the walk is where this kernel's own rules meet: per-process roots
(`vfs_current_root`), mount namespaces, mount crossing, and the `..`
rule that was written carefully enough to be commented at length
(`vfs.c:717-726`). Adding expansion to it is the test of whether those
rules compose. A link whose target is absolute must restart at the
*process's* root, or a process confined to a root can be handed a link
that names its way out -- the exact escape the leading-slash rule
already prevents.

## Design

### A fourth type, and two operations

```c
enum vnode_type {
    VNODE_REG = COSMO_DT_REG,
    VNODE_DIR = COSMO_DT_DIR,
    VNODE_CHR = COSMO_DT_CHR,
    VNODE_LNK = COSMO_DT_LNK,   /* 6 */
};
```

`vnode_ops` gains two, both optional (a filesystem that offers neither
simply has no links; `vfs_symlink` returns `-EPERM` as Linux does for a
filesystem that cannot):

```c
/* Create `name` in `dir` as a link to `target` (a NUL-terminated path,
 * at most VFS_PATH_MAX-1 bytes, never validated: a dangling link is a
 * link). */
int (*symlink)(struct vnode *dir, const char *name, size_t len, const char *target, struct vnode **out);
/* Copy the target into `buf`, at most `len` bytes, NOT NUL terminated.
 * Returns the number of bytes copied, or -errno. */
int (*readlink)(struct vnode *vn, char *buf, size_t len);
```

`readlink` returning a count rather than filling a buffer is POSIX's
shape and the one the walk wants: the walk asks for `VFS_PATH_MAX` and
gets back a length.

### The walk expands, with a budget

Expansion belongs to `walk_parent`, not to `step`, because a link
rewrites the remainder of the path. The loop gains three pieces of
state: `links` (how many expansions so far), `buf` (the rewritten path,
allocated only when the first link is met), and the rule for where to
continue.

`step` consumes the caller's reference to `dir` before it returns the
child (`vfs.c:764`), so the walk cannot "continue from `cur` as it was":
that reference is gone. Before each `step`, `walk_parent` takes a second
reference on `cur` and holds it as `linkdir`; when the child is not a
link it is released immediately (one atomic pair per component), and
when it is, `linkdir` is exactly the directory a relative target
resolves against. `step`'s own ownership contract is unchanged, which
keeps the `..` path -- where `step` swaps `dir` for the mountpoint and
puts the original (`vfs.c:739-744`) -- as it is.

When `step` returns a child whose type is `VNODE_LNK`:

1. If `links == VFS_MAX_SYMLINKS` (8), release `linkdir` and the child
   and return `-ELOOP`.
2. Read the target with `readlink` into a `VFS_PATH_MAX` scratch, then
   release the child: the link's own vnode is not needed past this
   point.
3. Form the new path: the target, then `/`, then the unconsumed
   remainder of the current path. If that exceeds `VFS_PATH_MAX - 1`,
   return `-ENAMETOOLONG`.
4. If the target begins with `/`, release `linkdir` and continue from
   `vfs_current_root()` -- the *calling process's* root, not the global
   one. Otherwise continue from `linkdir`, the directory the link was
   found in, whose reference the walk is already holding.
5. `links++`, and go round the loop with the new path.

The scratch is one `kmalloc(VFS_PATH_MAX)` per walk that meets a link,
freed on every exit path, and never taken by a walk that meets none:
two kilobytes of kernel stack in every path resolution is not a trade
this kernel makes (the file-path unit sized its bounce for exactly this
reason), and the allocation failing is `-ENOMEM`, which a caller can
already receive.

Two budgets, not one: `VFS_MAX_SYMLINKS` (8) bounds expansions and
`VFS_MAX_COMPONENTS` (40) still bounds components across the whole walk
including expanded ones, so a link chain that grows the path cannot
outrun either. Both return `ELOOP`, which is what Linux returns for
both.

`..` after an expansion means the target's parent, because the
expansion is textual and happens before the component is stepped --
Linux's behaviour, and the only one consistent with `..` being resolved
by the filesystem.

### The last component: who follows and who does not

The walk above resolves every component *except* the last;
`walk_parent`'s contract is unchanged. Whether the last one is followed
is the caller's decision, and every caller states it:

| call | the last component |
| --- | --- |
| `vfs_open` without `COSMO_O_NOFOLLOW` | followed; a dangling link is `-ENOENT` |
| `vfs_open` with `COSMO_O_NOFOLLOW` | not followed; a link is `-ELOOP` (Linux's answer) |
| `vfs_open` with `COSMO_O_CREAT` on an existing link | followed, as Linux does: the target is created or opened |
| `vfs_stat` | followed |
| `vfs_lstat` (new) | not followed: the link's own type, size (the target's length) and times |
| `vfs_readlink` (new) | not followed, and `-EINVAL` if it is not a link |
| `vfs_unlink`, `vfs_rename` | never followed -- they name an entry in a parent, which they already do |
| `vfs_rmdir` | never followed; a link is `-ENOTDIR` |
| `vfs_mkdir` | the parent walk follows; the new name is created |
| a trailing slash (`link/`) | followed, and `-ENOTDIR` unless the target is a directory |

`vfs_open`'s existing trailing-slash rule (`vfs.c` adds
`COSMO_O_DIRECTORY` when the path ends in `/`) composes with this: a
link to a directory named with a trailing slash opens the directory.

### The system calls

Three new numbers, and one new flag bit:

| nr | call | returns |
| --- | --- | --- |
| 89 | `symlink(const char *target, const char *path)` | 0, or `EEXIST`, `ENOENT`, `ENAMETOOLONG`, `EPERM`, `EROFS`, `ENOSPC` |
| 90 | `readlink(const char *path, char *buf, size_t len)` | bytes copied (not NUL terminated), or `EINVAL` if not a link |
| 91 | `lstat(const char *path, struct cosmo_stat *st)` | 0 |

`SYS_COUNT` 89 → 92. `COSMO_O_NOFOLLOW` is `0x20000`, the next bit
above `COSMO_O_DIRECTORY`, and joins `sys_open`'s accepted mask -- which
the hardening unit made exhaustive, so adding a flag is now a
one-line change in exactly one place (`native.c`). `COSMO_DT_LNK` is 6.
The libc wrappers (`libc/include/cosmo/syscall.h`) gain the three.

`readlink` copying without a terminator is POSIX and is what the Linux
personality needs; the libc wrapper does not add one either, and the
API document says so once, loudly.

### The Linux personality stops lying

Six entry points, the same six everywhere this report counts them:

1. `readlink` -- `-ENOSYS` today, implemented over `vfs_readlink`.
2. `readlinkat` -- `-ENOSYS` today, the same.
3. `symlink` -- absent from both tables, added over `vfs_symlink`.
4. `symlinkat` -- absent from both tables, added.
5. `lstat` -- stops being an alias for `stat` (`syscalls.c:1834-1836`)
   and becomes the non-following one.
6. `newfstatat` -- honours `LX_AT_SYMLINK_NOFOLLOW` (`linux_abi.h:49`),
   which it currently reads and drops.

With, in support:
- `convert.c` gains `LX_S_IFLNK` (`0xA000`) and `LX_DT_LNK` (10) arms,
  so a link stops being reported as a regular file
  (`convert.c:44-48,164-168`).
- `LX_O_NOFOLLOW` (already in the accepted open mask, `convert.c:30`)
  maps to `COSMO_O_NOFOLLOW` instead of being dropped.

### ramfs: the target is the node

`struct ramfs_node` gains `char *target` (kmalloc'd, NUL terminated,
freed in evict) and `ramfs_new` gains a third arm giving
`ramfs_lnk_ops` -- `readlink` only, no `readpage`/`writepage`, so a
link's bytes are never a page. `ramfs_symlink` allocates the node,
copies the target, and links it into the parent exactly as
`ramfs_create_common` does (`ramfs.c:111`), so the pinned-vnode
invariant (V9) is unchanged.

### cosmofs: the target is the file's bytes, behind a version gate

`CFS_TYPE_LNK 3` in the mode's top nibble
(`cosmofs_format.h:123-126`), and the target stored as the file's own
data: one block, `size` = the target's length, `compress_algo` none.
The inode's spare `uint32_t reserved` (`cosmofs_format.h:261`) stays
spare; a 4-byte inline target would buy nothing.

**The target block is written in the same transaction as the inode and
the entry, not through the page cache.** `cfs_create_common` allocates
the inode, writes it and adds the directory entry under `fs->lock`
(`cosmofs.c:1314-1371`), all joining the open transaction; a regular
file's *data*, by contrast, reaches disk from the page cache at
write-back (`cfs_writepage`, `cosmofs.c:1752`), which can be a later
transaction. A link built that way could commit as a zero-length link
whose target is not there yet -- a durable wrong answer after a crash,
not a lost write. So `cfs_symlink` allocates the data block and writes
the target through the same buffered-block path the directory entries
use, sets the extent in the inode, and only then writes the inode and
adds the entry: one transaction, committed or not at all. `cfs_readlink`
reads it back the same way.

**And it leaves nothing behind when it fails.** Allocation happens in
the in-memory bitmap, which is authoritative for the open transaction
(`cosmofs_core.c:6-9`), so a failure after `cfs_alloc_data` and before
the entry exists would otherwise commit an unreachable block -- the
directory never names the inode, and nothing ever frees it. Every path
out of `cfs_symlink` after an allocation therefore either returns the
block with `cfs_free_block_deferred` and the inode to the inode map, or,
where the undo itself cannot be done (the deferred free list has no
memory, which today logs and leaks, `cosmofs_core.c:387`), abandons the
transaction with `cfs_fail` (`cosmofs_core.c:665-671`) so the last
committed root stays current and nothing partial reaches the disk. The
order is chosen so the cheap failures come first: the name-length and
version checks, then `dir_find` for `EEXIST`, then the inode, then the
block, then the entry. A link's bytes are therefore never a page in
either filesystem, which is also why `ramfs_lnk_ops` has no
`readpage`/`writepage`.

`CFS_VERSION` 7 → 8. `CFS_VERSION_MIN` stays 2, so every existing image
still mounts. The gate is on creation, not on mount: `cosmofs_symlink`
returns `-EOPNOTSUPP` when the mounted superblock's version is below 8,
because a link written into a version-7 filesystem would be read by a
version-7 kernel as a regular file whose contents happen to be a path --
a wrong answer rather than a refusal. `mkfs` formats at 8; the existing
`cosmofs_test_format_version` hook (`cosmofs_core.c:1033`) is how the
test mounts a version-7 image and sees the refusal.

The directory entry's type byte gains the value; its six spare bytes
(`cosmofs_format.h:337-343`) stay spare.

### The §70 gate

*Ownership and lifetime.* A link is a vnode like any other: pinned by
its parent in ramfs, referenced through the cache in cosmofs, evicted
the same way; ramfs frees the target string in `evict`. The walk's
scratch buffer is owned by `walk_parent` and freed on every path out of
it, including the error paths that already exist.

*Concurrency.* The expansion reads the target with the link's own
`readlink` under the link's `lock`, then drops the reference before
continuing -- the walk holds one directory reference at a time, as it
does now. A link unlinked mid-walk behaves as a file unlinked mid-walk
does: the reference already taken keeps the node alive and the walk
finishes against it, or `VNODE_DEAD` makes the next lookup `-ENOENT`
(`vfs.c:762`).

*Memory.* One `VFS_PATH_MAX` allocation per walk that meets a link, and
one per `readlink` call in the caller's own buffer. ramfs adds the
target's bytes per link; cosmofs adds one block per link.

*Error handling.* `ELOOP` for a cycle, for exceeding the expansion
budget, and for `O_NOFOLLOW` on a link; `ENAMETOOLONG` for an expansion
that does not fit; `EINVAL` for `readlink` on a non-link; `EPERM` for
`symlink` on a filesystem without the operation; `-EOPNOTSUPP` for a
cosmofs older than version 8; `ENOENT` for a dangling target, from the
lookup that fails, with no special case.

*Security.* The one that matters: an absolute target restarts at
`vfs_current_root()`, so a link is bounded by the same root a leading
slash is (`vfs.c:783-788`), and a process confined to a subtree cannot
be handed a link out of it. Every component of an expanded path is
searched with the same `may_search` credential check as any other
component, so a link grants no access its user did not have; the target
is *not* checked when the link is created, because a link to something
unreadable is not itself a leak and a dangling link is legal. Mount
namespaces are unaffected: expansion re-enters the same walk, which
resolves mounts through the caller's namespace.

*Performance.* One branch per component in the common case (the type
test), no allocation, no extra lookup. A path containing a link costs
one `readlink` (a page-cache read in cosmofs, a memcpy in ramfs), one
allocation, and a re-walk of the remainder.

*Future extensibility.* `readlink`/`symlink` are the first two of the
`vnode_ops` the audit's 8.3 lists; `link` (hard links) is the natural
next and shares the nlink accounting the file-path unit already relies
on. A dentry cache would cache expansions; nothing here forecloses it.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/vfs.h` | `VNODE_LNK`, `VFS_MAX_SYMLINKS`, `symlink`/`readlink` in `vnode_ops`, `vfs_symlink`/`vfs_readlink`/`vfs_lstat`, the "no symlinks" comment at :234 |
| `kernel-services/vfs/vfs.c` | expansion in `walk_parent`; the follow/no-follow rule per caller; `vfs_symlink`, `vfs_readlink`, `vfs_lstat`; `vnode_stat` unchanged but now reachable with the new type |
| `kernel-services/vfs/ramfs.c` | `ramfs_lnk_ops`, the target in `struct ramfs_node`, `ramfs_symlink`, `evict` |
| `kernel-services/filesystem/cosmofs/cosmofs_format.h` | `CFS_TYPE_LNK`, `CFS_VERSION` 8 |
| `kernel-services/filesystem/cosmofs/cosmofs.c` | the type arm in the inode decode/encode, `cosmofs_symlink`, `cosmofs_readlink`, the dirent type, the version gate |
| `kernel-services/filesystem/cosmofs/cosmofs_core.c` | format at version 8; the per-version gate beside the existing ones (:825-874) |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_DT_LNK`, `COSMO_O_NOFOLLOW`, `SYS_symlink`/`readlink`/`lstat`, `SYS_COUNT` 92 |
| `kernel/syscall/native.c` | the three calls, the table rows, `O_NOFOLLOW` in the accepted mask |
| `libc/include/cosmo/syscall.h` | the three wrappers |
| `compat/linux/syscalls.c`, `nr_x86_64.h`, `nr_aarch64.h` | the six entry points: `readlink`, `readlinkat`, `symlink`, `symlinkat`, `lstat` unaliased, `newfstatat`'s flag |
| `compat/linux/convert.c` | `S_IFLNK` and `DT_LNK` arms |
| `userland/coreutils/ls.c` | the `l` type char, `lstat` for the type column, `-> target` in the long form |
| `kernel-services/vfs/vfstest.c` | the walk and type tests |
| `kernel-services/filesystem/cosmofs/cosmofstest.c` | persistence and the version gate |
| `userland/init/init.c` | `fs_selftest`'s user-mode round trip |
| `tests/linux/` | the Linux-ABI program's readlink/symlink/lstat case |
| docs | `docs/kernel-services/vfs/{api,design,invariants,testing,architecture}.md`, `docs/kernel-services/filesystem/cosmofs/{design,architecture}.md` (a "Format version 8" section beside the existing ones), `docs/compat/linux/api.md`, README Status, `docs/README.md`, the inventory |

## New APIs

```c
/* kernel/include/kernel/vfs.h */
#define VFS_MAX_SYMLINKS 8u

int vfs_symlink(struct vnode *start, const char *path, const char *target);
int vfs_readlink(struct vnode *start, const char *path, char *buf, size_t len);   /* bytes, or -errno */
int vfs_lstat(struct vnode *start, const char *path, struct cosmo_stat *st);

/* struct vnode_ops, both optional */
int (*symlink)(struct vnode *dir, const char *name, size_t len, const char *target, struct vnode **out);
int (*readlink)(struct vnode *vn, char *buf, size_t len);

/* kernel/include/uapi/cosmo/syscall.h */
#define COSMO_DT_LNK      6
#define COSMO_O_NOFOLLOW  0x20000
#define SYS_symlink       89
#define SYS_readlink      90
#define SYS_lstat         91
#define SYS_COUNT         92
```

`vfs_open` keeps its signature; `COSMO_O_NOFOLLOW` travels in its
existing `flags`.

## Migration plan

1. **The type and the operations.** `VNODE_LNK`, `COSMO_DT_LNK`, the
   two `vnode_ops`, `vnode_stat`'s reachability, and the arms in the
   Linux conversions -- with no filesystem offering a link yet, so
   nothing changes behaviour and the tree stays green.
2. **ramfs stores one.** `ramfs_symlink`, `ramfs_lnk_ops`,
   `vfs_symlink`/`vfs_readlink`/`vfs_lstat`, and the first test: create,
   read back, `lstat` sees a link, `stat` of a link to a file sees the
   file. The walk does not expand yet, so a link in the middle of a path
   is `-ENOTDIR` -- asserted, so step 3 has something to change.
3. **The walk expands.** The budget, the scratch, the absolute-target
   rule, `..` after a link, and the loop, dangling, and length tests.
4. **The last component.** `O_NOFOLLOW`, the per-caller rule, and the
   table above turned into tests.
5. **cosmofs stores one.** `CFS_TYPE_LNK`, version 8, the gate, the
   target written inside the creating transaction, persistence across
   unmount, and the crash-consistency suite with a symlink case added.
6. **Both personalities and the userland.** The six Linux entry points
   above, the two conversion arms, `ls`, the user-mode round trip, and
   the Linux-ABI program's case.
7. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures; steps 3 and 5 also run
`make test-crash`, the release build and the host tests; step 5 runs the
cosmofs crash suite explicitly.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `vfs-symlink` (ramfs) | create, `readlink` returns the exact bytes with no terminator, `lstat` reports `COSMO_DT_LNK` and the target's length as the size, `stat` reports the target's type, `readdir` shows the link's type, `unlink` of the link leaves the target | make `readlink` NUL-terminate: the length assertion fails |
| `vfs-symlink-walk` | a link to a directory is walked through; a relative target resolves against the link's own directory, not the caller's cwd; an absolute target restarts at the process root; `..` after a link names the target's parent; `link/` opens the directory | resolve a relative target against the cwd: the "link's own directory" case fails |
| `vfs-symlink-root` | in a process rooted below the global root, an absolute target names inside that root and cannot escape | restart at the global root: the escape test finds the outer file |
| `vfs-symlink-loop` | `a -> b -> a` is `ELOOP`; a chain of 8 resolves; a chain of 9 is `ELOOP`; a link to itself is `ELOOP` | raise the budget to 9: the chain-of-9 case resolves and the test fails |
| `vfs-symlink-dangling` | `open` is `ENOENT`, `lstat` succeeds, `readlink` succeeds, `unlink` removes it | make `symlink` validate the target: creation fails |
| `vfs-symlink-nofollow` | `open` with `O_NOFOLLOW` on a link is `ELOOP`; on a non-link it is the file; `O_NOFOLLOW` on a path whose *intermediate* component is a link still follows | apply `O_NOFOLLOW` to intermediate components: the third case fails |
| `vfs-symlink-toolong` | a target plus remainder over `VFS_PATH_MAX` is `ENAMETOOLONG`, not a truncated path | drop the length check: the walk resolves a truncated name |
| `cosmofs-symlink` | a link survives unmount and remount with its target and type; `readdir` reports the type from the on-disk entry; the target occupies one block | store the target uncounted: the block accounting test fails |
| `cosmofs-symlink-crash` (in `cosmofscrash.c`, beside the existing prefix replays) | over every write prefix of a symlink creation, a replayed filesystem shows either no link at all or a link with its whole target -- never a zero-length one | write the target through the page cache instead of the creating transaction: a prefix replays to a link whose `readlink` returns 0 bytes |
| `cosmofs-symlink-nospace` | with a failure injected after the target block is allocated (the entry add refused), `symlink` returns the error, the filesystem's free-block count after the next commit equals the count before the call, and the inode number is reused by the next create -- so neither the block nor the inode leaked | skip the undo on the failure path: the free count is one short and the test fails |
| `cosmofs-symlink-version` | on a filesystem formatted at version 7 (`cosmofs_test_format_version`), `symlink` is `-EOPNOTSUPP` and everything else still works | drop the gate: the refusal assertion fails |
| `fs_selftest` (user mode) | `symlink`, `readlink`, `lstat` and `O_NOFOLLOW` through the libc wrappers; `ls -l` shows `l` and the arrow | leave `ls` following: the type column shows `-` |
| the Linux ABI program | `readlink`, `symlink`, `lstat` and `newfstatat` with `AT_SYMLINK_NOFOLLOW` each report the link, not the target | re-alias `lstat` to `stat`: the link case reports the target |

**Vacuity, named in advance.** Every "the link is followed" assertion is
paired with one where the target differs from the link (different
contents, different type, different size), so a test cannot pass by
reading the same bytes either way. The loop tests assert the *error*,
not merely that the call failed: a walk that returned `ENOENT` because
the chain was built wrong would fail them. `cosmofs-symlink`'s
persistence check reads the target after a remount with a cold cache,
so a value served from the page cache cannot stand in for the on-disk
one. And step 2's "a link in the middle of a path is `-ENOTDIR`" is
asserted before step 3 changes it, so the walk change has a test that
fails first.

### As run

The unit, on both architectures (PR #142, 2026-09-15):

| run | result |
| --- | --- |
| `make test` (x86-64) | 273 self-tests pass, including the four VFS link tests, the two cosmofs ones, and the two user-mode ones |
| `make test` (AArch64) | 273 pass |
| `make test-guard` (both) | pass on the protection-capable models |
| release boots (both) | pass |
| `make test-gic` (AArch64, both MSI configurations) | pass |
| `make test-crash`, `make test-wxn` | pass |
| host tests, `make fuzz`, `make analyze`, `make reproducible` | pass, clean, `reproducible: yes` |

The host layout test failed once on purpose: it pins `CFS_VERSION`, which
this unit bumps to 8. It now pins 8 and asserts that the inode did not
grow, which is the claim the version rests on.

`make test-wxn` also failed once from a stale sibling output tree
(`out/aarch64-debug-wxn`, left by the hardening unit) with a jump to a
null function pointer early in boot; a clean rebuild of that tree passes.
Every build fragment does track header dependencies, so the cause is not
established, and it is written down here rather than guessed at. CI
builds fresh and did not see it.

**Bug-proofs, as run** (each injection applied alone, then reverted):

| injection | result |
| --- | --- |
| `readlink-terminates`: the target NUL-terminated | `vfs-symlink` fails on `buf[4] == 'Z'` |
| `relative-wrong-base`: a relative target resolved against the caller's root | `vfs-symlink-walk` and `vfs-symlink-nofollow` fail on the through-a-directory-link reads |
| `absolute-global-root`: an absolute path resolved against the global root | the user-mode jail tests fail, the link one among them |
| `budget-plus-one`: the budget raised to 9 | `vfs-symlink-loop` fails on the pinned `VFS_MAX_SYMLINKS == 8` |
| `nofollow-everywhere`: no expansion of intermediate links | the same two walk tests fail |
| `no-length-check`: the over-long expansion trimmed instead of refused | `vfs-symlink-nofollow` fails on the `ENAMETOOLONG` assertion |
| `target-uncommitted`: the target left to page-cache write-back | `cosmofs-symlink` fails: the link is there and its target is not |
| `no-version-gate`: a link written to a version-7 filesystem | `cosmofs-symlink-version` fails |
| `lstat-alias`: the Linux `lstat` aliased to `stat` again | the Linux ABI program fails and the boot loses `LINUXTEST: PASS` |
| `ls-follows`: `ls` stats through the link | the user-mode test fails on the type column |

Five of those ten passed on their first run, and each exposed a test
that could not see its own bug -- which is the reason for running them:

- The budget test was built from the budget macro, so raising the budget
  moved the test with it. The value is pinned now.
- The over-long target was one enormous component, so the
  component-name check refused it and the expansion's own length check
  never ran. It is built from short components now.
- Nothing reached the Linux entry points at all, so the aliased `lstat`
  was untested. `lxtest` exercises all six now.
- Nothing asserted what `ls` prints, so making it follow links again
  changed nothing observable. The user-mode test reads its output now.
- The absolute-target injection missed, because the rule lives in
  `walk_parent`'s "absolute means absolute to this process" and the
  expansion inherits it by handing that code the expanded path. The
  injection now targets the rule itself -- worth saying plainly, since
  this unit did not write that rule, it relied on it.

One regression reached the branch and was caught in review: a
bug-proof injection. `vfs.c` was staged while the proof chain had the
`no-length-check` injection applied to it, so the trimming code was
committed; the chain's next stash then reverted the first attempt at
removing it. Both the code and the working habit are fixed (commit
before a proof chain starts, and treat the tree as the chain's until it
finishes).

## Benchmarks

Not measured, and the reason is worth stating rather than leaving a
gap: the cost this unit adds to a path that contains no link is one
type comparison per component and one reference pair, inside a walk that
already takes a mutex and calls into the filesystem for every component.
A benchmark of that would measure the run-to-run noise of the boot it
runs in, as the `USERBENCH` numbers from the file-path unit do at this
scale. What the unit does spend is bounded and stated instead: one
allocation of `2 * VFS_PATH_MAX` per resolution that meets a link and
none for one that does not, and one block per link on disk -- the latter
asserted by `cosmofs-symlink`, which watches the filesystem's own free
count fall when a link is made and rise when it goes.

## Risks

- **The walk is the most delicate function in the VFS.** Mount
  crossing, per-process roots, mount-namespace lookups and the `..`
  rule all live in `step`, and expansion changes the loop around it.
  The mitigation is the order of the plan: ramfs and the calls first,
  walking last, with the `..`-after-a-link and root-escape cases
  written as tests before the code.
- **An escape through an absolute target.** Named as its own test
  (`vfs-symlink-root`) because it is the one failure mode that would be
  a security bug rather than a wrong answer.
- **A cosmofs image written by this kernel and read by an older one.**
  The version gate is the answer, and it is enforced on creation
  because that is the only moment both versions are known.
- **`readlink` without a terminator** is a footgun for every caller.
  One sentence in the API document, one in the header, and the libc
  wrapper deliberately not adding one, so there is a single rule.
- **Scope creep into hard links and the dentry cache.** Both are named
  as adjacent work and neither is built here.

## Alternatives considered

- **Expand inside `step` instead of `walk_parent`.** Rejected: `step`
  resolves one component and returns one vnode; a link rewrites the
  remainder of the path, which is the loop's state, and threading that
  back out of `step` is the same change with a worse shape.
- **A recursive walk, as some kernels do.** Rejected: recursion depth
  on a 16 KiB kernel stack is exactly what the budget exists to bound,
  and an iterative rewrite makes the budget and the length check
  obvious.
- **Store the target inline in the cosmofs inode.** Rejected: four
  spare bytes. A "fast symlink" would need the inode's layout changed,
  which is a bigger format change than a type nibble for a saving that
  matters at a scale this filesystem is not at.
- **Follow the last component everywhere and add `lstat` only.**
  Rejected: `O_NOFOLLOW` is what a program that replaces a path safely
  needs, and it costs one flag bit in a mask the hardening unit already
  made exhaustive.
- **Hard links in the same unit.** Left out: they share the nlink
  accounting the file-path unit relies on and deserve their own tests
  (two names one inode, unlink one, the write-back rule for
  `nlink > 0`), which would double this unit's test surface.
- **A dentry cache to make expansion cheap.** Left out, and the
  inventory row keeps it: the row after this unit reads "no dentry
  cache (every component calls the filesystem); no `(ino, generation)`
  identity; no mount options string; no bind or overlay stacking".
