# e1000e driver: architecture

The first network device driven that is not virtio
(`docs/audit/next-subsystem.md`). Constitution sections 25 (drivers
never touch bus layout themselves), 26 (DMA through the API) and 21
(no offload complexity without a benchmark); Invariant 5 of
`drivers/network/README.md` (the stack never depends on a specific
NIC). Spec: the Intel 82574 datasheet, §4.6 and §14 for bring-up
(design.md, "Bring-up").

## Where it sits

```text
   network stack                netif_transmit, netif_rx, netif_default
        │                       (kernel/include/kernel/netif.h)
   struct netif "ethN"          netif_ops: transmit, release, tx_pending
        │
   drivers/network/e1000e.c  (module e1000e)
        bring-up, RX/TX legacy rings (256 each), IRQ handler, watchdog
        │
   drivers/pci   BAR0, MSI-X/MSI    kernel/device/dma.c   rings, buffers
```

The driver is a boot module (`modules/e1000e.ko`, source list
`MODULE_e1000e_SRCS` in `build/module.mk`, loaded after `nvme`), declared
with `COSMO_MODULE("e1000e", ..., MODULE_CAP_DRIVER)`; it reaches the
kernel only through exported symbols. Its register map and descriptor
layouts are in `drivers/network/e1000e.h`. `scripts/qemu-run.sh` attaches
`-device e1000e` on both machine types; `QEMU_NIC` chooses `both`
(default: virtio-net is `eth0`, this driver `eth1`), `virtio` or
`e1000e` (this driver alone, as `eth0`).

## Purpose

Drive the Intel 82574L (`8086:10d3`, what QEMU's `-device e1000e`
models) as one `struct netif`, and so give the stack's NIC-independence
claim a second implementation without adding anything to the kernel's
interface (invariant E6).

## Responsibilities

- Probe: enable the function, 64-bit DMA mask, map BAR0, mask
  interrupts, `CTRL.RST`, set link up, read the station address from
  `RAL0`/`RAH0`, clear the `MTA` (`e1000e_probe`).
- One receive and one transmit legacy descriptor ring of `E1000E_RING`
  (256) entries; one 2 KiB cluster mbuf per receive descriptor
  (`rx_arm`, `rx_process`); one descriptor per transmit segment, refused
  with `-ENOBUFS` when the ring lacks room (`e1000e_transmit`, E2).
- Dropping errored or short frames before the stack sees them (E1).
- One interrupt (MSI-X entry 0, else single-message MSI) whose handler
  services both rings whatever `ICR` says and logs link changes
  (`e1000e_irq`, E5).
- A one-second watchdog (`e1000e_watchdog`, `E1000E_WATCHDOG_NS`) that
  resets the transmit ring after `E1000E_HUNG_TICKS` (5) ticks without
  `TDH` movement (E4).
- Naming the interface the first free `ethN` (`pick_name`).
- Removal that frees ring DMA only after both engines acknowledge the
  stop, else marks the function `device_retain_dma` (`e1000e_remove`,
  `hw_quiesce`, E7).

## Non-responsibilities

- Checksum offload: `nif->caps` is 0; `net-nicbench` measured it as one
  to two percent of a send (design.md, "Offloads").
- Multiple queues, interrupt moderation (`ITR`), jumbo frames, VLAN,
  wake-on-LAN, EEPROM access, multicast filtering, and the `igb` family
  (design.md, "What is not here").
- Queuing, routing and interface selection: the stack owns queuing and
  `netif_default()` picks the interface; the driver adds no route.
- Its own statistics for what the layer counts: `rx_packets`,
  `rx_bytes`, `tx_packets` and `tx_bytes` are counted by `netif_rx` and
  `netif_transmit`; the driver counts only ring drops and hardware
  errors.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `struct netif_ops` (`transmit`, `release`, `tx_pending`) | `kernel/netif.h` | the stack calls in |
| `netif_register`, `netif_unregister`, `netif_rx`, `netif_set_up`, `netif_find`, `netif_put` | `kernel/netif.h` | the driver calls out |
| `m_getcl`, mbuf chains | `kernel/mbuf.h` | receive clusters, transmit segments |
| `pci_register_driver`, `pci_enable_device`, `pci_map_bar`, `pci_msix_enable/request/disable`, `pci_msi_enable/disable` | `drivers/pci.h` | probe/remove |
| `dma_set_mask`, `dma_alloc/free`, `dma_map/unmap` | `kernel/dma.h` | rings and buffers |
| `timer_setup`, `timer_start`, `timer_cancel_sync` | `kernel/timer.h` | watchdog |
| `device_retain_dma`, `device_release_dma` | `kernel/device.h` | stop not acknowledged (E7) |
| `e1000e_test_ring_frees`, `e1000e_test_has_orphan`, `e1000e_test_recover_stop_ack` | `CONFIG_SELFTEST` exports | `e1000e-stop-ack` self-test |
| `FI_E1000E_RX_DISABLE_ACK`, `FI_E1000E_TX_DISABLE_ACK` | `kernel/faultinject.h` | the same test |
