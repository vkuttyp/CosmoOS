# CosmoOS 1.1 roadmap

Written 2026-10-15, after v1.0.3. The [1.0 roadmap](roadmap-1.0.md) is
complete (M1-M6, v1.0.0 on 2026-10-12). This document records what was
merged after 1.0, the gaps recorded since, and the units proposed next.
The project owner picks the next unit from the proposed list, and
[`AGENTS.md`](../AGENTS.md#scope-rules)'s scope rules then apply to it as
they did to a 1.0 milestone. P1 is done; the owner chose the
[shell-completeness track](#s-shell-completeness) on 2026-10-15, S1 and
then S2 first.
[`plan.md`](plan.md) and the
[deferred-work inventory](audit/2026-09-deferred-work-inventory.md)
remain the record of everything else.

## Merged after 1.0

The post-1.0 queue the owner set on 2026-10-13/14, in the order it
merged. Each entry's detail is in
[`history/subsystem-units.md`](history/subsystem-units.md) and its report.

| Unit | PRs | Release | Acceptance in CI | Report |
|---|---|---|---|---|
| AArch64 platform discovery: the console, GIC, timer and PSCI from ACPI; Parallels Desktop boots to the live shell | #376 | v1.0.1 | `make test-platforms` (six QEMU variants, job `platforms`) | [acpi-discovery](audit/2026-10-13-acpi-discovery-report.md) |
| Parallels devices: firmware seed and time (boot protocol v10), timing-jitter entropy source, one-vector NVMe, legacy virtio transport, mutex handoff (S33) | #378, #379 | v1.0.2 (draft), v1.0.3 | `test-entropy` one boot per source; `mutex-handoff` | [parallels-devices](audit/2026-10-13-parallels-devices-report.md) |
| A readable screen console (8x16 face), RSA login keys, wall-clock file times | #380 | v1.0.2 (draft), v1.0.3 | `fb-console`; `test-net-login` RSA login; `lxtest` file times | [console-ssh-times](audit/2026-10-13-console-ssh-times-report.md) |
| Everyday commands: `vi`, `less`, `top`, `free`, `df`, `du`, `uptime`, `whoami`, `clear`, `md5sum`, `dd`, `cmp`, `hexdump`; procfs system files | #381 | v1.0.3 | `test-busybox` | [everyday-commands](audit/2026-10-14-everyday-commands-report.md) |
| `ping`: raw ICMP sockets over IPv4 (N31), `ITIMER_REAL` | #382 | v1.0.3 | `net-raw-icmp`; the networking guide pings the gateway | [ping](audit/2026-10-14-ping-report.md) |
| `scp` and `sftp`: dropbear's `scp`, OpenSSH's `sftp-server`; two scheduler fixes (S34, `poll-timer-outlives`) | #383 | v1.0.3 | `test-net-login` copies 256 KiB each way and runs an `sftp` batch | [scp-sftp](audit/2026-10-14-scp-sftp-report.md) |
| HTTPS for `wget`: mbedTLS 3.6.7 `ssl_client`, certificates checked against Mozilla's CA list | #384 | v1.0.3 | `test-net-login` serves good, untrusted, expired and misnamed certificates | [https](audit/2026-10-14-https-report.md) |
| virtio-gpu: a 2D driver; the screen console at the display's size, following a resize | #387 | after v1.0.3 | `make test-display` (a step of job `net-login`) | [virtio-gpu](audit/2026-10-14-virtio-gpu-report.md) |

Also merged: two test-budget records (#377, the AArch64 `blk` IRQ
budget; #385, the AArch64 entropy boots near 240 s) and #386 (the guide
names v1.0.3). v1.0.2 (tag at `7a87291b`, through #380) was published and
then returned to a draft; [v1.0.3](https://github.com/vkuttyp/CosmoOS/releases/tag/v1.0.3)
(`6011a5ec`, through #386, published 2026-10-15) replaces it and is the
first published release with the Parallels install, network
and ssh.

## Recorded since 1.0, not done

Open lines added to the
[inventory](audit/2026-09-deferred-work-inventory.md) since v1.0.0, by
area. None is scheduled; a proposed unit below names the ones it closes.

**Parallels Desktop and AArch64 platforms**

- SATA disks are not seen: Parallels' AHCI is a platform device described
  only in the DSDT, which needs an AML interpreter.
- ~~No power-off off QEMU: `arch_emulator_exit` is semihosting only and
  nothing calls PSCI `SYSTEM_OFF`, so `exit` halts and the VM stays
  running.~~ Closed by P1.
- The debug self-test suite assumes QEMU's machine (`pci`,
  `net-firewall`, `net-tap-release-order`, `blk-submit-unregister` fail on
  Parallels).
- A firmware serial console at or above the loader's 4 GiB direct map is
  not used (no machine seen with one).
- PCI enumeration turns memory decoding off while it sizes BARs; a console
  write in that window stalls on Parallels' framebuffer (nothing logs
  there today).

**Commands and the Linux personality**

- `wget` refusing a bad certificate on Parallels ends with "Resource
  temporarily unavailable" after mbedTLS's message (cause not
  investigated).
- `sftp`'s `ls -l` shows `?` for the link count (cause not investigated).
- `/proc/<pid>/stat` reports VSZ and RSS as 0, so `top` and `ps` show no
  process memory (reading another process's space needs a reference on
  it, P34).
- A read of `/dev/zero` returns at most 64 KiB (`dd bs=1M` copies partial
  records).
- `prctl` is `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` only;
  `ITIMER_VIRTUAL` and `ITIMER_PROF` are `-EINVAL`.
- No `ping6` and no raw IPv6 socket; a raw ICMP socket has no `bind`
  (`ping -I`), `connect`, `IP_HDRINCL` or `ICMP_FILTER`.
- `wget`'s CA bundle is fixed at build time and nothing checks
  revocation.

**Storage, network internals and tooling**

- Every mount of a freshly installed cosmofs warns that the superblock's
  free block count differs from the bitmap's by 4-7 blocks.
- `mbufq_enqueue` returns `false` both when it freed the packet and when
  it did not touch it; callers assume the first.
- `vm_map_phys` maps large blocks that `vm_unmap_phys` cannot tear down
  across a 2 MiB block; its 14 callers are not audited for sizes.
- `ports/busybox/README.md`'s config regeneration fails at `make
  defconfig` on the macOS build host.
- The AArch64 boots' 240 s whole-boot timeout: three timeouts recorded
  in `docs/testing/flakes.md`; PR #389 measures six in the last 40 CI
  runs, all the entropy job's first boot, records them there, and widens
  the timeout to 300 s.

The 1.0 out-of-scope list ([roadmap-1.0](roadmap-1.0.md#out-of-scope-for-10))
stands, less "GPU and display", which the virtio-gpu unit began (a text
console only: no display interface for programs, no 3D or cursor).

## Proposed next units

For the owner to choose from; none is started. Each names its acceptance
test, which is the unit's proof under the scope rules (green on main, box
ticked in the same PR).

- [x] **P1 Power-off on AArch64.** A `poweroff` command (BusyBox's
  `CONFIG_POWEROFF` is off today and nothing else provides one) and
  init's shutdown call PSCI `SYSTEM_OFF` when semihosting is absent (the
  conduit is already read from the FADT). Closes the "no power-off" line.
  (x86-64 has only QEMU's `isa-debug-exit` too; ACPI S5 needs the DSDT's
  `\_S5` values, so it waits for AML.) **Acceptance:** a `make
  test-platforms` variant with semihosting disabled boots twice, once
  ending with `poweroff` and once with `exit` from the login shell, and
  QEMU exits by itself within the boot budget each time; on Parallels
  Desktop the VM stops (manual, recorded in the report). Done
  2026-10-15: `make test-platforms`' `psci-exit` and `psci-poweroff` in
  CI (job `platforms`); on Parallels Desktop 27.0.3 both stopped the VM
  1.1 s after the command
  ([report](audit/2026-10-15-aarch64-poweroff-report.md)).
- [ ] **P2 Process memory in procfs** (widened by
  [S3](#s-shell-completeness), which does it with the rest of `/proc`).
  `/proc/<pid>/stat` reports VSZ and RSS, and `/proc/<pid>/status` gains
  `VmSize` and `VmRSS`, from a reference on the target's space taken under
  the rule exit already follows (P34). `/bin/ps` is the native
  `userland/system/ps.c`, which takes no options and prints no memory: the
  unit gives it VSZ and RSS columns (or enables BusyBox's `ps`,
  `CONFIG_PS`, now off). **Acceptance:** `test-busybox` runs `ps` and `top
  -b -n 1` and requires a non-zero VSZ and RSS for a running `sleep`, and
  reads that process's `status` for non-zero `VmSize` and `VmRSS`; a
  self-test reads a process's `stat` and `status` while it exits, many
  times, with no fault or leak.
- [ ] **P3 The Parallels-test residue.** The four small defects the
  post-1.0 tests found: `wget`'s bad-certificate exit message, `sftp`'s
  link count, `/dev/zero`'s 64 KiB short read, and the installer's stale
  superblock free count. Each a defect fix with its own failing test and
  probe. **Acceptance:** `test-net-login` requires the refusal to end on
  mbedTLS's reason alone and `sftp ls -l` to show a numeric link count;
  `test-busybox` checks `dd if=/dev/zero bs=1M count=4` copies 4 full
  records; `test-install` requires the installed disk's first mount to
  log no free-count warning.
- [ ] **P4 Shared libraries.** musl's dynamic linker as `PT_INTERP`, with
  `libc.so` in the image; until now only a stand-in interpreter has run.
  **Acceptance:** a dynamically linked musl BusyBox (and a small
  dynamically linked test program using `dlopen`) passes the M3 scripted
  test on both architectures.
- [ ] **P5 Setting the clock.** Absorbed by
  [S5](#s-shell-completeness), which carries its scope (`settimeofday`,
  `clock_settime`, BusyBox `ntpd`) and its acceptance, with one change:
  the operation privilege is `PRIV_SYS_ADMIN`, in place of the
  `PRIV_TIME` this entry proposed (uid 0 alone still grants nothing,
  `docs/kernel/security/design.md`).
- [ ] **P6 A network package repository.** `pkg` fetches an index and
  packages over HTTPS from a configured URL, checking the index's and each
  package's signature against the key ring, over the same certificate
  checks as `wget`. **Acceptance:** a test serves a signed repository
  from the host harness; `pkg update && pkg install` installs from it; a
  tampered package and an index signed by another key are refused; and
  the same repository behind an untrusted, an expired and a wrongly named
  certificate is refused before any index is read (a signature does not
  stop the replay of an older signed index, `docs/pkg/invariants.md`).

## S: shell completeness

Testing the installed system on Parallels Desktop found everyday
commands missing or failing. `clear` is built since v1.0.2, but an
installed system does not update itself (S7). `ip`, `ifconfig`,
`hostname -s`, BusyBox `ps` and `top`, `chown`, `reboot`, `mount` and
`date -s` fail because the Linux personality lacks the interfaces they
use: no `SIOCGIF*` ioctls or netlink, a sparse `/proc`, and no `chown`,
`reboot`, `mount` or `settimeofday` system calls.

The owner chose this track on 2026-10-15. Its units are taken in the
order S1, S2, S3, S4, S5, S6, S7. Each is a roadmap unit under
[`AGENTS.md`](../AGENTS.md#scope-rules)'s scope rules: one at a time, at
most 5 PRs, proven by its acceptance test (in `make test-busybox` or a
`make test-shell`, run in CI on both architectures), and done when that
test is green on main with its box ticked here in the same PR.

BusyBox's `ip` speaks only netlink (`RTM_GETLINK`/`RTM_GETADDR` over
`AF_NETLINK`). Netlink is not in S1: the native `ip` front end is the
deliberate substitute until netlink exists, and S1 records netlink in
the inventory.

- [x] **S1 Network inspection.** The Linux personality's interface
  ioctls: `SIOCGIFCONF`, and the getters `SIOCGIFNAME`, `SIOCGIFINDEX`,
  `SIOCGIFFLAGS`, `SIOCGIFADDR`, `SIOCGIFNETMASK`, `SIOCGIFBRDADDR`,
  `SIOCGIFHWADDR` and `SIOCGIFMTU`, with their `SIOCS*` setters under
  `PRIV_NET`. `/proc/net/dev`, `/proc/net/route`, `/proc/net/arp`,
  `/proc/net/tcp` and `/proc/net/udp` in Linux's formats. The
  `sethostname` system call (Linux has no `gethostname` call on these
  architectures: libc reads the name from `uname`, which already gives
  the uts name), and `/etc/hostname` set from DHCP option 12 (host name)
  when the server sends one. BusyBox
  `ifconfig`, `route`, `netstat`, `arp` and `hostname`. A native `ip`
  front end, read-only, printing iproute2's output format from `netctl`:
  `ip a`, `ip addr`, `ip link`, `ip r`, `ip route`, `ip -4 ...` and
  `ip neigh`. Netlink stays out and is recorded. **Acceptance:** on the
  installed system, `ip a` shows `eth0` with its DHCP address, and `ip
  link`, `ip -4 a`, `ip r` (the default route and the subnet) and `ip
  neigh` (the gateway) each print what they promise; `ifconfig`, `route
  -n`, `netstat -tuln` (dropbear on `:22`), `arp -a` (the gateway) and
  `hostname` print the machine's state; `ifconfig eth0 down` and `up`
  work as root and fail with `EPERM` when `PRIV_NET` is dropped; and each
  other setter is exercised once as root (an address and netmask set and
  read back, a broadcast and an MTU accepted or refused as documented)
  and once with `PRIV_NET` dropped (`EPERM`). Done 2026-10-15:
  `test-net-login`'s stage g in CI (job `net-login`, both
  architectures); on Parallels Desktop 27.0.3 the installed system showed
  the same and `ifconfig eth0 down`/`up` worked
  ([report](audit/2026-10-15-network-inspection-report.md)).
- [x] **S2 Applets.** `more`, `nl`, `od`, `strings`, `rev`, `tac`, `yes`,
  `cal`, `time`, `timeout`, `watch`, `nohup`, `killall`, `pgrep`,
  `pkill`, `pidof`, `install`, `realpath`, `split`, `fold`, `paste`,
  `comm`, `cksum`, `base64`, `unzip`, `bzip2`, `bunzip2`, `xz`, `unxz`,
  `zcat`, `mktemp`, `logger` (to the console log) and `setsid`; `ash`'s
  command history saved to `/root/.ash_history` (the console shell's
  `HOME` is `/` today). **Acceptance:** a scripted test runs each with
  known input and requires its known output; `pgrep`, `pkill`, `killall`
  and `pidof` find a background `sleep` through `/proc` and act on it;
  a uniquely tagged `logger` message appears in the kernel's log; a
  command typed in an interactive `ash` is in `/root/.ash_history` after
  that shell exits and a new one recalls it. Done 2026-10-15:
  `test-busybox` in CI (job `busybox`, both architectures); `logger`
  through a native `syslogd`, BusyBox's `xz` decompressing only
  ([report](audit/2026-10-15-everyday-applets-report.md)).
- [ ] **S3 Process tools on `/proc`** (widens P2). `/proc/<pid>/stat`
  with the fields BusyBox `ps` and `top` read (state, ppid, pgrp,
  session, tty_nr, utime, stime, priority, nice, num_threads, starttime,
  vsize, rss); `/proc/<pid>/status` (`Name`, `State`, `Pid`, `PPid`,
  `Uid`, `Gid`, `VmSize`, `VmRSS`, `Threads`); the `/proc/<pid>/cwd`,
  `exe` and `root` links; `/proc/<pid>/environ`; `/proc/meminfo` with
  `MemTotal`, `MemFree`, `MemAvailable`, `Buffers`, `Cached`, `SwapTotal`
  and `SwapFree`; `/proc/stat`'s `cpu` lines and `btime`;
  `/proc/loadavg`, `/proc/version` and `/proc/cpuinfo`. BusyBox `ps`,
  `top`, `free`, `uptime` and `kill -l`. The unit settles two rules
  first: who may read another process's `environ` (procfs leaves
  environments out because they cannot yet be read safely; a process
  that cleared `PR_SET_DUMPABLE` must not have its environment read
  without `PRIV_PROC`), and the revision of procfs's P4 that admits
  `version` and `cpuinfo`, which no program here reads today.
  **Acceptance:** `ps` lists init, sshd and the shell with their correct
  PIDs and states; `top -bn1` runs; `free` matches `sysctl`'s figures;
  P2's checks -- a non-zero VSZ and RSS for a running `sleep` in `ps`,
  `top` and its `status`, and a self-test reading a process's `stat` and
  `status` while it exits, many times, with no fault or leak; and a
  cross-user or non-dumpable `environ` read refused.
- [ ] **S4 Ownership.** `chown`, `fchown`, `fchownat`, `lchown` and
  `chroot`; BusyBox `chown`, `chgrp` and `su` (one user, root: `su` to
  root is a no-op that works, `su` to another uid needs `PRIV_CRED`).
  **Acceptance:** a `chown` on cosmofs persists across a reboot; an
  unprivileged process gets `EPERM`; `fchown`, `fchownat`, `lchown` on a
  symbolic link (the link changes, its target does not) and `chroot` are
  each exercised; `chgrp` changes the group; `su` to root works, and `su`
  to another uid is refused with `PRIV_CRED` dropped.
- [ ] **S5 Power and clock** (absorbs P5). `reboot(2)` with
  `LINUX_REBOOT_CMD_RESTART`, `HALT`, `POWER_OFF`, `CAD_ON` and `CAD_OFF`;
  PSCI `SYSTEM_RESET` on AArch64, the keyboard-controller and ACPI reset
  register paths on x86-64. Power-off on x86-64 stays QEMU-only
  (`isa-debug-exit`) until an AML interpreter can read `\_S5`.
  `reboot(2)` needs `PRIV_SYS_ADMIN`. `settimeofday` and `clock_settime`
  under `PRIV_SYS_ADMIN` (in place of P5's proposed `PRIV_TIME`); the RTC
  read at boot on both machines (PL031 on virt, CMOS on q35) and written
  by `hwclock`. BusyBox `reboot`, `halt`, `poweroff -f`, `date -s`,
  `hwclock` and P5's `ntpd`. **Acceptance:** `reboot` restarts the
  installed system under QEMU on both architectures and on Parallels
  Desktop (run by the owner, recorded in the report), and is refused with
  `sys_admin` dropped; `date -s` then `hwclock -w` persists across the
  reboot where the machine has an RTC; `date -s` is refused for an
  unprivileged process and for root with `sys_admin` dropped; and P5's
  check: the harness serves NTP a day ahead and `ntpd -q` sets
  `CLOCK_REALTIME` to within 1 s of it.
- [ ] **S6 Mounting.** The Linux `mount` and `umount2` doors (cosmofs,
  ramfs and procfs, with the flags the native mount supports), each under
  `PRIV_MOUNT` as the native calls are; `/etc/fstab` and `mount -a` from
  `/etc/rc`; BusyBox `mount`, `umount`, `lsblk` (from `/sys/block` or a
  procfs equivalent naming the GPT partitions) and `blkid` (the
  `PARTUUID` and the cosmofs label). **Acceptance:** a second cosmofs on
  the blank disk is mounted by `/etc/fstab` at boot and survives a
  reboot; a ramfs and a procfs mount and unmount by hand; `lsblk` names
  the GPT partitions and `blkid` the root's `PARTUUID` and label; `mount`
  and `umount` are refused with `PRIV_MOUNT` dropped.
- [ ] **S7 Updating an installed system** (after P6). `pkg upgrade` for
  the userland, and `cosmo-upgrade`, which replaces the ESP's contents
  (loader, kernel, `boot.tar`, manifest) from a release: it checks the
  manifest's signature before writing anything and keeps the previous set
  under `\cosmo\prev` for fallback. Two questions its design settles
  first: how an interrupted replacement stays bootable (the loader reads
  only the fixed `\cosmo` paths today, so either the switch is atomic or
  the loader can boot `\cosmo\prev`), and whether `--rollback` returns
  the packages `pkg upgrade` changed too (`docs/pkg/invariants.md`: a
  package upgrade cannot be rolled back today) or promises the boot set
  alone. **Acceptance:** an installed v1.0.3 system is upgraded to the
  current build without reinstalling and reboots into it, and
  `cosmo-upgrade --rollback` returns it to v1.0.3 (as far as the design
  promises); a tampered manifest and one signed by another key are
  refused with the ESP byte-for-byte unchanged; an update interrupted
  mid-replacement still boots.

Larger items, not proposed as one unit until scoped: SATA on Parallels
(an AML interpreter, or a DSDT-described AHCI path with no generic AML),
IPv6 beyond the link (routing, `ping6`), and a display interface for
programs on virtio-gpu.
