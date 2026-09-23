# NEXT SUBSYSTEM — a directory descriptor names a directory

Constitution §68 report. It takes up the third of the three Linux
personality gaps the deferred-work inventory lists together in §2.6 —
*"a real directory fd — `check_dirfd` returns `-ENOSYS` for any `dirfd`
but `AT_FDCWD`, so an `openat` relative to an opened directory fails"* —
and marks that clause *taken up*; the build strikes it. `pselect6`, the
first of the three, is built; `sysinfo`, the second, stays open and is
not this unit.

## Problem

Every `*at` system call in the Linux personality resolves a relative
path from one of two places: the process's current directory, when the
caller passes `AT_FDCWD`, or the root, when the path is absolute. A real
directory descriptor — the thing the `*at` family exists to accept — is
refused before it is looked at:

```c
/* Only AT_FDCWD is a directory handle the kernel can resolve from; a
 * real dirfd would need openat semantics the VFS does not offer yet. */
static int check_dirfd(int64_t dirfd, const char *path)
{
    if ((int)dirfd == LX_AT_FDCWD || path[0] == '/')
        return 0;
    return -ENOSYS;
}
```

(`compat/linux/syscalls.c`.) Nine calls go through it — `openat`,
`newfstatat`, `faccessat`, `readlinkat`, `symlinkat`, `mkdirat`,
`mknodat`, `unlinkat`, `renameat` — and `fchdir`, the call that makes a
directory descriptor the working directory, is not in the table at all.

The comment's premise is also out of date. The VFS has taken a starting
directory as its first argument since the working-directory work
(`vfs_open(start, path, …)`, and every sibling), and the cwd-hold unit
proved the rule that makes any such start safe to pass: the caller holds
a reference for the length of the walk (V35). A directory descriptor is
already a referenced directory. Nothing the door needs is missing except
the door.

### Measured

`tools/dirfd-probe.py`, shipped with this report, injects into `lxtest`
— at the point where it already holds a directory descriptor for
`/tmp/lxdir`, which contains `moved` — one call of each kind against that
descriptor with a relative path, each chosen so that a door that
resolved the descriptor would answer something other than `ENOSYS`:

| call | x86-64 | AArch64 |
| --- | --- | --- |
| `openat` | -38 | -38 |
| `newfstatat` | -38 | -38 |
| `faccessat` | -38 | -38 |
| `readlinkat` | -38 | -38 |
| `symlinkat` | -38 | -38 |
| `mkdirat` | -38 | -38 |
| `mknodat` | -38 | -38 |
| `unlinkat` | -38 | -38 |
| `renameat` | -38 | -38 |
| `fchdir` | -38 | -38 |

`-38` is `ENOSYS`. **Every call that can take a directory descriptor
refuses one**, on both architectures. (The AArch64 boot that carried the
probe failed on `net-nicbench` over its time budget, a load-sensitive
benchmark that runs minutes before `lxtest`; the probe's lines and
`LINUXTEST: PASS` were both printed.)

### Why it matters

- **Real programs use the `*at` family with real descriptors**, and not
  as an optimisation: it is how a program walks a tree without racing a
  rename of an ancestor. musl's `nftw`, `fts`-style walkers, `rm -r` in
  every coreutils and busybox, `find -delete`, and any program that
  opens a directory and then works inside it. On this system each of
  them fails the moment it descends, with an errno that says the call
  does not exist.
- **`ENOSYS` is the wrong answer.** A program reasonably treats
  `ENOSYS` as "this kernel lacks the call" and may fall back to a path
  string built by concatenation — which is exactly the ancestor-rename
  race the `*at` family exists to close.
- **The machinery is there.** Nothing below the door changes except
  `vfs_rename`, which takes one start for its two paths and needs two.

## Current implementation

**The walk.** Every VFS path entry point takes a `start` vnode:
`vfs_open`, `vfs_stat`, `vfs_lstat`, `vfs_mkdir`, `vfs_mknod`,
`vfs_unlink`, `vfs_rmdir`, `vfs_symlink`, `vfs_readlink`, `vfs_rename`.
A relative path begins at `start`; an absolute one at the calling
process's root, whatever `start` is. The caller holds a reference to
`start` for the call (V35).

**The door.** Each of the nine `*at` calls runs `check_dirfd`, then
takes `process_cwd_get()` and passes it as `start`. `openat` is thinner
still: it calls `do_open`, which re-reads the path and takes the cwd
itself, so a base cannot be threaded through without changing `do_open`.
`renameat` passes one `start` for both of its paths, because
`vfs_rename(start, oldpath, newpath)` has only one.

**Descriptors.** A directory opens as a file (`lxtest` already does,
for `getdents64`), and `O_DIRECTORY` refuses anything else. A file holds
a referenced vnode and nothing about the path it was opened by.

**The working directory** is a referenced vnode and a normalised path
string, published together under the process lock (P27); `getcwd`
answers the string. A directory descriptor has the vnode and not the
string, which is the one design question in this unit.

## Design

### 1. One resolver: a base for a descriptor

```c
/* The directory a relative path in an *at call resolves from, referenced:
 * the working directory for AT_FDCWD or an absolute path (which ignores
 * it), otherwise the directory the descriptor names. -EBADF if dirfd is
 * not an open file, -ENOTDIR if it is not a directory. The caller puts
 * it (V35: the reference covers the walk, whatever another thread does
 * to the descriptor meanwhile). */
static int at_base(int64_t dirfd, const char *path, struct vnode **out);
```

It replaces `check_dirfd` at all nine sites, and each passes its result
where it passes `process_cwd_get()` today. The descriptor is looked up
with no rights demanded, as Linux does — a directory opened read-only,
or for search only, is a valid base — and its vnode is referenced
before the handle's reference is dropped, so a sibling thread closing
the descriptor mid-call cannot free the base under the walk. That is the
cwd-ref rule applied to a second kind of base, and the held-walk seam
already covers it: it holds any relative walk at `walk_parent`, whatever
the base.

`do_open` gains a base argument; `lx_open` and `lx_creat` pass the cwd,
`lx_openat` passes `at_base`'s answer.

### 2. `renameat` with two bases

`vfs_rename2(ostart, oldpath, nstart, newpath)` — the two
`parent_for_mutation` calls `vfs_rename` already makes, each from its
own start. `vfs_rename(start, a, b)` becomes `vfs_rename2(start, a,
start, b)`, so the native door and every existing caller are unchanged.
The cross-mount (`-EXDEV`) and into-itself (`-EINVAL`) checks are made on
the two parents, as today, and do not care how they were reached.

### 3. `fchdir`, and the path it publishes

`fchdir(fd)` makes a directory descriptor the working directory. The
vnode is the descriptor's; the **path** has to come from somewhere,
because `getcwd` answers the string and the string is what the next
relative `chdir` normalises against (P27).

**A directory file records the normalised absolute path it was opened
by.** At open, when the result is a directory, the door that opened it
normalises the path against the base it resolved from — the cwd's path,
or the base descriptor's own recorded path — and stores it with the
file. `fchdir` publishes the vnode and that string together under the
process lock, exactly as `process_chdir` does. A directory opened with
no recorded path (none exists today; the rule is written for the future
case) is refused `-ENOENT` rather than given an invented name.

The limitation is P27's, stated there already: a renamed ancestor is not
noticed by the string. The vnode is authoritative for resolution and the
string for display, for a descriptor as for `chdir`.

Native opens record the path too, because a Linux program can receive a
native-opened directory descriptor across `spawn`'s handle inheritance
and `fchdir` into it.

### 4. The invariant

**P31. A relative path in an `*at` call resolves from the directory its
descriptor names, referenced for the walk.** `AT_FDCWD` names the
working directory; any other descriptor must be an open directory
(`-EBADF`, `-ENOTDIR`); `ENOSYS` is not an answer any `*at` call gives. A
directory file carries the path it was opened by, and `fchdir` publishes
it with the vnode as `chdir` does.

### 5. The §70 gate

**Correctness.** One resolver at nine sites, and one VFS change
(`vfs_rename2`) that the old entry point becomes a wrapper around.

**Concurrency.** The base is referenced before the handle's reference
is dropped (V35); a descriptor closed mid-call leaves the walk holding
its own reference. `fchdir` publishes under `p->lock` as `chdir` does and
puts the old directory outside it.

**Ownership and lifetime.** A directory file owns its recorded path and
frees it with the file.

**Security.** A descriptor grants nothing the path did not: every walk
still checks search permission on every component, and an absolute path
still starts at the process's root. A descriptor opened *before* a
process's root was narrowed still names what it named — as a Linux
`dirfd` does, and as the native handle inheritance rules already
account for (`docs/kernel/security/design.md`, handles across roots).

**Failure.** `-EBADF`, `-ENOTDIR`, and whatever the walk answers. No
`ENOSYS`.

**Performance.** One handle lookup per `*at` call with a real
descriptor, instead of a cwd lookup. Unmeasured by design: it replaces
work, it does not add it.

## Affected files

| file | change |
| --- | --- |
| `compat/linux/syscalls.c` | `at_base` replaces `check_dirfd` at nine sites; `do_open` takes a base; `renameat` uses `vfs_rename2`; `lx_fchdir` |
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_fchdir` (81, 50) |
| `kernel-services/vfs/vfs.c`, `kernel/include/kernel/vfs.h` | `vfs_rename2`; `vfs_rename` becomes its wrapper; `struct file` gains the recorded path of a directory, freed in `file_release` |
| `kernel/process/process.c`, `kernel/include/kernel/process.h` | `process_fchdir(vn, path)`, sharing `process_chdir`'s publish |
| `kernel/syscall/native.c` | the native open records a directory's path |
| `tests/linux/lxtest.c` | every `*at` call with a real descriptor; `fchdir` and `getcwd`; `-EBADF` and `-ENOTDIR`; a closed-then-used descriptor |
| `kernel-services/vfs/vfstest.c` | `vfs_rename2` across two starts |
| `docs/compat/linux/api.md`, `testing.md`; `docs/kernel/process/invariants.md` (**P31**); `docs/kernel-services/vfs/api.md` | as built |
| `docs/audit/2026-09-deferred-work-inventory.md` | §2.6's dirfd clause, *taken up* here and struck by the build |
| `README.md` | Status entry |
| `tools/dirfd-probe.py` | shipped with this report |

## APIs

### New

```c
/* kernel-services/vfs: a rename whose two paths resolve from two
 * starts, for renameat. vfs_rename(s, a, b) is vfs_rename2(s, a, s, b). */
int vfs_rename2(struct vnode *ostart, const char *oldpath, struct vnode *nstart, const char *newpath);

/* kernel/process: make `dir` (referenced by the caller, who keeps its
 * reference) the working directory with `path` as its name, published
 * together under the process lock. -ENOTDIR if it is not a directory,
 * -EACCES without search permission. */
int process_fchdir(struct vnode *dir, const char *path);
```

`LX_fchdir` in the Linux table. `at_base` is static to the door.

### Existing, relied on

`vfs_open` and every sibling's `start` argument; `handle_lookup` and
`file_from_kobject`; `path_normalize`; the V35 rule for any walk's base.

## Migration plan

1. **`at_base` and the nine sites**, with `lxtest`'s new block against a
   real descriptor. The probe's `ENOSYS` column must become the answers
   the calls exist to give.
2. **`vfs_rename2`**, with `vfstest`'s two-start case, then `renameat`
   on both architectures (`LX_renameat` is 264 on x86-64 and 38 on
   AArch64). `renameat2` is not in the table and stays out of scope.
3. **Directory paths and `fchdir`**, with `getcwd` after `fchdir`
   answering the descriptor's path, and a relative `chdir` after it
   normalising against that path.
4. Docs, inventory, README, the banner; release builds, `gmake
   host-test`, every mutation alone with the boot confirmed.

## Tests

| test | what it proves | bug-proof |
| --- | --- | --- |
| `lxtest`, directory descriptors | each of the nine calls resolves a relative path from a real descriptor: `openat` opens `moved`, `newfstatat` and `faccessat` find it, `mkdirat`/`unlinkat`/`mknodat`/`symlinkat`/`readlinkat` act inside the directory and the result is visible by absolute path, `renameat` across two descriptors | `at_base` answering the cwd for every descriptor: every call acts in the wrong directory and the absolute-path cross-check fails. The cross-check is what makes the test about *which* directory, not merely about a success |
| `lxtest`, refusals | a regular file as `dirfd` is `-ENOTDIR`; a closed descriptor is `-EBADF`; `AT_FDCWD` and absolute paths are unchanged | `at_base` accepting a regular file: the walk fails later with a different errno, and the check names it |
| `lxtest`, `fchdir` | after `fchdir(dfd)`, `getcwd` answers `/tmp/lxdir` and a relative `open` finds `moved`; a relative `chdir("..")` then answers `/tmp` | `fchdir` publishing the vnode without the path: `getcwd` answers the old directory while opens resolve in the new one — the published-together rule P27 exists for |
| `vfs-rename2` | a rename between two directories reached from two different starts; `-EXDEV` and `-EINVAL` still found on the parents | `vfs_rename2` passing `ostart` for both lookups: the new name lands in the wrong directory |

## Benchmarks

None: the unit replaces a cwd lookup with a descriptor lookup on a path
that already walks a directory. Stated rather than measured.

## Risks

- **The recorded path goes stale on a rename of an ancestor**, as
  `chdir`'s does (P27's gap). Resolution is by vnode and is right; only
  `getcwd`'s answer after `fchdir` can be stale, and only for a
  directory renamed after it was opened.
- **A descriptor survives a narrowing of the process's root.** Linux
  behaves the same, and this system's handle-inheritance rules already
  treat an inherited handle as a capability; the report names it rather
  than inventing a new restriction.
- **`renameat2` stays absent**, so a program that calls it — musl's
  `renameat` on AArch64 does not; glibc's may — still gets `ENOSYS`, now
  from the table, which is honest about the call being absent, unlike
  today's `ENOSYS` from calls that exist. Its flags (`RENAME_NOREPLACE`,
  `RENAME_EXCHANGE`) are semantics this VFS does not have and are their
  own unit.

## Alternatives considered

- **Resolve a descriptor by rebuilding its path string** and walking
  that from the root. It reintroduces the ancestor-rename race the `*at`
  family exists to close; the vnode is the correct base.
- **Compute `fchdir`'s path by walking `..` and searching each parent
  for the child's inode.** No dentry cache and no parent pointers make it
  a directory scan per level, and a mount crossing makes it wrong without
  more machinery. Recording the path at open is cheap and has the same
  staleness `chdir` already accepts.
- **Add `*at` calls to the native ABI in the same unit.** The native
  libc has no `openat` and no native program asks for one; the native
  door records directory paths (for `fchdir` of an inherited handle) and
  nothing more. A native `*at` family is a separate unit if a native
  program ever needs it.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_014qfcm8FQycZFpYeonUcCz2
