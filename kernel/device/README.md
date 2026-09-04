# kernel/device

The device model (constitution section 25): buses, devices, drivers,
resources, probing (`device.c`); the DMA API (section 26: `dma.c`,
coherent allocations and mappings, no IOMMU yet); and the Phase 6
self-tests (`devtest.c`: device model, PCI, DMA, entropy, block,
virtio console). Bus drivers live under `drivers/` (PCI in the kernel,
VirtIO as modules); the block layer is `kernel/block/`.
Documentation: `docs/kernel/device/`.
