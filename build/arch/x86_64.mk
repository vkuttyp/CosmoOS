# x86-64 target definitions.

KERNEL_TARGET := x86_64-unknown-none-elf
# The kernel and its modules compile for x86_64-linux-gnu, as Linux's own
# kernel does (-ffreestanding keeps it from assuming a C library): LLVM
# places the stack protector's canary in a segment-relative slot
# (-mstack-protector-guard=tls, %gs:PERCPU_CANARY_OFFSET) only for targets
# with such a slot -- glibc Linux, Fuchsia, Android -- and for
# x86_64-unknown-none uses one global __stack_chk_guard, which cannot be per
# thread (BUILD-1). Nothing in the tree tests __linux__; user space and
# libc keep KERNEL_TARGET.
KERNEL_CC_TARGET := x86_64-unknown-linux-gnu
LOADER_TARGET := x86_64-unknown-windows

# Kernel: higher-half (top 2 GiB) code model, no red zone (interrupts use
# the current stack), general registers only (no FPU/SSE state to save),
# static non-PIC.
KERNEL_ARCH_CFLAGS := \
	-mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
	-fno-pic -fno-pie -DARCH_X86_64=1 \
	$(KERNEL_STACK_PROTECTOR) -mstack-protector-guard=tls \
	-mstack-protector-guard-reg=gs -mstack-protector-guard-offset=$(PERCPU_CANARY_OFFSET)

KERNEL_ARCH_LDFLAGS :=

# Loader: Microsoft ABI is the default for the windows target. No stack
# probes (there is no __chkstk in a freestanding image).
LOADER_ARCH_CFLAGS := \
	-mno-red-zone -mgeneral-regs-only -mno-stack-arg-probe \
	-DARCH_X86_64=1

LOADER_ARCH_LDFLAGS := /machine:x64
LOADER_EFI_NAME := BOOTX64.EFI
QEMU_SYSTEM := qemu-system-x86_64
