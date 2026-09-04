# drivers/pci

PCI core, part of the kernel image: configuration access through ECAM
(ACPI MCFG) or the architecture's legacy mechanism, depth-first
enumeration across bridges, BAR sizing, capability lookup, MSI-X and MSI
through `irq_request_msi`, the `pci` bus and `struct pci_driver`.
Resource assignment, INTx routing and hot-plug are not implemented.
Header: `drivers/include/drivers/pci.h`. Documentation: `docs/drivers/pci/`.
