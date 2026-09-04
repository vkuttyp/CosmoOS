/*
 * x86/idt.h - Interrupt descriptor table and vector map. Private to x86-64.
 *
 * Vector space:
 *   0-31    CPU exceptions (fixed by the architecture)
 *   32-47   legacy PIC IRQ 0-15 (masked; kept mapped so spurious ones are
 *           identifiable)
 *   48-255  free for LAPIC, IOAPIC, MSI, IPIs (allocated in Phase 3/6)
 */

#ifndef X86_IDT_H
#define X86_IDT_H

#include <stdint.h>

#define IDT_VECTORS 256u

#define X86_TRAP_DE  0u   /* divide error */
#define X86_TRAP_DB  1u   /* debug */
#define X86_TRAP_NMI 2u
#define X86_TRAP_BP  3u   /* breakpoint */
#define X86_TRAP_OF  4u   /* overflow */
#define X86_TRAP_BR  5u   /* bound range */
#define X86_TRAP_UD  6u   /* invalid opcode */
#define X86_TRAP_NM  7u   /* device not available */
#define X86_TRAP_DF  8u   /* double fault */
#define X86_TRAP_TS  10u  /* invalid TSS */
#define X86_TRAP_NP  11u  /* segment not present */
#define X86_TRAP_SS  12u  /* stack fault */
#define X86_TRAP_GP  13u  /* general protection */
#define X86_TRAP_PF  14u  /* page fault */
#define X86_TRAP_MF  16u  /* x87 FP */
#define X86_TRAP_AC  17u  /* alignment check */
#define X86_TRAP_MC  18u  /* machine check */
#define X86_TRAP_XM  19u  /* SIMD FP */
#define X86_TRAP_VE  20u  /* virtualization */
#define X86_TRAP_CP  21u  /* control protection */

#define X86_EXCEPTION_COUNT 32u
#define X86_VECTOR_IRQ_BASE 32u
#define X86_VECTOR_IRQ_COUNT 16u

/* Stubs generated in isr.S: one per vector, each X86_ISR_STUB_SIZE bytes. */
#define X86_ISR_STUB_SIZE 16u
extern const uint8_t x86_isr_stubs[];

/* Build the IDT and load it. Called once on the boot CPU; other CPUs
 * later load the same table. */
void idt_init(void);

/* Load the already-built IDT on the calling CPU. */
void idt_load(void);

#endif /* X86_IDT_H */
