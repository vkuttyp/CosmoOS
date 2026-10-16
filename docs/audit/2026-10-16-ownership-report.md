# Ownership: S4 — 2026-10-16

The 1.1 roadmap's S4, the fourth unit of the shell-completeness track.
Base: main `e1a0e844` (S3 merged). Goal: files can change owner and
group, a link's own owner can be set, a process can take a new root, and
`su` works -- for root, the one user.

## Before

| What | Before |
|---|---|
| `chown`, `lchown`, `fchown`, `fchownat` | not in the Linux personality (`-ENOSYS`); no `vfs_chown` |
| `chroot` | not in the Linux personality; a root only at spawn (`COSMO_SPAWN_SETROOT`), fixed for the process's life and read without a lock |
| cosmofs links | no `setattr`; `inode_sync` would have written a link as a regular file |
| BusyBox `chown`, `chgrp`, `su` | not built |

## Decisions

**Who may change ownership.** Linux's rule with this kernel's privilege
in `CAP_CHOWN`'s place: a new owner needs `PRIV_SYS_ADMIN`, which the
security design already names for "overriding file permissions and
ownership"; the owner may give the node one of its own groups, and may
name the owner and group it already has. Any change asked for clears a
regular file's set-user-id bit, and its set-group-id bit when it is
group-executable, as Linux's `ATTR_KILL_SUID`/`ATTR_KILL_SGID` do.

**Who may chroot, and where it leaves the process.** `PRIV_MOUNT`, the
privilege that already gives a child a root. The working directory moves
to the new root with it -- Linux leaves it where it was, outside the new
root, where every relative path reaches out and `..` never meets the root.
The security design already refused that for a child given a root at
spawn ("a rooted child starts at its root"); `chroot` follows the same
rule, so the classic chroot escape cannot start. BusyBox's own `chroot`
and nearly every program `chdir("/")` straight after, so the difference
shows only to a program that relied on the escape.

**The root becomes mutable (P39).** Until now `root` was fixed at spawn
and `vfs_current_root` read it without a lock. `chroot` replaces it, with
the working directory, under `p->lock`, and puts the old ones after;
`vfs_current_root` takes its reference under the same lock, and procfs
reads another process's root there. A walk on another thread holds the
root it began with.

**`su`.** BusyBox's, unchanged: as root it asks no password, then
`setgroups`, `setgid`, `setuid` and the shell. To another uid that needs
`PRIV_CRED` (the existing credential rules); with `cred` dropped,
`setgroups` is refused and `su` says so.

## Changes

| Area | Change | Files |
|---|---|---|
| VFS | `vfs_chown` (`VFS_ID_KEEP` for "leave it") | `kernel/include/kernel/vfs.h`, `kernel-services/vfs/vfs.c` |
| cosmofs | links get `setattr`; `inode_sync` keeps a link's type and 0777 | `kernel-services/filesystem/cosmofs/cosmofs.c` |
| process | `process_chroot`; `vfs_current_root` under `p->lock` (P39) | `kernel/include/kernel/process.h`, `kernel/process/process.c` |
| procfs | `proc_same_root` reads another process's root under its lock | `kernel-services/filesystem/procfs/procfs.c` |
| Linux personality | `chown` and `lchown` (x86-64), `fchown`, `fchownat` (`AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`), `chroot` | `compat/linux/syscalls.c`, `compat/linux/nr_{x86_64,aarch64}.h` |
| BusyBox | `chown` (long options), `chgrp`, `su` (syslog, shell checks) | `ports/busybox/{applets,busybox.config,mkconfig.py}` |
| tests | `ash.sh`'s S4 section; `test-net-login` stage i and the check in e; `lxtest`'s ownership section | `tests/busybox/ash.sh`, `tests/boot/net_login_test.py`, `tests/linux/lxtest.c` |

## Tests

The acceptance, in CI on both architectures:

- **`test-busybox`** (job `busybox`): `chown 1000:1000` and `chgrp 0` read
  back by `stat`; `chown -h` changes a link and not its target, and
  `chown` through a link changes the target; `chown 0` of a 6755 file
  leaves 755; `chown` refused (`Operation not permitted`) to uid 1000 and
  to root with `sys_admin` dropped, the file unchanged; `su -c 'id -u'`
  is 0, `su` to a `tester` added for the test runs as 1000:1000, and with
  `cred` dropped `su` to it is refused. 165 checks in all.
- **`test-net-login`** (job `net-login`): stage i chowns a file on the
  installed system's cosmofs root to 1000:1000 and a link to it, with
  `-h`, to 1001:1001, and syncs; after the reboot (stage e) `stat` shows
  both owners.

In every debug boot, `lxtest`'s ownership section: `fchown`; `fchownat` by
path with -1 keeping the group, with `AT_SYMLINK_NOFOLLOW` (the link
changes, its target does not), following a link without it, and with
`AT_EMPTY_PATH` on the open file; an empty path without the flag
(`ENOENT`) and an unknown flag (`EINVAL`); `chroot` of a regular file
(`ENOTDIR`); a child's `chroot`, after which `/marker` opens, the same
file by its outer path is `ENOENT`, `../../marker` stops at the root and
`getcwd` is `/`, while the parent's root is unchanged; and a uid-1000
child refused `fchown`, `fchownat` and `chroot` (`EPERM`).

Checked against their bugs by mutation:

- cosmofs links without `setattr`: `test-net-login` fails at stage i, before
  the reboot even -- the link's vnode is evicted and reloaded from its
  unchanged inode.
- `chroot` leaving the working directory where it was: `lxtest`'s child
  fails its `../../marker` check (exit 5).

New features are proven by their acceptance test; no probe (AGENTS.md).

## Validation

MATRIX

## Recorded instead of done

Inventory §14: no native `chown`/`chroot`; `chown` on procfs or devfs
reads back only until the vnode is evicted (as `chmod`); `fchownat`
through a directory descriptor needs its write right (as `fchmodat`); no
`useradd`; supplementary groups only from `setgroups`.
