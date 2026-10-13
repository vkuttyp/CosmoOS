# Known limitations

What CosmoOS 1.0 does not do, or does differently from what you may
expect. Each item names where it is recorded; the work that remains
after 1.0 is in [`docs/plan.md`](../plan.md), and the full list of known
gaps in the [deferred-work inventory](../audit/2026-09-deferred-work-inventory.md).

## Machines

- **QEMU is the tested platform.** x86-64 on `q35` and AArch64 on `virt`
  are tested by CI; there is no physical hardware matrix. Since v1.0.1
  the AArch64 image boots on Parallels Desktop 27 on Apple Silicon, and
  since v1.0.2 installs on its NVMe disks and has a network there; its
  SATA disks are not seen ([Tested platforms](platforms.md)). Out of scope for 1.0
  ([roadmap](../roadmap-1.0.md)): GPU and display (a framebuffer console
  only), hotplug, NUMA, more than 64 CPUs, huge pages, live migration,
  RAID-Z, eBPF, and the Intel VMX backend on hardware.
- **Randomness from the weakest source last.** The pool seeds from the
  CPU's random instructions, the firmware's `EFI_RNG_PROTOCOL` or a
  virtio-rng; with none of them (Parallels Desktop) a timing-jitter source
  seeds it, credited conservatively (docs/kernel/security/design.md §6).
  `random.jitter=off` on the command line turns that off, and the pool
  then never seeds there. The commands in this guide add `-device
  virtio-rng-pci`.
- **The clock is read once.** The wall clock is set at boot from the RTC
  or, with none (Parallels), the firmware's time; there is no NTP and no
  way to set it.
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
  page](shell.md): no `ping`, `dd`, `md5sum`, `less`, `vi`, `bunzip2`;
  no hard links; no `/sys`.
- The terminal driver honours the common modes (canonical input, echo,
  signals, raw mode, window size); most other termios flags and control
  characters are accepted and ignored (inventory 1.4).
- No swap.

## Networking

- **`wget` is HTTP only**: TLS is compiled out of BusyBox, by decision
  ([Networking](networking.md)).
- The address comes from DHCP. There is no runtime tool to set one by
  hand; `/etc/resolv.conf` is written once per boot, not on lease
  renewal; the DHCP client does no ARP probe or DECLINE, no INIT-REBOOT,
  no RELEASE, and ignores classless routes (option 121) (inventory 2.5).
- IPv6 is link-local; there is no IPv6 routing beyond the link, no IP
  fragmentation, and no TCP SACK, window scaling or timestamps
  (inventory 2.5).
- ssh is dropbear with **key login only (Ed25519 or RSA), root only**;
  no scp, sftp or port forwarding ([ssh](ssh.md)).

## Storage

- cosmofs files are capped at 516128 blocks of 4 KiB (about 1.97 GiB),
  64516 blocks (252 MiB) when encrypted; a larger write fails with
  `EFBIG`.
- A read-only mount still writes the replay of an interrupted free or
  orphan record, as a journalling filesystem replays its journal.
- No cosmofs rollback to a snapshot from the running system, no clones,
  no RAID-Z, no resilvering (inventory 2.4); a deleted newest snapshot's
  id can be reused after a remount.
- A root disk that appears late (USB storage) is not waited for: init
  stays on the live root and says so.

## Packages

- The repository is the one the image was built with, on the installed
  disk; there is no network repository and no fetcher (`docs/pkg/`).
- No rollback of a completed upgrade.

## Linux programs

- Static and PIE Linux binaries for x86-64 and AArch64 run under the
  Linux personality; it is a subset of Linux's system calls, so a
  program that needs a missing one gets `ENOSYS`. The loader honours
  `PT_INTERP`, but only a stand-in interpreter has been run through it:
  no real `ld.so` with shared libraries, musl's or glibc's, has.
  No containers or desktop software (`docs/plan.md`).
