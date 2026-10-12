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
   `wget` (HTTP only, and why).
4. [ssh](ssh.md): a key, the dropbear server, logging in as root.
5. [Packages](packages.md): `pkg` and the repository on the installed
   disk.
6. [Privileges](privileges.md): the privilege set, dropping privileges,
   and what an ssh session holds.
7. [Known limitations](limitations.md).

Commands for your computer are shown as plain blocks; commands typed on
CosmoOS are shown after its prompt, `cosmo$ ` on the console and `# `
in an ssh session, followed by what they print.

## How this guide is tested

Every command in these pages is run by CI against the release images:
`tests/guide/run_guide.py` reads the pages in the order above, runs each
block for your computer in one shell, starts each QEMU or ssh session,
types each CosmoOS command at its prompt and requires the output shown
(`...` stands for text that differs from run to run). It runs on Linux
for both architectures, and the macOS steps run on a macOS runner (the
CI job `guide`; the release workflow runs it against the signed images
before publishing them). A change that makes a page wrong fails CI.
