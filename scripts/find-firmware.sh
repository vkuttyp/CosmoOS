#!/bin/sh
# find-firmware.sh [x86_64]
#
# Print the path of an OVMF/EDK2 UEFI firmware code image for QEMU.
# Honors $OVMF_CODE if set. Exit 1 if nothing is found.
set -eu

arch=${1:-x86_64}

if [ -n "${OVMF_CODE:-}" ]; then
    if [ -f "$OVMF_CODE" ]; then
        echo "$OVMF_CODE"
        exit 0
    fi
    echo "find-firmware: OVMF_CODE=$OVMF_CODE does not exist" >&2
    exit 1
fi

case "$arch" in
x86_64)
    candidates="
        /usr/share/OVMF/OVMF_CODE_4M.fd
        /usr/share/OVMF/OVMF_CODE.fd
        /usr/share/edk2/ovmf/OVMF_CODE.fd
        /usr/share/edk2-ovmf/x64/OVMF_CODE.fd
        /usr/share/edk2/x64/OVMF_CODE.4m.fd
        /usr/share/edk2/x64/OVMF_CODE.fd
        /usr/share/qemu/edk2-x86_64-code.fd
        /usr/share/qemu/OVMF.fd
        /opt/homebrew/share/qemu/edk2-x86_64-code.fd
        /usr/local/share/qemu/edk2-x86_64-code.fd
    "
    ;;
*)
    echo "find-firmware: unsupported architecture $arch" >&2
    exit 1
    ;;
esac

for c in $candidates; do
    if [ -f "$c" ]; then
        echo "$c"
        exit 0
    fi
done

# Last resort: ask QEMU where its data directory is.
if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    datadir=$(qemu-system-x86_64 -L help 2>/dev/null | head -n 1 || true)
    if [ -n "$datadir" ] && [ -f "$datadir/edk2-x86_64-code.fd" ]; then
        echo "$datadir/edk2-x86_64-code.fd"
        exit 0
    fi
fi

echo "find-firmware: no UEFI firmware found; install ovmf/edk2 or set OVMF_CODE" >&2
exit 1
