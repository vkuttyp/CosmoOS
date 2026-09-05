# x86-64 target definitions.

KERNEL_TARGET := x86_64-unknown-none-elf
LOADER_TARGET := x86_64-unknown-windows

# Kernel: higher-half (top 2 GiB) code model, no red zone (interrupts use
# the current stack), general registers only (no FPU/SSE state to save),
# static non-PIC.
KERNEL_ARCH_CFLAGS := \
	-mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-fno-pic -fno-pie -DARCH_X86_64=1

KERNEL_ARCH_LDFLAGS :=

# Loader: Microsoft ABI is the default for the windows target. No stack
# probes (there is no __chkstk in a freestanding image).
LOADER_ARCH_CFLAGS := \
	-mno-red-zone -mgeneral-regs-only -mno-stack-arg-probe \
	-DARCH_X86_64=1

LOADER_ARCH_LDFLAGS := /machine:x64
LOADER_EFI_NAME := BOOTX64.EFI
QEMU_SYSTEM := qemu-system-x86_64
