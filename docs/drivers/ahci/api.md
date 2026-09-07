# AHCI driver: API

The driver has no interface of its own. It is a module that binds a PCI
class and registers a `struct blkdev` per SATA disk; everything a user
or another subsystem sees comes through the block layer's existing
surface.

## Binding

| | |
| --- | --- |
| Module | `ahci` (`modules/ahci.ko`, `MODULE_CAP_DRIVER`, no dependencies) |
| Bus | `pci`, class `01/06` (mass storage, SATA) with programming interface `01` (AHCI) checked in probe; another interface is refused `-ENODEV` |
| Registers | one `struct blkdev` per port with a SATA disk, exact name `ahci<controller>p<port>` (`ahci0p1`), `dev` = the controller's PCI function, `sector_size` and `capacity` from IDENTIFY, `max_sectors` 256, `max_segments` 56, `timeout_ns` 10 s |
| Interrupt | one MSI-X vector (entry 0) where the function has MSI-X, else its single-message MSI; on CPU 0; no INTx |
| Refused | ATAPI devices, port multipliers, unknown signatures, geometries outside 512–4096 bytes a sector, controllers with neither MSI-X nor MSI — one log line each |

## Test-only operations (`struct blkdev_ops`)

**`debug_dma(bd, addr)`** — a `READ DMA EXT` of one sector into the bus
address the caller chose, so the IOMMU fault test can make the
controller DMA somewhere it may not.

**`debug_presence(bd, present)`** — the driver's own hotplug paths on
demand. `false` on a live disk: the port is stopped, every command it
holds completes `-ENODEV`, the blkdev is unregistered (the hardware
stays attached). `true` on an unregistered blkdev: it only names the
controller and port; the port is probed and a *new* blkdev registered
under the same name — the old object is never re-registered. `true` on a
live disk: the port is stopped, the commands it holds complete `-EIO`, a
COMRESET is issued, the disk re-identified, and the same blkdev keeps
serving if the same serial, capacity and sector size answer (else it is
detached and the port probed anew).

## Log lines

```text
[ INFO] ahci0: pci:00:1f.2: AHCI 1.0, 6 port(s) implemented of 6, 32 slots, 64-bit DMA, NCQ capable
[ INFO] ahci0: port 1: QEMU HARDDISK (QM00003) is ahci0p1: 16384 sectors of 512 bytes, LBA48, write cache, NCQ yes depth 32
[ INFO] blk: ahci0p1: 16384 sectors of 512 bytes (8 MiB)
[ INFO] ahci0: port 1: an ATAPI device (signature 0xeb140101) is not driven
[ INFO] ahci0: no disk on any port
[ INFO] ahci0: port 1: ahci0p1 removed (N issued, N completed, N errors, N resets)
[ WARN] ahci0: port 1: command timed out (PxCI 0x..., PxTFD 0x...); restarting the port
[ WARN] ahci0: port 1: task file error (PxTFD 0x..., PxSERR 0x...) in slot S; N command(s) reissued
[ INFO] ahci0: port 1: a different disk answered after the reset (SERIAL)
[ERROR] ahci0: port P: IDENTIFY DEVICE failed (-errno)
```

The boot test requires the module to load always; with a disk
(`QEMU_SATA=disk`, the default) the controller line and the port 1
lines; on `q35` also port 0, which carries the boot image (a plain
`-drive` on `q35` is a SATA disk on the ICH9's first port, so the driver
drives the medium it booted from); with `QEMU_SATA=cd` the ATAPI line.

## Counters

`struct blkdev` counts reads, writes, flushes, errors, timeouts,
requeued and redrained for each disk as for every disk; the driver adds
nothing to them. Per port it keeps `issued`, `completed`, `errors`
(timeouts and task-file errors) and `resets` (COMRESETs), reported when
the disk is removed.

## QEMU

`scripts/qemu-run.sh`: on `q35` the ICH9 AHCI is built in and the boot
image already sits on its port 0 (`ide.0`), so the 8 MiB test disk goes
on port 1 (`-device ide-hd,drive=sata0,bus=ide.1`); on `virt`, `-device
ahci,id=ahci0` first and the disk on `ahci0.1`, so the disk is `ahci0p1`
on both machines. The image is `QEMU_SATADISK` (default `sata.img`
beside the others). `QEMU_SATA` selects `disk` (default), `cd` (an
`ide-cd` instead: the ATAPI refusal), or `0` (no test disk; `q35` keeps
the controller with the boot image on it, `virt` has no controller and
the driver binds nothing).

## Kernel changes for this module

None to any interface a driver or a filesystem uses. Two additions of
the test-only kind: `blkdev_ops.debug_presence` (optional, the shape
`debug_dma` already had) and the fault-injection kind `FI_AHCI_CI` (a
slot is filled and its `PxCI` bit never written). No new exports: the
module uses what NVMe and USB storage already needed.
