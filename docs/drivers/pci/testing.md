# PCI: testing

## Self-test `pci` (`kernel/device/devtest.c`)

Runs in debug builds under QEMU q35 (`make test`). It checks that
enumeration found at least one function and that index 0 is the host
bridge (`pci:00:00.0`, class `06.00`, vendor `8086`), that
`device_find(&pci_bus, "pci:00:00.0")` returns that device, and for
every function:

- the 8-, 16- and 32-bit configuration accessors agree on the vendor
  and device id dword, and the cached `vendor`/`device` fields match;
- the vendor is not `0xffff`;
- every implemented BAR has a power-of-two size and a base aligned to
  that size;
- `pci_find_capability` returns the cached MSI and MSI-X offsets and 0
  for capability id `0xfe`.

Then: `pci_device_at(count)` is NULL, an unknown id is not found, the
host bridge is found by its own id, and every vendor `1af4` (virtio)
function has MSI-X and a memory BAR. The test logs the function count
(9 under the default QEMU configuration), the virtio count (3) and the
access mechanism (`ECAM`).

## Indirect coverage

Every boot exercises `pci_msix_enable`/`pci_msix_request` for the
config vector and one vector per interrupt-driven virtqueue (the virtio
module logs `irq: MSI vector N on CPU 0 (virtio-cfg|virtio-vq)`), the
capability walk (`pci_find_capability` over virtio's vendor
capabilities), `pci_map_bar` (BARs holding the virtio structures and the
MSI-X table), and `pci_enable_device` with bus mastering. The `blk`
self-test's I/O completing at all proves the MSI-X path end to end.

## Running

```sh
make test                 # self-tests, 4 CPUs
QEMU_SMP=1 make test      # vectors all on CPU 0 anyway; checks nothing SMP-specific breaks
make run                  # then read the [DEBUG] pci: lines in the serial output
```

## Gaps

- Legacy configuration access (`arch_pci_legacy_*`) is not tested: q35
  always provides an MCFG. A `-machine pc` run would cover it.
- Single-message MSI (`pci_msi_enable`) is not exercised: every QEMU
  virtio device has MSI-X.
- Bridges: the default configuration has no PCI-to-PCI bridge, so the
  secondary-bus recursion runs only on the code-review level. Adding
  `-device pcie-root-port` to `QEMU_EXTRA` would exercise it.
- No negative tests for malformed capability lists or BARs (the guards
  are reviewed, not driven).
