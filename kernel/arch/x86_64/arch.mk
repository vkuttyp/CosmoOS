# x86-64 architecture sources. Paths are relative to $(ROOT).

KERNEL_ARCH_SRCS := \
	kernel/arch/x86_64/entry.S \
	kernel/arch/x86_64/isr.S \
	kernel/arch/x86_64/uaccess.S \
	kernel/arch/x86_64/start.c \
	kernel/arch/x86_64/cpu.c \
	kernel/arch/x86_64/fpu.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/idt.c \
	kernel/arch/x86_64/pic.c \
	kernel/arch/x86_64/trap.c \
	kernel/arch/x86_64/mmu.c \
	kernel/arch/x86_64/percpu.c \
	kernel/arch/x86_64/lapic.c \
	kernel/arch/x86_64/ioapic.c \
	kernel/arch/x86_64/irqc.c \
	kernel/arch/x86_64/timer.c \
	kernel/arch/x86_64/pit.c \
	kernel/arch/x86_64/switch.S \
	kernel/arch/x86_64/context.c \
	kernel/arch/x86_64/trampoline.S \
	kernel/arch/x86_64/smp.c \
	kernel/arch/x86_64/user.c \
	kernel/arch/x86_64/modreloc.c \
	kernel/arch/x86_64/pci_legacy.c \
	kernel/arch/x86_64/fwcfg.c \
	kernel/arch/x86_64/syscall_entry.S \
	kernel/arch/x86_64/serial.c \
	kernel/arch/x86_64/backtrace.c \
	kernel/arch/x86_64/shutdown.c \
	kernel/arch/x86_64/hv.c \
	kernel/arch/x86_64/svm.c \
	kernel/arch/x86_64/svm_npt.c \
	kernel/arch/x86_64/svm_run.S \
	kernel/arch/x86_64/vmx.c \
	kernel/arch/x86_64/vmx_ept.c \
	kernel/arch/x86_64/vmx_run.S

KERNEL_LINKER_SCRIPT := $(ROOT)/kernel/arch/x86_64/linker.ld
