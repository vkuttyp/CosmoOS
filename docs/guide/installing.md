# Installing

CosmoOS is installed the way it is tested: you boot the live image in
QEMU with a blank disk attached, run `cosmo-install` on that disk, and
from then on boot the disk alone.

## What you need

QEMU 9.2 or newer with its UEFI firmware, and about 1.2 GiB of free
space. On Debian or Ubuntu:

<!-- test: host linux -->
```sh
sudo apt-get update
sudo apt-get install -y qemu-system-x86 ovmf qemu-system-arm qemu-efi-aarch64 openssh-client python3
```

On macOS, with [Homebrew](https://brew.sh):

<!-- test: host macos -->
```sh
brew install qemu
```

## Getting and checking the images

From the release page, download into an empty directory the files for
your architecture -- `cosmoos-x86_64.img` or `cosmoos-aarch64.img` is the
one you need; the loader, kernel, boot archive, command line and manifest
beside it are the same files the image holds, and `cosmo-upgrade` is for
[upgrading](upgrading.md) an installed system -- and `SHA256SUMS`. Every
command in this guide runs in that directory. Check what you downloaded:

<!-- test: host linux -->
```sh
sha256sum --ignore-missing -c SHA256SUMS
```

<!-- test: host macos -->
```sh
shasum -a 256 --ignore-missing -c SHA256SUMS
```

Each file you have must say `OK`. The release notes list the same sums,
and the images rebuild bit for bit from the release's tag (`make
release` in the source tree), so you can also build them yourself and
compare.

## Firmware

QEMU boots the images through UEFI firmware, which you copy into the
directory. On Linux, for x86-64 and for AArch64:

<!-- test: host linux x86_64 -->
```sh
cp /usr/share/OVMF/OVMF_CODE_4M.fd efi-x86_64.fd
```

<!-- test: host linux aarch64 -->
```sh
cp /usr/share/AAVMF/AAVMF_CODE.fd efi-aarch64.fd
```

On macOS:

<!-- test: host macos x86_64 -->
```sh
cp "$(brew --prefix)/share/qemu/edk2-x86_64-code.fd" efi-x86_64.fd
```

<!-- test: host macos aarch64 -->
```sh
cp "$(brew --prefix)/share/qemu/edk2-aarch64-code.fd" efi-aarch64.fd
```

QEMU's `virt` machine wants its flash image to be exactly 64 MiB; on
AArch64, extend the copy (this does nothing to one that already is):

<!-- test: host aarch64 -->
```sh
dd if=/dev/zero of=efi-aarch64.fd bs=1048576 count=0 seek=64
```

## Booting the live image with a blank disk

Work on a copy of the image (the x86-64 firmware writes a small file to
the disk it boots from), and make an empty 1 GiB disk to install on:

<!-- test: host x86_64 -->
```sh
cp cosmoos-x86_64.img live.img
```

<!-- test: host aarch64 -->
```sh
cp cosmoos-aarch64.img live.img
```

<!-- test: host -->
```sh
dd if=/dev/zero of=disk.img bs=1048576 count=0 seek=1024
```

Then boot the live image with the disk attached. The serial console is
your terminal: everything CosmoOS prints appears here, and what you type
goes to its shell. On x86-64:

<!-- test: session x86_64 prompt="cosmo$ " -->
```sh
qemu-system-x86_64 -machine q35 -cpu qemu64,+nx -smp 2 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=efi-x86_64.fd \
    -drive format=raw,file=live.img \
    -drive if=none,id=disk,format=raw,file=disk.img -device virtio-blk-pci,drive=disk \
    -device virtio-rng-pci \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -nographic -no-reboot
```

On AArch64:

<!-- test: session aarch64 prompt="cosmo$ " -->
```sh
qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 2 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=efi-aarch64.fd \
    -drive if=none,id=boot,format=raw,file=live.img -device virtio-blk-pci,drive=boot \
    -drive if=none,id=disk,format=raw,file=disk.img -device virtio-blk-pci,drive=disk \
    -device virtio-rng-pci \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -semihosting-config enable=on,target=native \
    -nographic -no-reboot
```

What the options are for:

| Option | Why |
|---|---|
| `-drive if=pflash,...` | the UEFI firmware |
| `-device virtio-rng-pci` | an entropy source. Without one -- and without a CPU that has RDRAND/RDSEED or RNDR, which QEMU's default models do not -- the random pool never seeds, and `getrandom`, the ssh host key and anything else that needs randomness wait for it ([Known limitations](limitations.md)) |
| `-netdev user` | QEMU's user-mode network; the system configures it by DHCP ([Networking](networking.md)) |
| `isa-debug-exit`, `-semihosting-config` | let QEMU exit when CosmoOS powers off |
| `-nographic` | the serial console on your terminal; type `Ctrl-a x` to leave QEMU by force |

The loader checks the boot files before anything runs (see "Verified
boot" below), the kernel boots, and the live system's shell prompts with
`cosmo$ `. The live system runs from memory: nothing you do in it is kept.

## Installing

List the disks. On x86-64:

<!-- test: console x86_64 -->
```console
cosmo$ cosmo-install --list
NAME         DISK            SECTORS SECTOR  FLAGS
vda          -               2097152    512
ahci0p0      -                135168    512
ahci0p0p1    ahci0p0          131072    512  boot partuuid=...
```

and on AArch64:

<!-- test: console aarch64 -->
```console
cosmo$ cosmo-install --list
NAME         DISK            SECTORS SECTOR  FLAGS
vda          -                135168    512
vdb          -               2097152    512
vda1         vda              131072    512  boot partuuid=...
```

The live image's disk is the one with the `boot` partition (on x86-64
QEMU attaches it to the SATA controller, `ahci0p0`; on AArch64 it is the
first virtio disk). The blank 1 GiB disk is the one of 2097152 sectors of
512 bytes: `vda` on x86-64, `vdb` on AArch64. The kernel logs each
program it starts and the status it exits with (`[ INFO] process: ...`)
on the console between the lines the programs print; this guide leaves
those lines out. Install on the blank disk. On x86-64:

<!-- test: console x86_64 -->
```console
cosmo$ cosmo-install vda
cosmo-install: vda: esp sectors 2048-133119, root sectors 133120-2097118
cosmo-install: copied ... files in ... directories
cosmo-install: installed on vda: esp vda1, root vda2, partuuid ...
```

On AArch64:

<!-- test: console aarch64 -->
```console
cosmo$ cosmo-install vdb
cosmo-install: vdb: esp sectors 2048-133119, root sectors 133120-2097118
cosmo-install: copied ... files in ... directories
cosmo-install: installed on vdb: esp vdb1, root vdb2, partuuid ...
```

`cosmo-install` refuses a disk that already has a partition table unless
you add `--force`, and a disk that is mounted. Power the live system off;
QEMU exits:

<!-- test: console -->
```console
cosmo$ exit
```

## The disk layout

`disk.img` is now a GPT disk with two partitions:

| Partition | Type | Content |
|---|---|---|
| 1 | EFI System Partition (`c12a7328-f81f-11d2-ba4b-00a0c93ec93b`), 64 MiB, FAT32 | a byte-for-byte copy of the live image's: `\EFI\BOOT\BOOTX64.EFI` (`BOOTAA64.EFI`), `\cosmo\kernel.elf`, `\cosmo\boot.tar`, `\cosmo\cmdline`, `\cosmo\manifest` |
| 2 | CosmoOS root (`c9b09224-e00d-418e-846a-9f1d9a61bfd5`), the rest of the disk | a cosmofs filesystem: `/bin`, `/sbin`, `/etc`, `/root`, `/var` (with the package repository in `/var/lib/pkg/repo`), and empty mount points |

The signed command line, `\cosmo\cmdline`, says `root=boot-disk`: the
kernel mounts the CosmoOS root partition of the disk the loader was read
from, and init switches to it. The live image has no such partition, so
it stays on its in-memory root.

## Verified boot

The loader (`BOOTX64.EFI`, `BOOTAA64.EFI`) carries the release's public
key. Before it runs anything it reads `\cosmo\manifest`, checks its
Ed25519 signature against that key, and then checks that `kernel.elf`,
`boot.tar` and `cmdline` have the size and SHA-512 the manifest names.
On the console that is:

```text
verified boot: \cosmo\manifest signed by key ... (...)
verified boot: \cosmo\kernel.elf matches the manifest
```

and the kernel's banner says `Verified boot, key ...`. A changed byte in
any of the three, a manifest signed by another key, or a missing
manifest stops the boot before the kernel runs:

```text
verified boot: rejected \cosmo\<file>: <reason>
cosmoboot: FATAL: verified boot: refusing to boot (status 0x800000000000001a)
```

A loader built from source with `VERIFY_BOOT=0` checks nothing; the
kernel then prints the banner `UNVERIFIED BOOT: the loader was built with
VERIFY_BOOT=0 and checked nothing` and logs it as a warning. The release
images never do. What verified boot does not cover -- the root
filesystem, Secure Boot, rollback -- is in [Known
limitations](limitations.md).

Next: [First boot and the shell](shell.md).
