#!/bin/sh
# mkimage.sh OUTPUT.img BOOTX64.EFI kernel.elf boot.tar cmdline manifest
#
# Build the boot disk: a GPT disk whose partition 1 is a FAT32 EFI System
# Partition holding the UEFI loader at the removable-media fallback path,
# the kernel where the loader expects it, the boot archive (init and
# boot-time kernel modules, see mkbootarchive.py), the command line
# \cosmo\cmdline and the signed manifest \cosmo\manifest over those three
# (mkmanifest.py; docs/boot/design.md, "Verified boot"). Uses mtools and
# scripts/mkgpt.py, so no root or loop devices are needed on any host. The
# installer copies partition 1 byte for byte (roadmap M2, M5).
set -eu

out=$1
loader=$2
kernel=$3
archive=$4
cmdline=$5
manifest=$6
here=$(cd "$(dirname "$0")" && pwd)

# 64 MiB is the smallest size mformat reliably formats as FAT32.
size_mib=64

esp="$out.esp.tmp"
rm -f "$esp"
dd if=/dev/zero of="$esp" bs=1048576 count=$size_mib status=none 2>/dev/null \
    || dd if=/dev/zero of="$esp" bs=1048576 count=$size_mib 2>/dev/null

# A fixed volume serial: the image is a function of its inputs.
mformat -i "$esp" -F -v COSMOOS -N 434f534d ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/cosmo
mcopy -i "$esp" "$loader" "::/EFI/BOOT/$(basename "$loader")"
mcopy -i "$esp" "$kernel" ::/cosmo/kernel.elf
mcopy -i "$esp" "$archive" ::/cosmo/boot.tar
mcopy -i "$esp" "$cmdline" ::/cosmo/cmdline
mcopy -i "$esp" "$manifest" ::/cosmo/manifest

python3 "$here/mkgpt.py" "$out.tmp" "$esp"
rm -f "$esp"
mv "$out.tmp" "$out"
