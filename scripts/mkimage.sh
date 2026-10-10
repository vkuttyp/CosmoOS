#!/bin/sh
# mkimage.sh OUTPUT.img BOOTX64.EFI kernel.elf [boot.tar]
#
# Build the boot disk: a GPT disk whose partition 1 is a FAT32 EFI System
# Partition holding the UEFI loader at the removable-media fallback path,
# the kernel where the loader expects it, the optional boot archive (init
# and boot-time kernel modules, see mkbootarchive.py), and the command-line
# slot \cosmo\cmdline (docs/boot/design.md, "The command line"). Uses
# mtools and scripts/mkgpt.py, so no root or loop devices are needed on
# any host. The installer copies partition 1 as it is (roadmap M2).
set -eu

out=$1
loader=$2
kernel=$3
archive=${4:-}
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
if [ -n "$archive" ]; then
    mcopy -i "$esp" "$archive" ::/cosmo/boot.tar
fi
# The command-line slot: one sector, the marker line, then NULs. The
# loader passes the text up to the first NUL; the installer finds the
# sector by its marker and rewrites it in place, so it never has to write
# FAT (docs/boot/design.md, "The command line").
slot="$out.cmdline.tmp"
python3 -c "import sys; t=b'#cosmo-cmdline v1\n'; open(sys.argv[1],'wb').write(t+bytes(512-len(t)))" "$slot"
mcopy -i "$esp" "$slot" ::/cosmo/cmdline
rm -f "$slot"

python3 "$here/mkgpt.py" "$out.tmp" "$esp"
rm -f "$esp"
mv "$out.tmp" "$out"
