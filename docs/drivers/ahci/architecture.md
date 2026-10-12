# AHCI driver: architecture

The subsystem `docs/audit/next-subsystem-ahci.md` named: SATA disks
through an AHCI host bus adapter. Constitution sections 25 (drivers
never touch bus layout themselves), 26 (DMA through the API), 28 (the
block layer knows no driver) and 21 (NCQ not written without a
benchmark that shows it pays). Written to AHCI 1.3.1 (bring-up §10.1,
error recovery §6.2.2.1) and ATA8-ACS.

## Where it sits

```text
   filesystems / self-tests     blk_read, blk_write, blk_submit
        │
   kernel/block/blk.c           validation, pending queue, timeouts
        │                       struct blkdev "ahci<c>p<p>", one per disk
   drivers/storage/ahci.c  (module ahci)
        HBA bring-up, ports, 32 command slots, IDENTIFY,
        IRQ handler, worker thread ahci/<n> (hotplug, recovery)
        │
   drivers/pci   BAR5 (ABAR), MSI-X/MSI    kernel/device/dma.c   command
                                           lists, FIS areas, tables, data
```

The driver is a boot module (`modules/ahci.ko`, source list
`MODULE_ahci_SRCS` in `build/module.mk`, the last driver in the boot
archive), declared with `COSMO_MODULE("ahci", ..., MODULE_CAP_DRIVER)`;
it reaches the kernel only through exported symbols. Register offsets,
the command header, FIS and PRDT layouts are in
`drivers/storage/ahci.h`. On `q35` it binds the built-in ICH9 AHCI at
`pci:00:1f.2`, whose port 0 carries the boot image; on `virt`
`scripts/qemu-run.sh` adds `-device ahci`. The harness's 8 MiB test
disk is on port 1 on both machines, so it is `ahci0p1` (api.md,
"QEMU").

## Purpose

Drive SATA disks behind an AHCI controller as block devices, one
`struct blkdev` per port with a disk, whose DMA device is the
controller: a port has no identity of its own, so it is not a
`struct device` (invariant A1, design.md "What this unit answers").

## Responsibilities

- Binding PCI class `01/06` and accepting only programming interface
  `01` (`ahci_ids`, `ahci_probe`).
- Controller bring-up: BIOS/OS handoff when `CAP2.BOH`, `GHC.AE`, HBA
  reset, `CAP` facts, the DMA mask (64-bit when `CAP.S64A`), one
  interrupt (MSI-X entry 0, else single-message MSI), and `GHC.IE` only
  after the ports and the worker exist (A7).
- Per port: stop, allocate and program the command list, received-FIS
  area and command tables (`port_alloc`, `port_init`), start, and read
  `PxSSTS`/`PxSIG` to classify the device (`port_probe`).
- Disks: `IDENTIFY DEVICE`, geometry, LBA48, write cache, and
  registration under the exact name `ahci<c>p<p>` (`disk_identify`,
  `disk_attach`).
- The data path: `READ DMA EXT`, `WRITE DMA EXT` and `FLUSH CACHE EXT`,
  one non-queued command per bio in a free slot, a PRDT of up to
  `AHCI_PRDT_MAX` (56) segments; `-EAGAIN` when every slot is taken
  (`ahci_submit`, A3).
- Completion in the handler (`ahci_irq`); task-file error recovery,
  COMRESET and hotplug on the worker (`ahci_worker`, `port_recover`,
  `port_comreset`); the block layer's timeout (`ahci_timeout`,
  `port_restart`) (A2, A4, A5, A9).
- Removal that releases port DMA only after both engines acknowledge
  the stop, else marks the function `device_retain_dma`
  (`ahci_remove`, A8).

## Non-responsibilities

- NCQ: the IDENTIFY facts are logged but `READ/WRITE FPDMA QUEUED` is
  not written; the benchmark in testing.md says it does not pay here.
- ATAPI devices, port multipliers, unknown signatures and controllers
  without MSI-X or MSI: refused with one log line each (A6).
- Power management beyond `PxCMD.SUD|POD`, stable naming by serial or
  label, and more than 256 sectors per command (design.md, "What is
  deliberately not here").
- Queuing beyond the slots, ordering and flush semantics: the block
  layer's pending queue and the filesystems own them.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `struct blkdev_ops` (`submit`, `release`, `timeout`) | `kernel/blk.h` | the block layer calls in |
| `blkdev_ops.debug_dma`, `blkdev_ops.debug_presence` | `kernel/blk.h` | IOMMU and hotplug self-tests |
| `blk_register_named`, `blk_unregister`, `bio_complete`, `bio_segments`, `bio_segment` | `kernel/blk.h` | the driver calls out |
| `pci_register_driver`, `pci_enable_device`, `pci_map_bar`, `pci_msix_enable/request/disable`, `pci_msi_enable/disable` | `drivers/pci.h` | probe/remove |
| `dma_set_mask`, `dma_alloc/free`, `dma_map/unmap` | `kernel/dma.h` | port memory, data |
| `thread_create`, `completion_*`, `synchronize_irq` | kernel exports | worker, synchronous commands, removal |
| `device_retain_dma` | `kernel/device.h` | stop not acknowledged (A8) |
| `ahci_test_comreset_recovery`, `ahci_test_comreset_restart`, `ahci_test_error_pending` | `CONFIG_SELFTEST` exports | `ahci-comreset-ack`, `ahci-error-reset` |
| `FI_AHCI_CI`, `FI_AHCI_PROBE_ROLLBACK`, `FI_AHCI_STOP_ACK`, `FI_AHCI_COMRESET_ACK` | `kernel/faultinject.h` | the AHCI self-tests |
