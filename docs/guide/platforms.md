# Tested platforms

What CosmoOS has been run on, and how much of it works there.

## QEMU (the tested platform)

x86-64 on the `q35` machine and AArch64 on the `virt` machine, QEMU 9.2
or newer, from Linux and macOS: everything in this guide. CI boots both
on every change, and the release workflow runs the guide's commands
against the release images ([How this guide is tested](README.md)).
AArch64 is also booted on QEMU set up unlike virt's defaults -- a GICv3,
no memory above 4 GiB, a CPU with and without the RNDR instruction, no
serial port -- by `make test-platforms`.

## Parallels Desktop 27 on Apple Silicon (AArch64)

The AArch64 image boots to the live shell since v1.0.1. To try it:
create a virtual machine for "Other Linux", ARM64, with UEFI, and put
`cosmoos-aarch64.img` on a disk of the "plain" (raw) type, as its first
SATA disk. The kernel finds the machine from its ACPI tables and logs
one line saying what it found:

```text
platform: firmware OEM 'PRLS  '; console PL011 at 0x2110000 (DBG2, polled); GICv3 GICD 0x2010000 GICR 0x2500000 ITS 0x0 MSI frame 0x2250000; timer PPIs 30 (physical) 27 (virtual); PSCI SMC
```

| | |
|---|---|
| Works | verified boot; both CPUs; the console on the screen and on the VM's serial port (set it to a file or a socket); the USB keyboard; the live shell and every command that needs nothing below; randomness (since v1.0.2: the timing-jitter source seeds the pool in milliseconds, as Parallels offers no other); the wall clock (since v1.0.2: the firmware's time at boot) |
| Does not work yet | **installing**: Parallels' SATA disks are not on PCI, and its NVMe controller (`--iface nvme`) offers only MSI, which the NVMe driver does not use yet; **networking and ssh**: the network adapter is a legacy virtio-net, which the virtio driver does not drive yet |

The details, and why, are in
[`docs/audit/2026-10-13-acpi-discovery-report.md`](../audit/2026-10-13-acpi-discovery-report.md).

## Other machines

Not tested. A machine whose firmware describes its devices in ACPI
(SPCR or DBG2, MADT, GTDT, FADT) gets as far as its devices allow; one
whose tables are missing says which one on the screen and stops.
