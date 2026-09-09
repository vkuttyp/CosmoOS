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
# The key test tells the guest that something will type on the keyboard,
# so the self-test waits for characters instead of skipping.
if [ -n "${QEMU_FWCFG_KEYTEST:-}" ]; then
    fwcfg="$fwcfg -fw_cfg name=opt/cosmo/keytest,string=$QEMU_FWCFG_KEYTEST"
fi
# QEMU_ASID=paranoid runs the whole boot with address-space tags allocated
# and written but every switch flushing anyway (kernel/asid.h): a run in
# which no translation survives a switch, so an isolation failure that
# appears only without it is a stale translation by construction.
if [ -n "${QEMU_ASID:-}" ]; then
    fwcfg="$fwcfg -fw_cfg name=opt/cosmo/asid,string=$QEMU_ASID"
fi
# The NICs (docs/drivers/e1000e/api.md). QEMU_NIC: both (default: virtio-net
# on n0 as eth0 and an Intel 82574L on its own backend as eth1), virtio (as
# before the e1000e driver), or e1000e (the Intel NIC alone, on n0, so it
# is eth0 and the default interface and the whole network suite runs over
# it). Port forwards and the pcap filter are on n0 whichever device it is.
nic=${QEMU_NIC:-both}
case "$nic" in
virtio) nic_devs="-netdev $netdev -device virtio-net-pci,netdev=n0,mac=52:54:00:c0:5f:05" ;;
e1000e) nic_devs="-netdev $netdev -device e1000e,netdev=n0,mac=52:54:00:c0:5f:06" ;;
*) nic_devs="-netdev $netdev -device virtio-net-pci,netdev=n0,mac=52:54:00:c0:5f:05 -netdev user,id=n1,ipv4=on,ipv6=on -device e1000e,netdev=n1,mac=52:54:00:c0:5f:06" ;;
esac
# USB (docs/drivers/usb/api.md): an xHCI controller with a mass-storage
# device on it, backed by an 8 MiB image beside the other disks (the
# usb-storage self-test writes to it). QEMU_USB: qemu (default, the
# qemu-xhci model), nec (the NEC uPD720200 model, nec-usb-xhci), or 0
# (no controller: the USB tests skip).
usb=${QEMU_USB:-qemu}
usbdisk=${QEMU_USBDISK:-$outdir/usb.img}
usb_devs=""
if [ "$usb" != "0" ]; then
    if [ ! -f "$usbdisk" ]; then
        dd if=/dev/zero of="$usbdisk" bs=1048576 count=8 status=none 2>/dev/null \
            || dd if=/dev/zero of="$usbdisk" bs=1048576 count=8 2>/dev/null
    fi
    case "$usb" in
    nec) xhci_model=nec-usb-xhci ;;
    *)   xhci_model=qemu-xhci ;;
    esac
    usb_devs="-device $xhci_model,id=xhci0 -drive if=none,id=usbdisk,format=raw,file=$usbdisk -device usb-storage,bus=xhci0.0,drive=usbdisk"
    # The keyboard (docs/drivers/usb/api.md, "The keyboard"): QEMU_KBD is
    # root (default: a usb-kbd on a root port), hub (the same keyboard
    # behind a usb-hub, which is where the route string is exercised), or
    # 0 (no keyboard: the key tests skip). Keys are injected over QMP by
    # the boot test; there is no window and no host keyboard involved.
    case "${QEMU_KBD:-root}" in
    0)   ;;
    hub) usb_devs="$usb_devs -device usb-hub,id=hub0,bus=xhci0.0,port=2 -device usb-kbd,bus=xhci0.0,port=2.1" ;;
    *)   usb_devs="$usb_devs -device usb-kbd,bus=xhci0.0,port=3" ;;
    esac
fi
# QMP, so the boot test can inject key events (input-send-event). Nothing
# else uses it, and without QEMU_QMP the socket is not created at all.
qmp=""
if [ -n "${QEMU_QMP:-}" ]; then
    qmp="-qmp unix:$QEMU_QMP,server=on,wait=off"
fi
# SATA (docs/drivers/ahci/api.md): a disk on an AHCI controller -- q35's
# built-in ICH9 (its ports are ide.0..ide.5; the boot image above sits on
# ide.0, which is where q35 puts a plain -drive, so the test disk goes on
# port 1), or -device ahci on virt (port 1 too, so the disk has the same
# name on both machines) -- backed by an 8 MiB image beside the others.
# QEMU_SATA: disk (default), cd (an ATAPI device instead: the driver
# refuses it), or 0 (no disk; q35 keeps its controller with only the boot
# image on it, virt has none).
sata=${QEMU_SATA:-disk}
satadisk=${QEMU_SATADISK:-$outdir/sata.img}
sata_drive=""
sata_dev_x86=""
sata_dev_a64=""
if [ "$sata" != "0" ]; then
    if [ ! -f "$satadisk" ]; then
        dd if=/dev/zero of="$satadisk" bs=1048576 count=8 status=none 2>/dev/null \
            || dd if=/dev/zero of="$satadisk" bs=1048576 count=8 2>/dev/null
    fi
    case "$sata" in
    cd) sata_model=ide-cd ;;
    *)  sata_model=ide-hd ;;
    esac
    sata_drive="-drive if=none,id=sata0,format=raw,file=$satadisk"
    sata_dev_x86="-device $sata_model,drive=sata0,bus=ide.1"
    sata_dev_a64="-device ahci,id=ahci0 -device $sata_model,drive=sata0,bus=ahci0.1"
fi
# The display (docs/kernel/diagnostics/design.md, "The framebuffer
# console"): whatever device the firmware lights and hands the loader as
# a Graphics Output Protocol framebuffer, which the kernel then draws on.
# QEMU_DISPLAY: on (default -- q35 keeps QEMU's built-in VGA at 1280x800;
# virt gets a ramfb at 800x600, since edk2 for AArch64 carries the ramfb
# driver), virtio (a virtio-gpu-pci: edk2's driver offers a Blt-only mode
# with no linear buffer, which the loader refuses), bochs (a
# bochs-display: a framebuffer on x86, and no GOP at all under AAVMF), or
# 0 (no display device: the kernel keeps the serial console alone).
# There is never a window: -display none is always passed.
display=${QEMU_DISPLAY:-on}
display_dev_x86=""
display_dev_a64=""
case "$display" in
0)      display_dev_x86="-vga none" ;;
bochs)  display_dev_x86="-vga none -device bochs-display"
        display_dev_a64="-device bochs-display" ;;
virtio) display_dev_x86="-vga none -device virtio-gpu-pci"
        display_dev_a64="-device virtio-gpu-pci" ;;
*)      display_dev_a64="-device ramfb" ;;
esac
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
    # The interrupt controller. QEMU_GIC selects the distributor version
    # (2, QEMU's default, or 3/4) and QEMU_MSI the way an MSI reaches it:
    # its, gicv2m, or off. `msi=off` is not a configuration this tree can
    # boot -- no driver here falls back to INTx
    # (docs/audit/next-subsystem-gicv3.md) -- and is offered only so that
    # the decline path can be exercised deliberately.
    #
    # The `msi` property is newer than the machine: QEMU 10 and earlier
    # have only `its=on|off`, and asking for `msi=` there fails with
    # "Property 'virt-N-machine.msi' not found" half a second into the
    # boot. Say so here instead, because the useful configuration --
    # gic-version=3 with an ITS -- needs no property at all: an ITS is
    # what that machine builds by default.
    gic_msi=""
    if [ -n "${QEMU_MSI:-}" ]; then
        if qemu-system-aarch64 -machine virt,help 2>&1 | grep -q '^  *msi='; then
            gic_msi=",msi=${QEMU_MSI}"
        else
            echo "qemu-run: this QEMU's virt machine has no 'msi' property (needs QEMU 11 or newer);" >&2
            echo "qemu-run: QEMU_MSI=${QEMU_MSI} cannot be honoured. Drop it for the ITS, which is" >&2
            echo "qemu-run: what gic-version=3 builds by default." >&2
            exit 1
        fi
    fi
    iommu_machine=""
    [ "${QEMU_IOMMU:-1}" != "0" ] && iommu_machine=",iommu=smmuv3"
    # The virtualization extensions: firmware then hands the loader EL2,
    # which it keeps for guests (docs/kernel/arch/aarch64/design.md,
    # "Exception level 2"). QEMU_EL2=0 boots at EL1 as before.
    el2_machine=""
    [ "${QEMU_EL2:-1}" != "0" ] && el2_machine=",virtualization=on"
    exec qemu-system-aarch64 \
        -machine "virt,gic-version=${QEMU_GIC:-2}${gic_msi}${iommu_machine}${el2_machine},accel=${QEMU_ACCEL:-tcg}" \
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
        $nic_devs \
        $usb_devs \
        $sata_drive $sata_dev_a64 \
        $display_dev_a64 \
        $qmp \
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
    $nic_devs \
    $usb_devs \
    $sata_drive $sata_dev_x86 \
    $display_dev_x86 \
    $qmp \
    $fwcfg \
    $pcap \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial stdio \
    -display none \
    -monitor none \
    -no-reboot \
    ${QEMU_EXTRA:-}
