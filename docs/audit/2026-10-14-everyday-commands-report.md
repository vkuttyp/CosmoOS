# Everyday commands: an editor, a pager, what the machine is doing — 2026-10-14

The first of four units the owner chose after asking "what essentials are
we missing" over ssh on v1.0.2 (2026-10-13): everyday commands, then scp
and sftp, then `ping`, then HTTPS. Base: `fbcon-scale` 16e8d287 (the
v1.0.2 console, RSA keys and file times, PR #380), itself on main
`74537ed4`. Goal: the commands a shell session reaches for work as on
Linux, reading what they read on Linux.

## The commands, and what each needed

| Applet | Reads or calls | Before this unit |
|---|---|---|
| `vi`, `less`, `clear` | the terminal (termios, window size) | applet not built |
| `md5sum`, `dd`, `cmp`, `hexdump`, `du` | files only | applet not built |
| `whoami` | `/etc/passwd` | not on the live image |
| `free` | `sysinfo` | applet not built |
| `uptime` | `sysinfo` (uptime, load averages) | loads always 0 |
| `df` | `/proc/mounts`, `statfs` | neither existed |
| `top` | `/proc/meminfo`, `/proc/stat`, `/proc/loadavg`, `/proc/<pid>/stat`, `/proc/<pid>/cmdline` | none existed |

## Changes

| Area | Change | Files |
|---|---|---|
| shell | cosmo-sh lexes each pipeline just before it runs, so `$?` and `$X` see the commands before them on the line (`false; echo $?` printed 0) | `userland/shell/sh.c` |
| load averages | sampled on CPU 0's tick, Linux's `calc_load` (5 s period, `FSHIFT` 11, at most 64 missed periods caught up); no lock and no atomic read-modify-write on the tick; `sysinfo` reports them | `kernel/scheduler/sched.c` |
| process | the command line (256 bytes, at create, fork and exec) and the start time | `kernel/process/process.c` |
| `statfs` | `statfs`/`fstatfs` (x86-64 137/138, AArch64 43/44), Linux's 120-byte struct; an optional `fs_type` op and magic; cosmofs reports its blocks less its reserve, a mount with a cache budget reports that | `compat/linux/syscalls.c`, `kernel-services/vfs/vfs.c`, `cosmofs_core.c` |
| procfs | `meminfo`, `stat`, `loadavg`, `uptime`, `mounts` at the root and `stat`, `cmdline` per process, in Linux's format (P4) | `kernel-services/filesystem/procfs/procfs.c` |
| `/proc/mounts` | rendered by `vfs_mounts_render` under `g_mount_list_lock`, a new spinlock leaf (V7) | `kernel-services/vfs/fsctl.c`, `vfs.c` |
| applets | 13 applets; `/etc/passwd`, `/etc/group` on the live image | `ports/busybox/`, `userland/etc/` |

procfs's design said it would not mimic Linux's system files. P4 reverses
that for a fixed list: Linux programs read those paths, and their format
is what they parse. The list is closed; a new file is a design change.

## Found on the way

- **A lock-order inversion, caught on the first AArch64 boot.** procfs
  renders a file in its lookup, which the walk calls with the directory's
  vnode lock held; `/proc/mounts` then took `g_mounts_lock`, which mount
  takes before the mountpoint's vnode lock. lockdep panicked init. A
  visible namespace's mount list and `g_root_mount` now also change under
  `g_mount_list_lock`, a leaf, and the render takes only that.
  `switch_root` frees the dropped records after releasing it, so nothing
  is taken under it.
- **`lxtest` asserted the load averages were zero**, which pinned their
  absence. It now requires them written (not the poison) and under 1024.
- **`vi` through a pipe** asks the terminal for its cursor position when
  it has no window size, and reads the answer from stdin, eating the first
  keystroke; and after an ESC it reads on until 50 ms pass with no input,
  which a pipe at end of file answers with EOF. Linux behaves the same.
  The test sets `$LINES`/`$COLUMNS` and keeps the writer open past the
  ESC.

## Tests

- `tests/busybox/ash.sh`: 80 `expect` lines (62 on main; the harness
  counts 91 checks): each new applet, `/proc/mounts`, `/proc/$$/cmdline`,
  `/proc/$$/stat`, `top -b -n 1`'s memory, load and own lines, `df /`,
  and `vi` writing a file.
- BusyBox's testsuite: the five cases excluded for want of `dd`, `md5sum`
  or `cmp` run and pass (`tail` 4, `xargs` 12, `tar` 22 pass); `suite.sh`'s
  `cmp` stand-in is gone.
- init's procfs self-test reads every new file and checks its shape.
- `rc.test`: `false; cosmo-sh -c "exit $?"` and `Z=7; cosmo-sh -c "exit $Z"`.
  `tools/sh-status-probe.py --old` reverses the lexing change: rc.test
  then prints `status 0` and `SHTEST: FAIL`.

## Validation

Local, on the branch head: debug `make test` and `make test-busybox` pass
on x86-64 and AArch64 (scripted test 91 checks), release images build on
both. The full matrix and the probe's results are added before the PR.

## Not done here

- `/proc/<pid>/stat`'s VSZ and RSS read 0, so `top` shows no process
  memory: reading another process's space needs a reference on it, and
  exit drops the space without `p->lock` (P34). In the inventory.
- No `/sys`; no other `/proc` system file (`cpuinfo`, `version`,
  `<pid>/environ`, `<pid>/maps`).
