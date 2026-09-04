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
- `nvme/`, `network/`, `storage/`: later phases.
