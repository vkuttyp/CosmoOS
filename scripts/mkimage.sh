#!/bin/sh
# mkimage.sh OUTPUT.img BOOTX64.EFI kernel.elf [boot.tar]
#
# Build a FAT32 disk image holding the UEFI loader at the removable-media
# fallback path, the kernel where the loader expects it, and the optional
# boot archive (init and boot-time kernel modules, see mkbootarchive.py). Uses mtools so no root or loop
# devices are needed on any host.
set -eu

out=$1
loader=$2
kernel=$3
archive=${4:-}

# 64 MiB is the smallest size mformat reliably formats as FAT32.
size_mib=64

tmp="$out.tmp"
rm -f "$tmp"
dd if=/dev/zero of="$tmp" bs=1048576 count=$size_mib status=none 2>/dev/null \
    || dd if=/dev/zero of="$tmp" bs=1048576 count=$size_mib 2>/dev/null

mformat -i "$tmp" -F -v COSMOOS ::
mmd -i "$tmp" ::/EFI ::/EFI/BOOT ::/cosmo
mcopy -i "$tmp" "$loader" "::/EFI/BOOT/$(basename "$loader")"
mcopy -i "$tmp" "$kernel" ::/cosmo/kernel.elf
if [ -n "$archive" ]; then
    mcopy -i "$tmp" "$archive" ::/cosmo/boot.tar
fi

mv "$tmp" "$out"
