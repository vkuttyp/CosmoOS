# drivers/storage

The `ahci` module (`docs/drivers/ahci/`): SATA disks through an AHCI
host bus adapter -- on `q35`, the ICH9 the machine has always had, which
also carries the boot image. One `struct blkdev` per port with a SATA
disk, named `ahci<controller>p<port>`, whose DMA device is the controller:
a port has no identity of its own, so it is not a `struct device`, which
is the answer to the question the USB unit left open. Non-queued
commands in the HBA's 32 slots; NCQ was measured not to pay on this host
(`testing.md`, "Benchmarks"). ATAPI and port multipliers are refused with
one line each.
