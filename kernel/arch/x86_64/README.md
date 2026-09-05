# kernel/arch/x86_64

x86-64 target: boot entry, GDT/IDT, APIC/IOAPIC, paging, context switch, syscall entry, serial (UART 16550), and since Phase 12 the AMD-V virtualization backend behind `arch/hv.h` (`svm.c`, `svm_npt.c`, `svm_run.S`, private header `include/x86/svm.h`). Since Phase 13 one of two implementations of `kernel/include/arch/*.h`; the sibling is `kernel/arch/aarch64/`. The user copy primitive with exception fixups is `uaccess.S` (`rep movsb`, one table entry; `docs/kernel/memory/design.md` §6.1).
