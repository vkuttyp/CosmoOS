# drivers/virtio

VirtIO as a normal device subsystem (constitution section 27), entirely
in kernel modules loaded from the boot archive:

| Module | Sources | Provides |
|---|---|---|
| `virtio` | `virtio.c`, `virtqueue.c`, `virtio_pci.c` | the `virtio` bus, device initialisation (spec 3.1.1), split virtqueues, the virtio-pci modern transport with one MSI-X vector per queue; 15 exported symbols |
| `virtio_blk` | `virtio_blk.c` | block device `vda` on the block layer |
| `virtio_rng` | `virtio_rng.c` | feeds the kernel entropy pool (4 KiB per boot) |
| `virtio_console` | `virtio_console.c` | a polled console sink (`virtio-console`) |

`virtio_net` waits for the networking phase. Independent of CPU
virtualization (invariant 9). Header: `drivers/include/drivers/virtio.h`.
Documentation: `docs/drivers/virtio/`.
