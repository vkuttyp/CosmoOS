# x86-64 architecture sources. Paths are relative to $(ROOT).

KERNEL_ARCH_SRCS := \
	kernel/arch/x86_64/entry.S \
	kernel/arch/x86_64/isr.S \
	kernel/arch/x86_64/start.c \
	kernel/arch/x86_64/cpu.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/idt.c \
	kernel/arch/x86_64/pic.c \
	kernel/arch/x86_64/trap.c \
	kernel/arch/x86_64/mmu.c \
	kernel/arch/x86_64/serial.c \
	kernel/arch/x86_64/backtrace.c \
	kernel/arch/x86_64/shutdown.c

KERNEL_LINKER_SCRIPT := $(ROOT)/kernel/arch/x86_64/linker.ld
