#!/bin/sh
# qemu-run.sh IMAGE
#
# Boot the CosmoOS disk image under QEMU with UEFI firmware, serial on the
# terminal, no graphics. Environment:
#   QEMU_MEM    guest RAM (default 256M)
#   QEMU_ACCEL  accelerator (default tcg; kvm/hvf where available)
#   QEMU_EXTRA  extra QEMU arguments
#   OVMF_CODE   firmware image override
#
# The isa-debug-exit device lets the kernel terminate QEMU with an exit
# status: QEMU exits with (value << 1) | 1 for a write of `value`.
set -eu

image=$1
here=$(cd "$(dirname "$0")" && pwd)
firmware=$("$here/find-firmware.sh" x86_64)

exec qemu-system-x86_64 \
    -machine q35,accel="${QEMU_ACCEL:-tcg}" \
    -cpu qemu64,+nx \
    -smp "${QEMU_SMP:-4}" \
    -m "${QEMU_MEM:-256M}" \
    -drive if=pflash,format=raw,readonly=on,file="$firmware" \
    -drive format=raw,file="$image" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial stdio \
    -display none \
    -monitor none \
    -no-reboot \
    ${QEMU_EXTRA:-}
