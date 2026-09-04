# PCI: invariants

**P1. Drivers never touch configuration space layout.** BAR decoding,
capability walking, MSI/MSI-X programming and command-register bits are
in `pci.c`; a driver uses `struct pci_device` fields and `pci_*`
services. Check: review (no file outside `drivers/pci/` and
`kernel/arch/x86_64/pci_legacy.c` uses `PCI_*` register offsets or
`0xCF8`). Gap: none.

**P2. BAR sizes are derived by probing, never trusted from a table, and
decode is off while probing.** `decode_bars` clears memory/IO decode,
writes all ones, restores the original value. Check: `pci` self-test
(every size a power of two, base aligned to size). Gap: a device whose
BAR the firmware left unprogrammed (base 0) is recorded as is; nothing
assigns it.

**P3. Every `pci_device` lives forever and is owned by the core.**
Allocated once in `pci_init`, never freed; index and search functions
return plain pointers. Check: review. Gap: hot-plug would violate this
and needs reference-counted lookups first.

**P4. ECAM is mapped once, uncached, and only for the MCFG's segment 0
range; accesses outside that bus range read all ones and write
nothing.** Check: every boot on q35 (`pci: ECAM at 0xe0000000, buses
0-255`). Gap: legacy access is never exercised under test.

**P5. MSI-X entries are masked before they are programmed and after they
are released; INTx is disabled whenever MSI or MSI-X is enabled.**
Check: review of `pci_msix_enable`/`pci_msix_request`/`pci_msix_release`.
Gap: no test confirms no spurious INTx (nothing routes INTx anyway).

**P6. A vector handed to a device comes from `irq_request_msi` and is
released through `irq_release_msi` after the entry is masked.** Check:
every boot (config and queue vectors for three devices); `pci_msi_disable`
releases the single MSI vector recorded in `msi_vector`. Gap: no test
exercises the MSI (non-MSI-X) path, since every QEMU virtio device has
MSI-X.

**P7. Enumeration is finite: depth at most 8 bridges, at most
`PCI_MAX_DEVICES` (64) functions, capability walks at most 48 steps.**
Check: review; the guards are unconditional. Gap: none.

**P8. The `pci` bus is registered before any PCI driver and all
enumeration precedes boot-module loading.** `pci_init` runs in
`kernel_main` before `module_load_boot`. Check: every boot. Gap: none.
