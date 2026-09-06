#!/bin/sh
# qemu-run.sh IMAGE
#
# Boot the CosmoOS disk image under QEMU with UEFI firmware, serial on the
# terminal, no graphics. Environment:
#   QEMU_MEM    guest RAM (default 256M)
#   QEMU_ARCH   x86_64 (default) or aarch64: selects the machine (q35 or virt)
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
arch=${QEMU_ARCH:-x86_64}
firmware=$("$here/find-firmware.sh" "$arch")

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
# Milestone 9: an NVMe controller with one 8 MiB namespace (the nvme
# self-test writes to it; the harness gives it a fresh file per run).
nvmedisk=${QEMU_NVMEDISK:-$outdir/nvme.img}
if [ ! -f "$nvmedisk" ]; then
    dd if=/dev/zero of="$nvmedisk" bs=1048576 count=8 status=none 2>/dev/null \
        || dd if=/dev/zero of="$nvmedisk" bs=1048576 count=8 2>/dev/null
fi

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

if [ "$arch" = aarch64 ]; then
    # The virt machine wants a 64 MiB flash image; pad smaller firmware files.
    padded="$outdir/firmware-aarch64.fd"
    if [ ! -f "$padded" ] || [ "$firmware" -nt "$padded" ]; then
        cp "$firmware" "$padded.tmp"
        python3 -c "import sys; f=open(sys.argv[1],'r+b'); f.truncate(64*1024*1024)" "$padded.tmp"
        mv "$padded.tmp" "$padded"
    fi
    # GICv2 with a GICv2m MSI frame; semihosting carries the exit status
    # (docs/kernel/arch/aarch64/design.md). The scratch disk comes first so it
    # is vda for the storage self-tests, as on x86; the boot image is read-only.
    # An SMMUv3 in front of the PCI root complex (kernel/iommu); QEMU_IOMMU=0 leaves it out.
    iommu_machine=""
    [ "${QEMU_IOMMU:-1}" != "0" ] && iommu_machine=",iommu=smmuv3"
    exec qemu-system-aarch64 \
        -machine "virt,gic-version=2${iommu_machine},accel=${QEMU_ACCEL:-tcg}" \
        -cpu "${QEMU_CPU:-cortex-a72}" \
        -smp "${QEMU_SMP:-4}" \
        -m "${QEMU_MEM:-256M}" \
        -drive if=pflash,format=raw,readonly=on,file="$padded" \
        -drive if=none,id=testdisk,format=raw,file="$testdisk" \
        -device virtio-blk-pci,drive=testdisk \
        -drive if=none,id=boot,format=raw,readonly=on,file="$image" \
        -device virtio-blk-pci,drive=boot \
        -drive if=none,id=nvme0,format=raw,file="$nvmedisk" \
        -device nvme,drive=nvme0,serial=cosmo-nvme0 \
        -device virtio-rng-pci \
        -device virtio-serial-pci \
        -chardev file,id=vcon,path="$vcon" \
        -device virtconsole,chardev=vcon \
        -netdev "$netdev" \
        -device virtio-net-pci,netdev=n0,mac=52:54:00:c0:5f:05 \
        $fwcfg \
        $pcap \
        -semihosting-config enable=on,target=native \
        -serial stdio \
        -display none \
        -monitor none \
        -no-reboot \
        ${QEMU_EXTRA:-}
fi

# An Intel IOMMU (VT-d, DMA remapping only: intremap=off) in front of the
# PCI devices (kernel/iommu); QEMU_IOMMU=0 leaves it out.
iommu_dev=""
[ "${QEMU_IOMMU:-1}" != "0" ] && iommu_dev="-device intel-iommu,intremap=off"
exec qemu-system-x86_64 \
    -machine q35,accel="${QEMU_ACCEL:-tcg}" \
    $iommu_dev \
    -cpu "${QEMU_CPU:-qemu64,+nx,+svm,+npt}" \
    -smp "${QEMU_SMP:-4}" \
    -m "${QEMU_MEM:-256M}" \
    -drive if=pflash,format=raw,readonly=on,file="$firmware" \
    -drive format=raw,file="$image" \
    -drive if=none,id=testdisk,format=raw,file="$testdisk" \
    -device virtio-blk-pci,drive=testdisk \
    -drive if=none,id=nvme0,format=raw,file="$nvmedisk" \
    -device nvme,drive=nvme0,serial=cosmo-nvme0 \
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
