# e1000e driver: API

The driver has no interface of its own. It is a module that binds a PCI
id and registers a `struct netif`; everything a user or another
subsystem sees comes through the network stack's existing surface.

## Binding

| | |
| --- | --- |
| Module | `e1000e` (`modules/e1000e.ko`, `MODULE_CAP_DRIVER`, no dependencies) |
| Bus | `pci`, `struct pci_driver` with one id: vendor `0x8086`, device `0x10d3` (82574L) |
| Registers | `struct netif`, first free `ethN`, MTU 1500, `caps` 0 |
| Interrupt | one MSI-X vector (index 0) on CPU 0; single-message MSI when MSI-X is absent |

## Log lines

```text
[ INFO] e1000e: pci:BB:DD.F is ethN (52:54:00:c0:5f:06, link up)
[ INFO] e1000e: ethN: link down          / link up
[ WARN] e1000e: ethN: transmit hung (N outstanding, TDH T for 5 s); resetting the ring
[ERROR] e1000e: pci:BB:DD.F: <what failed at probe>
```

The first is what the boot test requires when the device is present.

## Counters

`nif->stats`, as for every interface: `rx_packets`, `rx_bytes`,
`rx_dropped` (short frames, no cluster to replace one), `rx_errors`
(the descriptor's error byte), `tx_packets`, `tx_bytes`, `tx_dropped`
(ring full, map failure, watchdog reset).

## QEMU

`scripts/qemu-run.sh` adds `-device e1000e,netdev=n1,mac=52:54:00:c0:5f:06`
on its own user-mode backend by default on both machine types, so a
normal boot has `eth0` (virtio-net) and `eth1` (e1000e). `QEMU_NIC`
selects: `both` (default), `virtio` (as before this driver), `e1000e`
(this driver alone, so it is `eth0` and the default interface — the
shape that runs the whole network suite over it).

## Errors at probe

| | |
| --- | --- |
| `-ENOMEM` | the driver structure, a ring, or a receive cluster |
| `-EIO` | BAR0 unmappable, or `RAH0.AV` clear (no station address) |
| `-ENODEV` | neither MSI-X nor MSI available |

A probe that fails leaves the device reset and masked and registers
nothing.
