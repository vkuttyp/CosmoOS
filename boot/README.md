# boot

UEFI bootloader and the boot protocol that hands control to the kernel ELF. Owns the loader-to-kernel contract; the kernel never depends on a third-party loader ABI. Generic loader code in `uefi/`, the CPU- and page-table-specific parts per architecture in `uefi/arch/{x86_64,aarch64}/` (`BOOTX64.EFI`, `BOOTAA64.EFI`); the protocol (`protocol/cosmoboot.h`, version 4) is shared.
