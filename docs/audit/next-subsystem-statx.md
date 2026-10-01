# NEXT SUBSYSTEM — the Linux personality has no statx

> **Built in PR #283.** `LX_statx` (332 / 291) + `lx_statx`, which resolves
> exactly as `lx_newfstatat` does and marshals a 256-byte `struct statx`
> through `lx_statx_from_native`; `stx_mask` reports the supported set
> (type/mode/nlink/uid/gid/ino/size/blocks/blksize + mtime/ctime) and omits
> `STATX_ATIME`/`STATX_BTIME`, which the kernel does not keep. The rest of this
> document is the §68 report as written for PR #282, before the
> implementation — read its "Design", "Affected files" and "Tests" in that
> light.
>
> Constitution §68 report. This PR adds the report and the probe
> (`tools/statx-probe.py`); `LX_statx`, the `struct statx` marshaller and the
> `lx_statx` handler described under "Design" and the edits in "Affected
> files" are planned work that lands in the implementation PR that follows,
> gated on CI. As committed here, `statx` returns `-ENOSYS`.

## Problem

The Linux personality implements `fstat`, `newfstatat`, `stat` and `lstat`
(`compat/linux/syscalls.c`), all of which marshal a `struct cosmo_stat` from
the VFS into a `struct lx_stat` through `lx_stat_from_native`. It does **not**
implement `statx(2)` (x86-64 332, AArch64 291): the number is unlisted in
`linux_table`, so the dispatcher routes it to `lx_unknown`, which returns
`-ENOSYS`.

That is the one modern stat call. glibc's `fstatat`/`stat` wrappers issue
`statx` first and only fall back on `-ENOSYS`; a statically linked musl
program or anything that calls `statx` directly for its extra fields
(`STATX_BTIME`, `stx_blksize`, the explicit `stx_mask`) gets `-ENOSYS` where
`fstat` would have succeeded. The information `statx` returns for the fields
this kernel tracks is exactly what `fstat`/`newfstatat` already produce from
the same `cosmo_stat`; what is missing is the `struct statx` marshalling, the
`mask`, and the call's own flag handling.

Prompt #2 §30 / the inventory's Linux-compat row names it:
`docs/audit/2026-09-deferred-work-inventory.md` §2.6, "missing (re-checked
2026-09-21): … statx …".

### Measured

`tools/statx-probe.py` adds `LX_statx`, a 5-argument syscall wrapper (`sc5`)
and a check to the Linux raw-ABI test `tests/linux/lxtest.c`, which runs in
the standard boot (one debug boot, x86-64):

```
LXSTATX: statx unimplemented -> -ENOSYS; no struct statx marshaller
```

The check asserts `statx(AT_FDCWD, "/tmp/lxtest.txt", 0, mask, buf)` returns
`-ENOSYS`, beside the `fstat`/`newfstatat` calls on the same file that
succeed — the gap is exactly the one call.

## Why it matters

- **The modern stat call is absent.** Current glibc prefers `statx`; a
  program built against it works only because of the `-ENOSYS` fallback, and
  a program that needs a `statx`-only field has no fallback. The kernel
  already has every field `statx` reports for its own files.
- **It is in pattern and cheap.** `statx` reuses the exact VFS stat path the
  other four stat calls use; it is a struct-marshalling and flag unit, not new
  filesystem logic — the same shape as the `dirfd` and `newfstatat` work.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| stat marshaller | `lx_stat_from_native` → `struct lx_stat` (`syscalls.c`, `stat_out`) | native `cosmo_stat` → the old `stat` ABI |
| path+fd stat | `lx_newfstatat` (`syscalls.c:541`) | `at_base` dirfd+path resolution, `AT_EMPTY_PATH`, `AT_SYMLINK_NOFOLLOW` → `vfs_stat`/`vfs_lstat` |
| dispatch | `linux_table[LX_NR_MAX]` (`syscalls.c:2707`); unlisted → `lx_unknown` | `statx` is unlisted → `-ENOSYS` |

## Design

### 1. The syscall

`LX_statx` (332 / 291) is added to `nr_x86_64.h` and `nr_aarch64.h`, and
`[LX_statx] = lx_statx` to `linux_table`. `lx_statx(dirfd, path, flags, mask,
ubuf)` resolves the target exactly as `lx_newfstatat` does — `at_base` for the
dirfd+path, `AT_EMPTY_PATH` to stat the fd itself, `AT_SYMLINK_NOFOLLOW`
choosing `vfs_lstat` over `vfs_stat` — into a `struct cosmo_stat`, then
marshals that into a `struct statx` and copies it out. The `AT_STATX_*SYNC`
flags are accepted and ignored: this kernel's attributes are always current,
so "force sync" and "don't sync" return the same answer.

### 2. The marshaller and the mask

A `struct statx` (the Linux ABI layout, with `STATX_*` mask bits) is added to
a compat header and the test's `lxabi.h`. The marshaller fills the fields the
kernel tracks — type and mode, nlink, uid/gid, inode, size, blocks, the
block size, and the **mtime and ctime** timestamps — and sets `stx_mask` to
**exactly** those it filled. `statx` is explicit that a field is meaningful
only when its bit is set in `stx_mask`, so a field the kernel does not keep is
left out of the mask rather than reported as a value it never recorded:

- `STATX_BTIME` (creation time) — not tracked.
- `STATX_ATIME` (access time) — **not tracked either**. `struct cosmo_stat`
  has only `mtime_ns` and `ctime_ns`; the old `lx_stat` marshaller copies the
  mtime into the `st_atime` slot (`convert.c`, `out->st_atime = out->st_mtime`)
  because that ABI has no way to say "no atime". `statx` does have that way —
  omitting the bit — so it must not claim `STATX_ATIME` for a time the kernel
  never recorded.

`stx_blksize` and `stx_attributes` are filled as the ABI requires.

### 3. Resolution and flags

Resolution reuses `lx_newfstatat`'s code path rather than a second copy, so a
dirfd, a relative path, an empty path with `AT_EMPTY_PATH`, and a symlink with
`AT_SYMLINK_NOFOLLOW` all behave as they do for `newfstatat`. The `mask` the
caller requests is advisory in Linux (the kernel may return more or fewer
bits); this implementation returns the fixed set it supports and reports it in
`stx_mask`, which is conformant.

## Affected files

| file | change |
|---|---|
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_statx` (332 / 291) |
| `compat/linux/<abi header>` | `struct statx`, `STATX_*` mask bits |
| `compat/linux/syscalls.c` | `lx_statx` + the marshaller; `[LX_statx]` in the table |
| `tests/linux/lxabi.h` | `struct lx_statx`, the mask bits, `sc5`, `LX_statx` |
| `tests/linux/lxtest.c` | `statx` checked against `fstat` on the same file |
| `README.md` | Status entry |

## APIs

The Linux `statx(2)` system call, in both personalities' tables. No native
ABI change. No new kernel subsystem — it rides the VFS stat path the other
stat calls already use.

## Tests

Planned for the implementation.

| test | proves |
|---|---|
| `lxtest` statx | `statx` on the test file returns 0 and its `stx_mode`/`stx_size`/`stx_nlink` equal what `fstat` returned for the same file; `stx_mask` has the expected bits and neither `STATX_BTIME` nor `STATX_ATIME` (the kernel keeps neither); the `AT_EMPTY_PATH` form on an open fd and `AT_SYMLINK_NOFOLLOW` on a symlink match `newfstatat` |

**Planned mutations** (each alone, boot confirmed):
- the marshaller leaving `stx_size` zero: the field-equality check against
  `fstat` fails.
- `stx_mask` not set for a filled field: the mask assertion fails.
- `AT_SYMLINK_NOFOLLOW` dropped: `statx` on a symlink returns the target's
  size, not the link's, and the check fails.

## Benchmarks

None.

## Risks

- **ABI layout.** `struct statx` must match the Linux ABI offsets exactly, on
  both architectures; a `STATIC_ASSERT` on its size (as `lx_stat` has) guards
  it, and the test compares concrete fields.
- **Honest mask.** The one semantic subtlety is that `stx_mask` must report
  only fields actually filled; a field the kernel does not track is left out,
  not zeroed-and-claimed.

## Alternatives considered

- **Alias `statx` to `newfstatat`'s marshaller.** The output structs differ
  (statx has the mask and extra fields), so a shared path can produce the
  `cosmo_stat` but not the final struct; the marshaller is statx-specific.
- **Report every `STATX_*` bit including `STATX_BTIME`/`STATX_ATIME` as zero (or atime-as-mtime).** Wrong: a
  caller reads a field only when its mask bit is set, and claiming a creation
  time the kernel does not keep would be a lie the mask exists to prevent.
