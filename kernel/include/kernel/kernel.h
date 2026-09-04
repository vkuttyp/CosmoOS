/*
 * kernel.h - Kernel entry and image layout symbols.
 *
 * kernel_main() is the architecture-independent entry called by the
 * architecture start code once the CPU is in a sane state (console
 * available, descriptor tables loaded, interrupts masked but deliverable).
 *
 * The __kernel_* symbols are defined by the linker script and bound the
 * kernel image in virtual memory. They are arrays so that taking their
 * address yields the symbol value without a load.
 */

#ifndef KERNEL_KERNEL_H
#define KERNEL_KERNEL_H

#include <kernel/compiler.h>

struct cosmoboot_info;

void kernel_main(const struct cosmoboot_info *info) __noreturn;

extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];

static inline bool kernel_text_contains(uintptr_t addr)
{
    return addr >= (uintptr_t)__text_start && addr < (uintptr_t)__text_end;
}

static inline bool kernel_image_contains(uintptr_t addr)
{
    return addr >= (uintptr_t)__kernel_start && addr < (uintptr_t)__kernel_end;
}

#endif /* KERNEL_KERNEL_H */
