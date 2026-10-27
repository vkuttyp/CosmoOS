# Roadmap 1.2 A2, second PR: hard links and the native metadata calls (VFS-4, VFS-6), 2026-10-28

A2 (`docs/roadmap-1.2.md`): "Hard links (VFS-4) and a native `chmod`
(VFS-6), which build systems use, land here." Two backlog lines:

- **VFS-4** usability, M. No hard links (`link`/`linkat`).
- **VFS-6** usability, S. The native ABI has no `chmod`, `chown`,
  `chroot`, `utimes` or `umask`; the Linux umask is not applied to native
  creation.

## Scope and baseline

Branch `a2-links` from `a2-long-names` (#473) at `5fe2ab7f`: cosmofs's
`link` takes a long name the way a creation does (`names_allow`), so this
PR builds on the first and merges after it. New features, proven by their
tests; no probe (`AGENTS.md`).

## The design

`docs/kernel-services/vfs/design.md`, "Hard links";
`docs/kernel-services/filesystem/cosmofs/design.md`, "Hard links";
`docs/kernel/syscall/api.md`, rows 107-112; `docs/compat/linux/api.md`,
rows 86 and 265.

- **`vfs_link(ostart, oldpath, nstart, newpath, follow)`** and an optional
  `link` vnode operation. A directory is `-EPERM`; a final symbolic link is
  linked itself unless `follow`; two mounts are `-EXDEV`, checked before
  the new name's directory, as Linux does, then a writable mount, write and
  search permission, `-EEXIST`; a file whose last name is gone is
  `-ENOENT`; at `VFS_LINK_MAX` (65000) names, `-EMLINK` (a new errno, 31,
  in the kernel, the native ABI and libc).
- **A file dies with its last name.** `VNODE_DEAD` was set by every unlink
  and every rename's replace; it is now set only when the victim's
  `nlink` reaches zero (or it is a directory), so a file is still reached
  through its other names.
- **ramfs** keeps one pin per file, not one per name: `vfs_umount`'s busy
  check counts one, so a reference per name would have made a ramfs with
  a hard link impossible to unmount. Unlink and the unmount's teardown
  drop the pin with the last name; the teardown meets a file once per
  name, so it decrements rather than reads a flag the first meeting may
  have freed.
- **cosmofs** needed nothing on disk: an inode has `nlink`, unlink and a
  rename's replace already take one link of several, and the checker
  already counts every entry naming an inode. `cfs_link` adds an entry
  and a link in one transaction, under the tree's write lock; a snapshot
  is read-only to it either way and a clone is another tree.
- **Linux:** `linkat` (`AT_SYMLINK_FOLLOW`; `AT_EMPTY_PATH` leaves an
  empty path `-ENOENT`, as an unprivileged Linux; any other flag
  `-EINVAL`) and, on x86-64, `link`.
- **Native:** `SYS_link` 107 (not following, as POSIX `link`),
  `SYS_chmod` 108, `SYS_chown` 109 (`COSMO_ID_KEEP`), `SYS_utimes` 110
  (`COSMO_UTIME_NOW`), `SYS_umask` 111 and `SYS_chroot` 112, over the
  functions the Linux door already used (`vfs_setattr`, `vfs_chown`,
  `process_chroot`), with libc's `link`, `chmod`, `chown`, `umask` and
  `chroot`.
- **The umask on native creation.** `SYS_open` with `O_CREAT`,
  `SYS_mkdir` and `SYS_mknod` now take the process's mask, as Linux
  creation did. Only one native caller asked for group or other write: a
  root-owned world-writable file in `/tmp` for the sticky-bit test, which
  now `chmod`s it to `0666` after creating it.

## Tests

- `cosmofs-link` (kernel): one file with four names in two directories,
  one long (raising the pool to 15); `-EPERM` and `-EEXIST`; a link to a
  symbolic link is that link; names removed one by one, the file read
  through the rest; the count across a remount, checked clean; a snapshot
  read-only both ways; an open file outliving its last name, freed after
  it and checked clean.
- `links` (user mode, a new section, so `run_boot_test.py`'s list of
  sections names it): links on `/tmp`, a second ramfs mount `EXDEV` that
  still unmounts with a file of two names in it, the umask on native
  creation, `chmod`, `utimes` and `chown`. The unprivileged child: native
  `chroot` and a non-owner's `chmod` are `EPERM`.
- `lxtest`: `link`/`linkat`, `EEXIST`, `EPERM`, `EINVAL`, `EXDEV` (into
  `/proc`), a symbolic link with and without `AT_SYMLINK_FOLLOW`.

The first boot of the branch passed all three and failed on the harness:
the new section was not on its list.

## Validation (2026-10-28, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| debug `make test` (4 CPUs) | PASS (178.3 s) | PASS (224.9 s) |
| debug `QEMU_SMP=1 make test` | PASS (167.6 s) | PASS (198.7 s) |
| `make test-smp2` | PASS (176.5 s) | PASS (221.6 s) |
| `make test-chaos` | PASS (180.4 s) | PASS (186.4 s) |
| `make test-harness-retry` | PASS (182.2 s) | PASS (214.4 s) |
| `BUILD=release make test` | PASS (18.6 s) | PASS (21.0 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean: 7 baselined, 0 unexpected |
| `make host-test`, `make fuzz` | PASS, PASS | (host) |
