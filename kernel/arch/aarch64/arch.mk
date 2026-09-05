# AArch64 architecture sources (docs/kernel/arch/aarch64/). Paths are
# relative to $(ROOT).

KERNEL_ARCH_SRCS := \
	kernel/arch/aarch64/entry.S \
	kernel/arch/aarch64/vectors.S \
	kernel/arch/aarch64/switch.S \
	kernel/arch/aarch64/trampoline.S \
	kernel/arch/aarch64/start.c \
	kernel/arch/aarch64/cpu.c \
	kernel/arch/aarch64/irq.c \
	kernel/arch/aarch64/trap.c \
	kernel/arch/aarch64/gic.c \
	kernel/arch/aarch64/timer.c \
	kernel/arch/aarch64/mmu.c \
	kernel/arch/aarch64/percpu.c \
	kernel/arch/aarch64/context.c \
	kernel/arch/aarch64/user.c \
	kernel/arch/aarch64/smp.c \
	kernel/arch/aarch64/pl011.c \
	kernel/arch/aarch64/fwcfg.c \
	kernel/arch/aarch64/pci.c \
	kernel/arch/aarch64/modreloc.c \
	kernel/arch/aarch64/backtrace.c \
	kernel/arch/aarch64/shutdown.c \
	kernel/arch/aarch64/hv.c

KERNEL_LINKER_SCRIPT := $(ROOT)/kernel/arch/aarch64/linker.ld
