#!/bin/sh
# qemu-run.sh IMAGE
#
# Boot the CosmoOS disk image under QEMU with UEFI firmware, serial on the
# terminal, no graphics. Environment:
#   QEMU_MEM    guest RAM (default 256M)
#   QEMU_ACCEL  accelerator (default tcg; kvm/hvf where available)
#   QEMU_CPU    CPU model (default qemu64,+nx,+svm,+npt: TCG emulates AMD-V with nested
#               paging, which the virtualization tests need; use `host` with kvm/hvf)
#   QEMU_EXTRA  extra QEMU arguments
#   OVMF_CODE   firmware image override
#
# The isa-debug-exit device lets the kernel terminate QEMU with an exit
# status: QEMU exits with (value << 1) | 1 for a write of `value`.
set -eu

image=$1
here=$(cd "$(dirname "$0")" && pwd)
firmware=$("$here/find-firmware.sh" x86_64)

# Phase 6 devices: a scratch virtio-blk disk (8 MiB, created next to the
# image unless QEMU_TESTDISK names one), a virtio-rng, and a virtio
# console whose output lands in QEMU_VCON (default: vcon.log next to
# the image) so the boot test can read it back.
outdir=$(dirname "$image")
testdisk=${QEMU_TESTDISK:-$outdir/testdisk.img}
if [ ! -f "$testdisk" ]; then
    dd if=/dev/zero of="$testdisk" bs=1048576 count=8 status=none 2>/dev/null \
        || dd if=/dev/zero of="$testdisk" bs=1048576 count=8 2>/dev/null
fi
vcon=${QEMU_VCON:-$outdir/vcon.log}
: > "$vcon"

# Phase 8: QEMU user-mode networking on a virtio-net NIC. The harness
# adds host port forwards (QEMU_NET_HOSTFWD, a comma-separated list of
# "tcp:127.0.0.1:P-:7" style rules) and passes its own listening port to
# the guest through fw_cfg (QEMU_FWCFG_NETTEST, e.g. "tcp=34567").
netdev="user,id=n0,ipv4=on,ipv6=on"
if [ -n "${QEMU_NET_HOSTFWD:-}" ]; then
    for rule in $(printf '%s' "$QEMU_NET_HOSTFWD" | tr ',' ' '); do
        netdev="$netdev,hostfwd=$rule"
    done
fi
fwcfg=""
if [ -n "${QEMU_FWCFG_NETTEST:-}" ]; then
    fwcfg="-fw_cfg name=opt/cosmo/nettest,string=$QEMU_FWCFG_NETTEST"
fi
# QEMU_PCAP=file.pcap records every frame on the guest NIC (debugging).
pcap=""
if [ -n "${QEMU_PCAP:-}" ]; then
    pcap="-object filter-dump,id=f0,netdev=n0,file=$QEMU_PCAP"
fi

exec qemu-system-x86_64 \
    -machine q35,accel="${QEMU_ACCEL:-tcg}" \
    -cpu "${QEMU_CPU:-qemu64,+nx,+svm,+npt}" \
    -smp "${QEMU_SMP:-4}" \
    -m "${QEMU_MEM:-256M}" \
    -drive if=pflash,format=raw,readonly=on,file="$firmware" \
    -drive format=raw,file="$image" \
    -drive if=none,id=testdisk,format=raw,file="$testdisk" \
    -device virtio-blk-pci,drive=testdisk \
    -device virtio-rng-pci \
    -device virtio-serial-pci \
    -chardev file,id=vcon,path="$vcon" \
    -device virtconsole,chardev=vcon \
    -netdev "$netdev" \
    -device virtio-net-pci,netdev=n0,mac=52:54:00:c0:5f:05 \
    $fwcfg \
    $pcap \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial stdio \
    -display none \
    -monitor none \
    -no-reboot \
    ${QEMU_EXTRA:-}
