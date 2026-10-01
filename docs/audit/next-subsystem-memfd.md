# NEXT SUBSYSTEM — the Linux personality has no memfd_create

> Constitution §68 report. This PR adds the report and the probe
> (`tools/memfd-probe.py`); the anonymous-ramfs-file constructor, the
> `memfd_create` and `ftruncate` doors and the `lxtest` checks described under
> "Design" and the edits in "Affected files" are planned work that lands in the
> implementation PR that follows, gated on CI. As the report was committed
> (before PR #289), `memfd_create` returned `-ENOSYS`.
>
> **Built in PR #289.** The implementation landed `ramfs_anon_reg` in
> `kernel-services/vfs/ramfs.c`, `vfs_ftruncate` in `vfs.c`, and the
> `memfd_create`/`ftruncate` doors in `compat/linux/syscalls.c`, with the
> `lxtest` checks; both arches boot PASS and `host-test` passes. Every
> present-tense statement below describes the state the report measured,
> before this implementation.

## Problem

`memfd_create(2)` is unimplemented: it has no number in `compat/linux/nr_*.h`,
so it is not in `linux_table` and the dispatcher's `lx_unknown` returns
`-ENOSYS`. `memfd_create` makes an **anonymous file that lives in memory** — an
unnamed regular file a program sizes with `ftruncate`, `mmap`s, and passes to
another process as a shared buffer. It is how graphics stacks, allocators and
IPC libraries get shareable memory (it replaced the old `/dev/shm` dance). A
Linux program that reaches for it dies at creation.

The kernel already has the whole backing. `ramfs` mounts with **the page cache
as its store** (`MOUNT_CACHE_IS_STORE`): a regular ramfs file (`VNODE_REG`,
`ramfs_file_ops` in `kernel-services/vfs/ramfs.c`) keeps its bytes in the
vnode's embedded page cache (`struct pagecache pc`, `vfs.h`), with
`read_file`/`write_file`/`writepages`/`truncate` ops, and it is `mmap`-able
through the existing file-mapping path (`vm_user_map_file`, the file-regions
unit). A memfd is exactly an **unnamed** such file. What is missing is a way to
make one without a directory entry, and the `ftruncate` door to size it.

## Probe

`tools/memfd-probe.py` adds `LX_memfd_create` to both syscall-number headers
and one check to `tests/linux/lxtest.c` (which runs in the standard boot):

```
LXMEMFD: memfd_create unimplemented -> -ENOSYS; no anonymous memory file
```

The check asserts `memfd_create("probe", 0)` returned `-ENOSYS` at the report
commit, before the implementation PR. The marker prints only when the syscall
really returned `-ENOSYS`, so grepping it cannot show a false result after a
failed check.

## Why it matters

- **Shareable memory by fd.** `memfd_create` is the modern way to get a
  memory-backed file to `mmap` and hand to another process; without it the
  programs that use it (Wayland/graphics, allocators, sandboxes) fail at
  creation with `-ENOSYS`.
- **It is in pattern and the mechanism exists.** ramfs already keeps regular
  files in the page cache and already serves `mmap`; this unit adds an unnamed
  constructor and the `ftruncate` door, not a new filesystem or memory object.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| ramfs regular file | `ramfs_new` / `ramfs_file_ops` (`kernel-services/vfs/ramfs.c`) | a `VNODE_REG` whose bytes live in the vnode's page cache: `read_file`/`write_file`/`writepages`/`truncate` |
| typed node constructor | `ramfs_new(mnt, type, mode, parent)` (static); `ramfs_mkchr(path, …)` the public precedent | builds a typed, pinned ramfs vnode with the right ops |
| vnode → file → fd | `vfs_open_vnode(vn, flags, &f)` (`vfs.h`), then `handle_install(&proc->handles, &f->obj, rights)` (as `lx_openat` does) | wraps a vnode in a `struct file` and installs it as an fd |
| file mmap | `vm_user_map_file` / `vm_user_map_file_free` (`vmm.h`, the file-regions unit) | maps a file fd's page cache into a user address space |
| vnode resize | the `truncate` op (`vfs.h`), `ramfs_file_ops.truncate` | sets a regular file's size |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | `memfd_create` and `ftruncate` are unlisted → `-ENOSYS` |

## Design

### 1. An anonymous ramfs regular file

A small public constructor in `ramfs.c` (the `ramfs_mkchr` pattern, but
unnamed) builds a **born-unlinked** `VNODE_REG` via `ramfs_new(mnt, VNODE_REG,
mode, NULL)`: `ramfs_file_ops`, the embedded page cache, and no directory
entry, on the same ramfs mount the root already provides. Size starts at zero.

`ramfs_new` leaves a vnode `VNODE_PINNED` with `nlink = 1` and holds the
`vnode_alloc` reference as that pin — for a *named* file the pin is released by
`ramfs_unlink` (which clears `VNODE_PINNED`, drops `nlink`, and `vnode_put`s the
pin, "open files keep it alive", `ramfs.c`). An anonymous file has no name to
`unlink`, so the constructor must do the unlink's work up front: clear
`VNODE_PINNED` and set `nlink = 0`, leaving the single `vnode_alloc` reference
to be **consumed** by `vfs_open_vnode` into the `struct file` (as the exec path
relies on, `spawn.c`). The file then owns the one and only reference — there is
no retained pin — so closing the last fd drops it to zero and `ramfs_evict`
frees the vnode and its page-cache pages. This is the `O_TMPFILE` /
unlinked-but-open lifetime, exactly memfd's.

### 2. The Linux doors

- **`memfd_create(name, flags)`** — copy `name` from user with a bound (Linux
  caps it at 249 bytes; it is advisory — Linux uses it only for
  `/proc/self/fd` and accounting — so it is validated (readable, within the
  length limit) and otherwise ignored).
  `flags` must be a subset of `MFD_CLOEXEC` (`-EINVAL` otherwise):
  `MFD_ALLOW_SEALING` and `MFD_HUGETLB` are **rejected**, not silently
  accepted, so a program that depends on seals or hugepages gets a clear error
  at creation rather than a false success. Build the anonymous file, open it
  `O_RDWR` with `vfs_open_vnode`, and install it as a read+write fd. `MFD_CLOEXEC`
  is accepted and a no-op under the spawn model (as for `eventfd`/`pipe2`).
- **`ftruncate(fd, length)`** — resolve the fd to a `struct file`, and call the
  file's vnode `truncate` op under the vnode lock. A memfd starts empty, so a
  program sizes it with `ftruncate` before `mmap`; `ftruncate` is a general
  file door this unit also fills (unwired today). A negative length is
  `-EINVAL`; a non-truncatable vnode is `-EINVAL`.

The file then reads, writes, `mmap`s and resizes through the existing ramfs +
page-cache path with no new mechanism.

### 3. Lifetime

The anonymous vnode carries no pin (§1): its single reference is consumed by
`vfs_open_vnode` into the `struct file`, which holds it while the fd (or a
`dup`) is open; a file mapping holds a reference to the pages too. When the last
fd and mapping are gone the reference count reaches zero and `ramfs_evict` frees
the vnode and its page-cache pages — exactly an unlinked-but-open ramfs file.
The one subtlety the implementation must get right is that the constructor
clears `VNODE_PINNED` and the open consumes the reference, so there is no pin
left with no `unlink` to release it; otherwise the pages would never be freed.
No new lifetime mechanism beyond the vnode refcount the VFS already has.

## Affected files

| file | change |
|---|---|
| `kernel-services/vfs/ramfs.c`, a declaration in `vfs.h` | the anonymous `VNODE_REG` constructor |
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_memfd_create` (319 / 279), `LX_ftruncate` (77 / 46) |
| `compat/linux/linux_abi.h` | `MFD_CLOEXEC`/`MFD_ALLOW_SEALING`/`MFD_HUGETLB` |
| `compat/linux/syscalls.c` | `lx_memfd_create` + `lx_ftruncate` + the two table entries |
| `tests/linux/lxtest.c` | the memfd checks |
| `README.md` | Status entry |

## APIs

The Linux `memfd_create(2)` and `ftruncate(2)` system calls. No native ABI
change: the object is an ordinary ramfs regular file, reachable natively
through the VFS already; the gap this closes is the Linux one. `MFD_CLOEXEC` is
supported; `MFD_ALLOW_SEALING` and `MFD_HUGETLB` are rejected.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `memfd` basic | `memfd_create("m", MFD_CLOEXEC)` returns an fd; `ftruncate` to 4096 sets the size (`fstat`); a `pwrite` of a pattern reads back with `pread` |
| `memfd` mmap | after `ftruncate`, `mmap(MAP_SHARED)` the fd; a store through the mapping is visible via `pread`, and bytes written with `pwrite` are visible through the mapping — the file is memory-backed and shareable |
| `memfd` independent | two `memfd_create` calls return distinct files (a write to one is not seen in the other) |
| `memfd` lifetime | a loop of create + `ftruncate(a page)` + write + `close`, many times, does not leak: the file and its pages are freed on close. Asserted against a kernel vnode/page-cache accounting count if one is reachable (the honest form — a leak needs a counting test); otherwise it at least establishes close does not strand an fd or panic |
| `memfd` errors | `MFD_ALLOW_SEALING` and an unknown flag are `-EINVAL`; `ftruncate` with a negative length is `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- the constructor making a non-`VNODE_REG` node: the `pwrite`/`mmap` path fails.
- `memfd_create` installing the handle without the write right: the `pwrite`
  returns `-EBADF`.
- `ftruncate` not calling the `truncate` op: the size stays 0 and the `fstat`
  (and the `mmap` read) check fails.
- the constructor leaving `VNODE_PINNED` set (not doing the unlink's work): the
  lifetime loop leaks a vnode and its pages each iteration — caught by the
  accounting count the lifetime test watches.

## Benchmarks

None.

## Risks

- **No file sealing.** `MFD_ALLOW_SEALING` and the `F_ADD_SEALS`/`F_GET_SEALS`
  `fcntl`s are out of scope; the flag is rejected so a program that needs seals
  fails loudly at creation rather than trusting an unsealed file. A later unit
  can add sealing on this foundation.
- **No hugepages.** `MFD_HUGETLB` is rejected; this kernel has no hugepage
  backing.
- **`ftruncate` is wired for the general case.** It resolves any fd and calls
  the vnode `truncate` op, so it works for every truncatable file, not only
  memfds; a vnode without the op (a device, a pipe) gets `-EINVAL`, matching
  Linux's `-EINVAL`/`-ESPIPE` family for the un-truncatable.

## Alternatives considered

- **`mremap` instead.** The VM layer has no resize or move primitive
  (`vmm.h` creates, splits, protects and unmaps regions but cannot grow or move
  one), so `mremap` would need new VM infrastructure — unbounded for a compat
  unit. memfd reuses mature infrastructure untouched.
- **`signalfd` instead.** The signal subsystem stores pending signals as a
  bitmask (one `siginfo` per number), not the multi-instance queue a signalfd
  reads, and nothing wakes a waiter on arrival; it would need core signal
  surgery. Out of pattern.
- **A dedicated anonymous-memory object rather than a ramfs file.** ramfs
  regular files already are page-cache-backed and `mmap`-able; a second object
  would duplicate the page-cache file path. Reusing ramfs is why this is small.
- **Supporting `name` as a real path.** Linux memfds are anonymous; the name is
  advisory. Making it a real file would pull in directory placement and
  collisions for no gain, so the name is validated and then ignored.
