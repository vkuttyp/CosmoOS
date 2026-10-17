# Mounting: S6 — 2026-10-17

The 1.1 roadmap's S6, the sixth unit of the shell-completeness track.
Base: main `71fe9d3e` (S5 merged). Goal: filesystems mounted from a
shell with the commands a Linux user types, a second disk mounted at
every boot from `/etc/fstab`, and the disks and their filesystems named.

## Before

| What | Before |
|---|---|
| `mount(2)`, `umount2(2)` | not in the Linux personality (`-ENOSYS`); only the native `SYS_mount` and `SYS_umount`, through `/sbin/mount SOURCE DIR TYPE` |
| BusyBox `mount`, `umount` | not built |
| `/etc/fstab`, `mount -a` | none; nothing but the root was mounted at boot |
| `lsblk`, `blkid` | none; `cosmo-install --list` printed `/dev/blkctl`'s list in its own format |
| Making a cosmofs | the installer alone, through `/dev/blkctl` `FORMAT`; no user command |
| A cosmofs name | none: the superblock had a pool uuid and no label |

## Decisions

**BusyBox for `mount` and `umount`, native `lsblk` and `blkid`** (the
owner's choice, 2026-10-17). BusyBox 1.37 has no `lsblk`, and its
`blkid` prints `LABEL`, `UUID` and `TYPE` only -- no `PARTUUID` -- from
`volume_id`, which has no cosmofs probe and opens `/dev` block nodes the
kernel does not make. `/sbin/lsblk` and `/sbin/blkid` read `/dev/blkctl`
and print util-linux's formats, as S1's native `ip` prints iproute2's.
`lsblk` leaves out `MAJ:MIN` and `RM`.

**A cosmofs label, without a format version** (the owner chose a label
in the filesystem over the GPT partition name). It takes two of
`cfs_super`'s reserved words (`label[16]` at offset 136; one word stays
reserved; `crc` has not moved). The version is not raised: every format
this tree has written zeroes those words, so an older filesystem reads
as unnamed; nothing depends on the bytes; and a kernel that predates the
field copies the superblock whole at each commit, so a named filesystem
still mounts on v1.0.x and keeps its name. `/dev/blkctl` `FORMAT` carries
the label as the data after the struct (up to 16 printable characters,
no blank, `"` or `\`), `vfs_format` and `fs_type.format` pass it, and
`cosmofs_format_label` writes it. The installer names the root
`cosmo-root`; `mkfs.cosmofs [-L LABEL] DEVICE` is the user's way to make
one.

**One body, two doors.** The native calls' work moved into
`syscall_mount` and `syscall_umount` (`kernel/syscall/native.c`):
`PRIV_MOUNT`, then the target from the caller's directory (P27), named as
the walk reached it (P32). The Linux door translates and calls them, so
neither door can mount what the other cannot.

**The Linux door's translation.** `/dev/NAME` is the block device `NAME`
-- what `/proc/mounts` already printed -- though `/dev` has no block
nodes; a `/dev/NAME` with no device, or any other path, is `-ENOENT`; a
source without a slash is the label a device-less filesystem ignores.
`proc` is procfs. `MS_RDONLY` is the one flag; `MS_SILENT` (BusyBox sets
it always) and the old `MS_MGC_VAL` marker are ignored; every other flag
and any option string is `-EINVAL`. `umount2` takes `MNT_FORCE`.

**`/sbin/mount` stays.** BusyBox's `/bin/mount` comes first in `PATH`;
the native one keeps its `SOURCE DIR TYPE` syntax for the scripts that
name `/sbin/mount` (`tests/security/priv.sh`, init's namespace check,
`rc.test`'s snapshot mount). The callers that ran a bare `mount` with the
native syntax were changed: `ash.sh` and `install_test.py` to BusyBox's,
init and `rc.test` to `/sbin/mount`.

**`mount -a` before the services.** `/etc/rc` runs it after its banner;
the image's `/etc/fstab` lists nothing (the root is init's), and the
installer copies it. BusyBox's `FEATURE_MOUNT_LABEL`, loop, NFS, CIFS,
helpers and mtab support are off (`mkconfig.py` `FORCE`).

## Changes

- Kernel: `compat/linux/syscalls.c` (`lx_mount`, `lx_umount2`; numbers
  165/166 on x86-64, 40/39 on AArch64), `kernel/syscall/native.c`
  (`syscall_mount`, `syscall_umount`), `kernel/include/kernel/syscall.h`.
- cosmofs and the VFS: `cosmofs_format.h` (`label`, `CFS_LABEL_MAX`),
  `cosmofs_core.c` (`format_at`'s label, `cosmofs_format_label` as the
  type's `format`), `vfs.h`/`vfs.c` (`format(bdev, label)`,
  `vfs_format(..., label)`), `blkctl.c` and `uapi/cosmo/blkctl.h`
  (`FORMAT`'s label, `COSMO_BLKCTL_LABEL`).
- Userland: `userland/system/lsblk.c`, `blkid.c`, `mkfs.cosmofs.c` and
  their shared `blkdevs.c`; `cosmo-install.c` (`cosmo-root`);
  `userland/etc/fstab`, `userland/etc/rc` (`mount -a`); `userland.mk`
  (the three programs, `etc/fstab`, a shared source compiled once).
- BusyBox: `mount` and `umount` in `applets`, `mkconfig.py` and
  `busybox.config`.
- Docs: compat design ("Mounting"), userland design ("Mounting", the
  installer's step 5) and API, VFS API and design (`FORMAT`), cosmofs
  design (the superblock) and API, security design (`PRIV_MOUNT`), the
  privileges guide's example, inventory §16.

## Tests

The acceptance, in CI on both architectures:

- **`test-net-login`** (job `net-login`) stage k, on the net boot, which
  now has a second, blank 16 MiB disk (the same file on the reboot): the
  root's device from `/proc/mounts`; `lsblk` shows it as its disk's last
  partition, mounted at `/`, after the ESP, and one 16 MiB disk beside;
  `blkid /dev/ROOT` prints `LABEL="cosmo-root"`, a uuid,
  `TYPE="cosmofs"` and the `PARTUUID` the installer reported;
  `mkfs.cosmofs -L s6data` on the blank disk, a line in `/etc/fstab` and
  `mount -a` mount it at `/data` (`/proc/mounts`, `lsblk`, `blkid`'s new
  label); `umount /data` with `mount` dropped fails and `/data` stays; a
  file is written and synced. On the reboot boot (e), before anything
  else, `/data` is mounted (by `/etc/rc`'s `mount -a`) and holds the
  file. The disk names differ by machine and are found, not assumed: on
  x86-64 the installed disk is the SATA `ahci0p0` and the blank one
  `vda`; on AArch64 `vda` and `vdb`.
- **`test-busybox`** (job `busybox`), 19 checks more (195): a ramfs
  mounted, written and listed by `mount`; a procfs mounted and read; a
  read-only ramfs refusing a create; `mount` and `umount` refused with
  `mount` dropped, nothing mounted or unmounted; `-o remount` "Invalid
  argument", an unknown type "No such device", a missing `/dev` device
  "No such file or directory"; each unmounted and gone. On the NVMe
  scratch disk: `mkfs.cosmofs -L s6scratch`, `blkid`'s label, a file
  surviving an unmount and a read-only remount, `lsblk` showing the
  mount point, `mkfs.cosmofs` refused on the mounted disk, a 17-character
  label refused.

In every debug boot, `lxtest`'s mount section: a ramfs mounted, written
and unmounted (the file gone with it); `MS_RDONLY` with `MS_SILENT` and
`MS_MGC_VAL` gives `EROFS` on a create; `MNT_DETACH`, `MS_REMOUNT`,
`MS_NOSUID` and an option string are `EINVAL`; `/dev/nosuch` and a
path outside `/dev` are `ENOENT`, an unknown type `ENODEV`; procfs by
`proc` with no source; uid 1000 refused both calls. The host test pins
the superblock offsets `blkid` reads.

### Mutations

Each in a throwaway worktree from `4c0d72cc`, x86-64 release:

| Mutation | Caught by |
|---|---|
| M1: `/etc/rc` without `mount -a` | `test-net-login`: "/data mounted by /etc/fstab after the reboot" |
| M2: the label not written (`format_at`) | `test-net-login`: `blkid`'s root line (no `LABEL="cosmo-root"`) |
| M3: no `PRIV_MOUNT` check in the Linux door or the shared body | `test-busybox`: `mount-noprv`, `umount-noprv` and the checks after them |

## Validation

- **The validation matrix**, on `4c0d72cc`, every step PASS on the first
  run; the boot times are the harness's own lines:

  | Step | x86-64 | AArch64 |
  |---|---|---|
  | `host-test`, `fuzz`, `analyze` | pass | pass |
  | debug `test`, `QEMU_SMP=1` / `2` / `4` | 146.3 / 155.2 / 147.7 s | 140.7 / 149.2 / 160.6 s |
  | `test-smp2` | 154.2 s | 158.6 s |
  | `test-chaos` | 157.3 s | 156.5 s |
  | `test-harness-retry` | 154.7 s | 161.1 s |
  | `BUILD=release test` | 18.1 s | 21.5 s |
  | `BUILD=release test-busybox` | 48.7 s (195 checks) | 55.9 s (195 checks) |
  | `BUILD=release test-net-login` | 78.8 s | 92.4 s |
  | `BUILD=release test-install` | 25.3 s | 40.6 s |

  The network harness passed in every debug boot (the boot test requires
  it). `analyze`: clean, 7 baselined diagnostics, 0 unexpected.
- **Branch CI**: recorded in the PR.
