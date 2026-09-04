#!/bin/sh
# check-reproducible.sh [ARCH] [BUILD]
#
# Build the tree twice into separate output directories and compare the
# resulting kernel ELF and loader EFI byte for byte. A difference means a
# build input leaked into the output (timestamp, absolute path, random
# seed) and must be fixed.
set -eu

arch=${1:-x86_64}
build=${2:-debug}
root=$(cd "$(dirname "$0")/.." && pwd)

a="$root/out/repro-a"
b="$root/out/repro-b"
rm -rf "$a" "$b"

make -C "$root" ARCH="$arch" BUILD="$build" OUT="$a" all >/dev/null
make -C "$root" ARCH="$arch" BUILD="$build" OUT="$b" all >/dev/null

status=0
for f in kernel/kernel.elf boot/BOOTX64.EFI; do
    if cmp -s "$a/$f" "$b/$f"; then
        printf '  same     %s\n' "$f"
    else
        printf '  DIFFERS  %s\n' "$f"
        status=1
    fi
done

if [ $status -eq 0 ]; then
    echo "reproducible: yes"
else
    echo "reproducible: NO" >&2
fi
exit $status
