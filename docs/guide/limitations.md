# Known limitations

What CosmoOS 1.0 does not do, or does differently from what you may
expect. Each item names where it is recorded; the work that remains
after 1.0 is in [`docs/plan.md`](../plan.md), and the full list of known
gaps in the [deferred-work inventory](../audit/2026-09-deferred-work-inventory.md).

## Machines

- **QEMU is the tested platform.** x86-64 on `q35` and AArch64 on `virt`
  are tested by CI; there is no physical hardware matrix. Since v1.0.1
  the AArch64 image boots on Parallels Desktop 27 on Apple Silicon, and
  since v1.0.3 installs on its NVMe disks and has a network there; its
  SATA disks are not seen ([Tested platforms](platforms.md)). Out of scope for 1.0
  ([roadmap](../roadmap-1.0.md)): GPU and display beyond a text console (on
  the firmware's framebuffer, or a virtio-gpu adapter at the display's
  own size: no 3D, cursor or display interface for programs), CPU and memory
  hotplug (PCIe disks can be added and removed since v1.0.11), NUMA, more than 64 CPUs, huge pages, live migration,
  RAID-Z, eBPF, and the Intel VMX backend on hardware.
- **Randomness from the weakest source last.** The pool seeds from the
  CPU's random instructions, the firmware's `EFI_RNG_PROTOCOL` or a
  virtio-rng; with none of them (Parallels Desktop) a timing-jitter source
  seeds it, credited conservatively (docs/kernel/security/design.md §6).
  `random.jitter=off` on the command line turns that off, and the pool
  then never seeds there. The commands in this guide add `-device
  virtio-rng-pci`.
- **The clock is stepped, not slewed.** The wall clock is set at boot
  from the RTC or, with none (Parallels), the firmware's time. Since
  v1.0.4 root can set it (`date -s`, `hwclock -w` where there is an RTC)
  and `ntpd -q` steps it once from a server; there is no NTP daemon,
  because `adjtimex` is not implemented, and no time zone.
- **No Wi-Fi, Bluetooth or sound.**

## Boot and security

- Verified boot covers the loader's three files (kernel, boot archive,
  command line). The **root filesystem is not verified**; the loader is
  not signed for UEFI **Secure Boot**; there is **no TPM measurement**
  and **no rollback protection** for manifests or modules (inventory
  2.9).
- **No KASLR**: the kernel is at a fixed address. User-space ASLR covers
  the stack, heap, `mmap` and PIE executables; a non-PIE (`ET_EXEC`)
  program loads at its fixed address.
- **One user, root.** Privileges, not uids, separate what processes may
  do ([Privileges](privileges.md)). A root session holding fewer
  privileges can still rewrite files in `/etc` that a later boot runs
  with all of them (M5 report, inventory 2.9).

## Shell and commands

- The commands are the BusyBox applets listed in [the shell
  page](shell.md): no `ping6`; no hard links; no `/sys`.
- `/proc` has the process directories and a fixed set of system files
  (`meminfo`, `stat`, `loadavg`, `uptime`, `mounts`, `version`,
  `cpuinfo`). A process's nice value is kept and shown but does not yet
  change how it is scheduled, and its user/system split of CPU
  time is sampled by the 250 Hz tick (the total is exact); `ps`'s `TT` column is a pair of Linux device numbers (`5,1`
  for the console), since devices here have none of their own; and a
  process's `environ` is what exec gave it, not what it has since written
  over (inventory §13).
- The terminal driver honours the common modes (canonical input, echo,
  signals, raw mode, window size); most other termios flags and control
  characters are accepted and ignored (inventory 1.4).
- No swap.

## Networking

- **`wget`'s certificate authorities are fixed** in the image
  (`/etc/ssl/certs/ca-certificates.crt`, as built); nothing updates them,
  and there is no certificate revocation check
  ([Networking](networking.md)).
- The address comes from DHCP. There is no runtime tool to set one by
  hand; `/etc/resolv.conf` is written once per boot, not on lease
  renewal; the DHCP client does no ARP probe or DECLINE, no INIT-REBOOT,
  no RELEASE, and ignores classless routes (option 121) (inventory 2.5).
- IPv6 is link-local; there is no IPv6 routing beyond the link, no IP
  fragmentation, and no TCP SACK, window scaling or timestamps
  (inventory 2.5).
- ssh is dropbear with **key login only (Ed25519 or RSA), root only**;
  no port forwarding ([ssh](ssh.md)).

## Storage

- A file that LZ4 compresses (text, logs, most programs) is written
  compressed for its first 96 MiB and plain after that, so it can be as
  large as the pool: on a 1 GiB pool the largest such file was 124 MiB
  on v1.0.13 and is 1016 MiB on v1.0.14, the pool full. A file written
  with an `fsync` after every 32 KiB, or one with a hole between every
  two records, can still reach cosmofs's limit of 4096 runs per file;
  the write that would cross it is refused (`File too large`) and
  nothing already written is lost. Before v1.0.14 that write was not
  refused, and what was written past about 128 MiB was lost (backlog
  CFS-32). Data that does not compress (already compressed, encrypted
  before it was written) has no such limit; with it a file can reach
  1000 GiB, 125 GiB on an [encrypted root](encrypted-root.md).
- A read-only mount still writes the replay of an interrupted free or
  orphan record, as a journalling filesystem replays its journal.
- A rollback to a snapshot takes effect at the next boot (or the next
  mount of another filesystem), not in the running system
  ([Snapshots and rollback](snapshots.md)); a clone of a clone, and a
  snapshot of a clone, are not possible; no RAID-Z (backlog CFS-13). A
  mirror holds at most four copies (backlog CFS-27), and a copy missing when the pool is
  mounted is rebuilt whole when it returns, not caught up (backlog
  CFS-29; [Mirrors](mirrors.md)).
- A root disk that appears late (USB storage) is not waited for: init
  stays on the live root and says so.

## Packages

- A network repository is fetched whole, file by file: no mirrors, no
  delta updates. Its index's freshness is only as good as the clock, which
  is stepped by hand or by `ntpd -q`, with no NTP daemon
  ([Packages](packages.md#network-repositories)).
- `cosmo-upgrade --rollback` returns packages and `/etc` only to a boot
  environment, which upgrades made by v1.0.8's `cosmo-upgrade` and later
  take; past an upgrade made by v1.0.7 or older it returns the boot set and
  the base system only ([Upgrading](upgrading.md#boot-environments)). Only
  files under `/root`, `/home` and `/var/log` written after the upgrade
  are kept across a rollback; the root is rolled back at the next boot,
  never while it runs; one pool.

## Linux programs

- Static and PIE Linux binaries for x86-64 and AArch64 run under the
  Linux personality; it is a subset of Linux's system calls, so a
  program that needs a missing one gets `ENOSYS`. Dynamically linked
  musl programs run through musl's own linker, `/lib/ld-musl-$ARCH.so.1`,
  with `libc.so` and `dlopen`; glibc's has not been
  tried. A library with text relocations is refused (W^X); build it
  `-fPIC`. Native CosmoOS programs stay static.
  No containers or desktop software (`docs/plan.md`).
