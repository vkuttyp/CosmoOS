# drivers

Device drivers built on the Device/Driver/Bus/Resource/DMA/Interrupt
framework in `kernel/device/`. Drivers contain no generic bus logic.

- `include/drivers/`: headers drivers and modules share (`pci.h`,
  `virtio.h`); on the kernel and module include paths.
- `acpi/`: static ACPI tables (RSDP, XSDT, MADT, MCFG lookup), in the
  kernel image.
- `pci/`: the PCI core (kernel image; drivers are modules and need the
  bus first). `docs/drivers/pci/`.
- `virtio/`: the `virtio` module and the `virtio_blk`, `virtio_rng`,
  `virtio_console` driver modules, built by `build/module.mk` and loaded
  from the boot archive. `docs/drivers/virtio/`.
- `nvme/`: the `nvme` module (milestone 9). `docs/drivers/nvme/`.
- `network/`: the `e1000e` module, the first NIC that is not virtio. `docs/drivers/e1000e/`.
- `usb/`: the `xhci` module (USB core and controller driver) and the `usb_storage` module: the first bus whose devices arrive, leave and have a parent. `docs/drivers/usb/`.
- `storage/`: the `ahci` module: SATA disks through an AHCI host bus adapter, one blkdev per port (`ahci0p1`), the controller as the DMA device. `docs/drivers/ahci/`.
