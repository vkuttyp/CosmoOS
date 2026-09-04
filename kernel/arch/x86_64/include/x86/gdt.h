/*
 * x86/gdt.h - Global descriptor table and TSS. Private to x86-64.
 *
 * Selector layout is fixed so that a future SYSRET works with a single
 * STAR value: user data must sit at kernel-code-base + 16 and user code
 * at +24 when the STAR sysret base is set to the user-data selector - 8.
 *
 *   0x00 null
 *   0x08 kernel code (64-bit, DPL 0)
 *   0x10 kernel data (DPL 0)
 *   0x18 user data   (DPL 3)
 *   0x20 user code   (64-bit, DPL 3)
 *   0x28 TSS         (16-byte system descriptor)
 */

#ifndef X86_GDT_H
#define X86_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08u
#define GDT_KERNEL_DATA 0x10u
#define GDT_USER_DATA   0x18u
#define GDT_USER_CODE   0x20u
#define GDT_TSS         0x28u

/* IST slots used by the IDT. */
#define IST_NONE         0u
#define IST_DOUBLE_FAULT 1u

/* Boot CPU: load the static GDT, reload segment registers, load the TSS
 * with the double-fault IST stack. */
void gdt_init(void);

/* Boot CPU, before starting `cpu`: allocate that CPU's GDT, TSS, and
 * double-fault stack. Returns 0 or -ENOMEM. */
int gdt_alloc_cpu(unsigned cpu);

/* Calling AP: load the tables gdt_alloc_cpu prepared for it. Resets the
 * GS base as a side effect; install the per-CPU pointer afterwards. */
void gdt_init_cpu(unsigned cpu);

/* Set the stack the CPU switches to on a ring 3 -> ring 0 transition.
 * Unused until user mode exists; provided so the TSS layout is settled. */
void gdt_set_kernel_stack(uint64_t rsp0);

#endif /* X86_GDT_H */
