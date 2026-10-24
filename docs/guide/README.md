# CosmoOS user guide

CosmoOS 1.0 runs on QEMU, on x86-64 (the `q35` machine) and AArch64 (the
`virt` machine), from a Linux or macOS host. This guide takes you from
the release images to an installed system you log in to over ssh. Read
the pages in order: each one continues where the one before it stopped.

1. [Installing](installing.md): QEMU and firmware, checking the images,
   booting the live image, installing to a disk, the disk layout, and
   verified boot.
2. [First boot and the shell](shell.md): the installed system, BusyBox
   `ash`, the commands there are, job control, and what is missing
   compared with Linux.
3. [Networking](networking.md): DHCP, `/etc/resolv.conf`, `netcfg`, and
   `wget` over HTTP and HTTPS.
4. [ssh](ssh.md): a key, the dropbear server, logging in as root.
5. [Packages](packages.md): `pkg` and the repository on the installed
   disk.
6. [Upgrading](upgrading.md): `cosmo-upgrade` to a newer release and
   back.
7. [Snapshots and rollback](snapshots.md): `cosmo-snap`, what a snapshot
   keeps, and rolling the system back to one.
8. [Checking and repairing](checking.md): the root's check at boot,
   `fsck.cosmofs`, `fsctl check` and `scrub`, mirrors.
9. [Mirrors](mirrors.md): a root on two disks, a disk that goes,
   `cosmo-pool replace`, resilvering, spares.
10. [Encrypted root](encrypted-root.md): `cosmo-install --encrypt`, the
   passphrase at boot, key slots, a key file, and what it protects.
11. [Privileges](privileges.md): the privilege set, dropping privileges,
   and what an ssh session holds.
12. [Known limitations](limitations.md).
13. [Tested platforms](platforms.md): QEMU, and Parallels Desktop on
   Apple Silicon.

Commands for your computer are shown as plain blocks; commands typed on
CosmoOS are shown after its prompt, `cosmo$ ` on the console and
`~ # ` in an ssh session, followed by what they print.

## How this guide is tested

Every command in these pages is run by CI against the release images:
`tests/guide/run_guide.py` reads the pages in the order above, runs each
block for your computer in one shell, starts each QEMU or ssh session,
types each CosmoOS command at its prompt and requires the output shown
(`...` stands for text that differs from run to run; the kernel's
`[ INFO]` lines are left out). The CI job `guide` runs it on Linux for
both architectures on every pull request, so a change that makes a
Linux step wrong fails there; `guide-macos` runs the macOS steps on a
macOS runner on every push to main; and the release workflow runs both
against the signed images before it publishes them. The fetches from the
internet check the project's own site; the one from `example.com`, a
site the project does not run, only warns when it fails, and a fetch
answered with a server error (5xx) is tried again, up to three times.

Upgrading's steps are the exception: they need an older release installed
first, so the runner cannot run them. `make test-upgrade` runs them instead
(the CI job `upgrade`, on every pull request, on both architectures), and
the page shows them as text. Snapshots and rollback's are the other: a rollback reboots
the machine. `make test-snapshot` runs them (the CI job `install`).
Checking and repairing's need a damaged disk: `make test-fsck` runs
them (the CI job `fsck`). Mirrors' remove a disk from a running machine:
`make test-mirror` runs them (the CI job `mirror`).
