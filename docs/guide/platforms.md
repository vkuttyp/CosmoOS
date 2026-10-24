# Tested platforms

What CosmoOS has been run on, and how much of it works there.

## QEMU (the tested platform)

x86-64 on the `q35` machine and AArch64 on the `virt` machine, QEMU 9.2
or newer, from Linux and macOS: everything in this guide. CI boots both
on every change, and the release workflow runs the guide's commands
against the release images ([How this guide is tested](README.md)).
AArch64 is also booted on QEMU set up unlike virt's defaults -- a GICv3,
no memory above 4 GiB, a CPU with and without the RNDR instruction, no
serial port -- and both architectures with 1, 4 and 8 GiB of RAM, by
`make test-platforms`.

## Parallels Desktop 27 on Apple Silicon (AArch64)

The AArch64 image boots to the live shell since v1.0.1, and since v1.0.3
it installs, boots the installed system, has a network and ssh, seeds
its randomness and keeps the right time. To try it: create a virtual
machine for "Other Linux", ARM64, with UEFI; put `cosmoos-aarch64.img`
on a disk of the "plain" (raw) type and attach it as an **NVMe** disk,
and add a second, blank NVMe disk of 1 GiB or more to install on.
Parallels' SATA disks are not PCI devices, so the kernel would not see
the live disk and `cosmo-install` would have no ESP to copy. Any amount
of memory works since v1.0.5 (run with 2, 4 and 8 GB); **v1.0.4 and
earlier need 2 GB**: with Parallels' default of 4 GB their loader lost
its own code at the handover and nothing followed its last line
(`jumping to kernel entry ...`). From the command line, with
`prlctl`, for a VM named `CosmoOS` (these need Parallels, so the guide's
own tests do not run them; they were run on Parallels Desktop 27.0.3
when this was written):

```text
prlctl set "CosmoOS" --memsize 2048
prlctl set "CosmoOS" --device-set hdd0 --iface nvme
prlctl set "CosmoOS" --device-add hdd --type plain --size 1024 --iface nvme
```

The two disks are two namespaces of one controller: `cosmo-install
--list` shows the live disk (the one with the `boot` partition) and
the blank one, numbered in the order the VM lists them (`nvme0n1` and
`nvme0n2`); `cosmo-install` with the blank one's name installs, and `exit`
or `poweroff` shuts the system down and the VM stops (a PSCI power-off;
v1.0.3 halts instead, and its VM stays running until you stop it).
With the blank disk first in the boot order (or the live
disk disconnected) the VM boots the installed system. The network
adapter's default, shared networking, gives it an address by DHCP; ssh
listens on port 22 of that address.

**ssh from the Mac.** Parallels' shared network puts the Mac at
10.211.55.2 and the VM at the address its boot log names (`eth0: bound
10.211.55.N/24 ...`); there is no port forwarding to set up. On the
installed system, put your public key (an existing `~/.ssh/id_rsa.pub`
or an Ed25519 one) in root's `authorized_keys` by serving it from the
Mac and fetching it at the VM's prompt. Serve a directory holding only
the public key, never `~/.ssh` itself, which would offer your private
keys to the network too:
`mkdir /tmp/pubkey && cp ~/.ssh/id_rsa.pub /tmp/pubkey/ && cd /tmp/pubkey
&& python3 -m http.server 8000 --bind 10.211.55.2`, then in the VM:

```text
mkdir -p /root/.ssh
wget -q -O /root/.ssh/authorized_keys http://10.211.55.2:8000/id_rsa.pub
chmod 600 /root/.ssh/authorized_keys
```

then `ssh root@10.211.55.N` from the Mac. (A `text` block: the guide's
tests cannot run Parallels.)

Clicking into the VM's window gives it the keyboard and the pointer, and
they stay there: **press Control+Option to get them back**. That is
Parallels' behaviour for a guest without Parallels Tools, which CosmoOS
does not have (they talk to Parallels through its own device, which this
kernel does not drive). Parallels' firmware offers the guest one display
mode, 1024x768, where v1.0.3 draws a 128x48 text screen. With the
virtio-gpu driver (merged after v1.0.3) the screen console takes the size Parallels gives the display and follows the VM's window
when it is resized (wider: 175x48 cells at 1407x768). Parallels keeps
the height at 768 pixels.

The kernel finds the machine from its ACPI tables and logs one line
saying what it found:

```text
platform: firmware OEM 'PRLS  '; console PL011 at 0x2110000 (DBG2, polled); GICv3 GICD 0x2010000 GICR 0x2500000 ITS 0x0 MSI frame 0x2250000; timer PPIs 30 (physical) 27 (virtual); PSCI SMC
```

| | |
|---|---|
| Works | verified boot; both CPUs; the console on the screen, at the window's size (the virtio-gpu adapter) and on the VM's serial port (set it to a file or a socket); the USB keyboard; the live shell; installing on an NVMe disk and booting it (the controller offers only MSI: one vector for every queue); the network (the adapter is a legacy virtio-net) with DHCP, DNS, `wget` and ssh; randomness (no CPU instruction, firmware generator or virtio-rng: the timing-jitter source seeds the pool in milliseconds); the wall clock (the firmware's time at boot); power-off (`exit`, `poweroff`: PSCI) |
| Does not work | SATA disks (not on PCI: a platform AHCI described only in the DSDT, which needs an AML interpreter); the sound, USB 2.0 (EHCI) and vsock devices have no driver, and the graphics adapter (virtio-gpu) only carries the text console (no 3D, cursor or display interface for programs); the debug self-test suite assumes QEMU's machine; adding or removing a disk while the VM runs (Parallels refuses it, and its NVMe controller sits on bus 0 with no hot-plug slot) |

v1.0.4, the published image, was run there on 2026-10-18 (Parallels
Desktop 27.0.3, 4 CPUs, 2 GB): it installed itself on a 512 MiB NVMe
disk, and the installed system booted with the release key, had its
address by DHCP (`ip a`), listed its processes (`ps`), mounted its root
from `/dev/nvme0n2p2` (`mount`, `df`), took an ssh login from the Mac,
and booted again after `reboot`.

The image that became v1.0.5 was run on the same VM on 2026-10-18 with
4096 MB and 8192 MB: it booted to the shell both times, `free` reported
4133256 KiB and 8294792 KiB, and the network came up by DHCP. With 4096
MB the installed v1.0.4 still stopped after its loader's last line.

The same installed v1.0.4 was then upgraded in place to the published
v1.0.5 on 2026-10-18 ([Upgrading](upgrading.md), "From v1.0.4 and
later"). At 2048 MB it fetched the aarch64 set and both `SHA256SUMS`
files from the Mac with `wget` into `/tmp/up`, and its own
`/sbin/cosmo-upgrade /tmp/up` verified them against the release key
36ae7bb4947896ab, switched the loader (`\EFI\BOOT\BOOTAA64.EFI`) and the
boot set, and kept v1.0.4's in `\cosmo\prev` (root: 177 files unchanged,
1 kept as changed here). After `reboot` the banner said `build v1.0.5`
(`uname -a`: `v1.0.5`). Then the VM was set to 4096 MB, Parallels'
default, and the upgraded disk booted to the shell (`free`: 4133176 KiB,
"RAM ends at 5120 MiB"), which needs the loader the upgrade brought;
at 8192 MB too (`free`: 8294712 KiB, DHCP address 10.211.55.12).

**A mirrored root, v1.0.11-rc1, 2026-10-23.** The VM (Parallels Desktop
27.0.3, 4 CPUs, 4096 MB) was upgraded from v1.0.10 to v1.0.11-rc1 with
its own `cosmo-upgrade`, the set served from the Mac. Parallels cannot
add or remove a disk while the VM runs (`prlctl` refuses: "The device
cannot be added or removed while the virtual machine is running"), so
each change of disks was made with the VM stopped:

- A third NVMe disk of 1 GiB was added (`prlctl set ... --device-add
  hdd --type plain --size 1024 --iface nvme`). It is a third namespace
  of the same controller, `nvme0n3`.
- `cosmo-pool attach nvme0n2p2 nvme0n3` raised the pool to format 13
  and resilvered the new copy, 13199 blocks in 1670 ms. `cosmo-pool
  status` then said `ONLINE` for both copies, and `fsctl scrub` was
  clean (0 bad copies).
- With the disk disabled (`--device-set hdd2 --disable`), the system
  booted and said `cosmofs: member 0 copy 1 is missing; the pool is
  degraded` and `init: the root pool is degraded`. `cosmo-pool status`
  said `DEGRADED`, copy 1 `MISSING`, and `/proc/mounts` said
  `rw,degraded`. A file written then read back intact.
- With the disk enabled again, the boot said `nvme0n3 is back as member
  0 copy 1: resilvering it whole`, and the copy was `ONLINE` 13799
  blocks and 1715 ms later. A scrub was clean, and both files read back
  with their checksums.

The same VM was then upgraded to v1.0.11-rc2 (the rc for the corrected
notes; the same code) and from it to v1.0.11 (`cosmo-upgrade --fetch
v1.0.11`); each boot checked the root clean and the mirror was `ONLINE`,
and a scrub under rc2 was clean.

The added disk carries only the pool's copy and no ESP, so the VM
boots from `nvme0n2` alone. `cosmo-install DISK --mirror DISK2` gives
both disks an ESP.

**ssh after reboot, v1.0.12-rc1 and v1.0.12, 2026-10-24.** A new VM,
"CosmoOS" (Parallels Desktop 27.0.3, 4 CPUs, 4096 MB; the published
v1.0.11 image on a 66 MiB disk, installed to a 512 MiB one, IP
10.211.55.15), was upgraded from v1.0.11 to v1.0.12-rc1 with its own
`cosmo-upgrade`, the rc's set served from the Mac. It took the boot
environment `be-v1.0.11` and booted `build v1.0.12-rc1` (verified, key
36ae7bb4947896ab); the root checked clean and the boot said `svc:
dropbear started`. `/run` was a ramfs, `/run/svc/dropbear.pid` held the
pid and its start time, `cosmo-upgrade --status` said the root has the
intended set's base system, `--esp-check` and `--repair` had nothing to
do on its one disk, `pkg update && pkg upgrade` read index serial 13, and
`dmesg` named no panic and no error but the usual `xhci probe failed`.
ssh from the Mac answered before and after each of two reboots, with
nothing removed by hand. Then `cosmo-upgrade --fetch v1.0.12` from the
Pages repository, a reboot to `build v1.0.12`, dropbear started, ssh
answered, and `pkg update` read serial 14. This VM has one disk, so the
mirrored ESPs are shown by `make test-mirror` only.

**An encrypted root, v1.0.13-rc1, 2026-10-24.** "CosmoOS" was upgraded
from v1.0.12 to v1.0.13-rc1 the same way (boot environment
`be-v1.0.12`; 43 files replaced, 1 added, 1 kept as changed here). It booted `build v1.0.13-rc1` (verified,
key 36ae7bb4947896ab), its plain root stayed at format 12 and checked
clean, `cosmo-key list` said it is not an encrypted pool, `--status` and
`--esp-check` were clean, `pkg update` read serial 15, and ssh answered.
A second VM, "CosmoOS encrypted" (4 CPUs, 4096 MB, IP 10.211.55.16),
booted the rc's published image on a 66 MiB disk and ran `cosmo-install
--encrypt` on a blank 512 MiB one, the passphrase typed at its two
prompts on the serial console:

    cosmo-install: slot 0 sealed with the passphrase (argon2id 65536 KiB, 3 passes: 0.22 s)

Booted from that disk, the console asked `passphrase for cosmo-root:`
before the root was checked or mounted, and:

    cosmo-key: cosmo-root unlocked with slot 0 (argon2id 65536 KiB, 3 passes: 0.16 s)
    cosmofs: root checked, 0 findings, 0 repaired
    init: switched to the disk root nvme0n2p2 (root=boot-disk)

The same after a reboot; HTTPS to the Pages repository, `pkg update`,
`cosmo-key list`, a clean `dmesg`, and ssh from the Mac all answered.
On the first boot dropbear was still making its host key when `ps`
ran; it was running after. Argon2id took 0.16 s here against about a
second under QEMU: the cost is fixed, not calibrated (backlog CFS-37),
so the guide and the release notes give both.

The details, and why, are in
[`docs/audit/2026-10-13-acpi-discovery-report.md`](../audit/2026-10-13-acpi-discovery-report.md)
and [`docs/audit/2026-10-13-parallels-devices-report.md`](../audit/2026-10-13-parallels-devices-report.md).

## Other machines

Not tested. A machine whose firmware describes its devices in ACPI
(SPCR or DBG2, MADT, GTDT, FADT) gets as far as its devices allow; one
whose tables are missing says which one on the screen and stops.
