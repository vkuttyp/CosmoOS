/*
 * cosmo/hv_machine.h - The machine a CosmoOS hypervisor guest is handed
 * (docs/kernel-services/virtualization/design.md, "The machine a guest
 * is handed").
 *
 * Every address and interrupt number here is the hypervisor's own choice,
 * defined once: the kernel implements the devices at these addresses,
 * the device-tree writer (tools/fdt) describes them at these addresses,
 * and nothing else defines one. They match what QEMU's `virt` machine
 * presents, which is what a stock AArch64 guest kernel's drivers and
 * device tree already expect -- a convenience, not the source.
 *
 * Shared with userland (the owner builds the device tree) and with the
 * guest fixtures that read one, so this header is C89-plain: macros and
 * fixed-width integer types only.
 */
#ifndef COSMO_HV_MACHINE_H
#define COSMO_HV_MACHINE_H

/* Where machine mode puts a guest's RAM: where virt puts it. A flat test
 * guest may put its RAM at 0 instead; the kernel does not care. */
#define COSMO_HVM_RAM_BASE        0x40000000ull

/* The GICv3 distributor and the redistributor frames, one per vCPU. */
#define COSMO_HVM_GICD_BASE       0x08000000ull
#define COSMO_HVM_GICD_SIZE       0x00010000ull
#define COSMO_HVM_GICR_BASE       0x080A0000ull
#define COSMO_HVM_GICR_STRIDE     0x00020000ull   /* RD_base then SGI_base, 64 KiB each */
#define COSMO_HVM_GICR_FRAMES     4u              /* COSMO_HV_VCPUS_MAX */
#define COSMO_HVM_NR_SPIS         256u            /* INTIDs 32..287 */

/* The console UART: a PL011, and the SPI it raises. */
#define COSMO_HVM_UART_BASE       0x09000000ull
#define COSMO_HVM_UART_SIZE       0x00001000ull
#define COSMO_HVM_UART_INTID      33u
#define COSMO_HVM_UART_CLOCK_HZ   24000000u       /* what the device tree's fixed clock says */

/* The generic timer's PPIs, as INTIDs (a device tree numbers a PPI from
 * 16: subtract it). The virtual timer is a guest's; the others are named
 * so the tree can say where they would be. */
#define COSMO_HVM_TIMER_PPI_SEC   29u
#define COSMO_HVM_TIMER_PPI_PHYS  30u
#define COSMO_HVM_TIMER_PPI_VIRT  27u
#define COSMO_HVM_TIMER_PPI_HYP   26u

/* virtio-mmio transport windows, where QEMU's virt puts its bank, each with
 * an SPI the distributor routes. An owner (vmctl) models the transport and
 * the device behind it; a guest finds it through the device tree. Window 0
 * is the block device, window 1 the network device -- adjacent 0x200 slots,
 * as virt lays them out. */
#define COSMO_HVM_VIRTIO0_BASE    0x0A000000ull
#define COSMO_HVM_VIRTIO0_SIZE    0x200ull
#define COSMO_HVM_VIRTIO0_INTID   48u
#define COSMO_HVM_VIRTIO1_BASE    0x0A000200ull
#define COSMO_HVM_VIRTIO1_SIZE    0x200ull
#define COSMO_HVM_VIRTIO1_INTID   49u

/* The arm64 Image header, as the boot protocol defines it. */
#define COSMO_HVM_IMAGE_MAGIC     0x644d5241u     /* "ARM\x64", little-endian at offset 56 */
#define COSMO_HVM_IMAGE_MAGIC_OFF 56u
#define COSMO_HVM_IMAGE_TEXT_OFF  8u              /* text_offset: where the image goes from a 2 MiB base */
#define COSMO_HVM_IMAGE_SIZE_OFF  16u             /* image_size */
#define COSMO_HVM_IMAGE_FLAGS_OFF 24u             /* bit 3: text_offset is from any 2 MiB base, not RAM's start */

#endif /* COSMO_HV_MACHINE_H */
