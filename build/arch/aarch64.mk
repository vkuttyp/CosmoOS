# AArch64 target definitions (docs/kernel/arch/aarch64/).

KERNEL_TARGET := aarch64-unknown-none-elf
LOADER_TARGET := aarch64-unknown-windows

# Kernel: ARMv8.0-A baseline (QEMU's cortex-a72; PAN and LSE are detected
# at run time where they matter), small code model (every symbol within
# +-4 GiB of the code: the image, the near arena and the direct map all
# are), general registers only (no FP/SIMD state in the kernel), inline
# LL/SC atomics rather than the outline-atomics runtime, static non-PIC.
# x18 is reserved as the platform register by convention; leaving it
# alone costs nothing and keeps the option open.
# -mno-omit-leaf-frame-pointer: at -O1 clang gives an AArch64 leaf
# function no frame record, so a stack walk from a PC inside one skips
# its caller (found by the lockup unit's sample of a spinning leaf;
# docs/kernel/diagnostics/design.md). x86-64 keeps the leaf frame with
# -fno-omit-frame-pointer alone.
KERNEL_ARCH_CFLAGS := \
	-march=armv8-a -mcmodel=small -mgeneral-regs-only -mno-outline-atomics \
	-mno-omit-leaf-frame-pointer \
	-ffixed-x18 -fno-pic -fno-pie -DARCH_AARCH64=1

KERNEL_ARCH_LDFLAGS :=

# Loader: PE/COFF for the ARM64 UEFI ABI (the plain AAPCS64 procedure
# call standard; no Microsoft-specific attribute is needed).
LOADER_ARCH_CFLAGS := \
	-march=armv8-a -mgeneral-regs-only -mno-outline-atomics \
	-DARCH_AARCH64=1
LOADER_ARCH_LDFLAGS := /machine:arm64

LOADER_EFI_NAME := BOOTAA64.EFI
QEMU_SYSTEM := qemu-system-aarch64
