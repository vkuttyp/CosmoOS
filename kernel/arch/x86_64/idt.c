/*
 * idt.c - Interrupt descriptor table.
 *
 * All 256 vectors point at the uniform stubs from isr.S, so no vector is
 * ever "unhandled at the CPU level"; the generic dispatcher decides what
 * an unexpected one means. #DF runs on its own IST stack. #BP is DPL 3 so
 * that a future user-mode debugger's int3 can reach the kernel.
 */

#include <kernel/string.h>

#include <x86/gdt.h>
#include <x86/idt.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;        /* bits 0-2 */
    uint8_t  type_attr;  /* P, DPL, gate type */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_entry) == 16, "IDT entry size");

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GATE_INTERRUPT_DPL0 0x8Eu /* P=1 DPL=0 type=0xE (interrupt gate, IF cleared) */
#define GATE_INTERRUPT_DPL3 0xEEu /* P=1 DPL=3 type=0xE */

static struct idt_entry g_idt[IDT_VECTORS] __attribute__((aligned(16)));

static void set_gate(unsigned vector, uintptr_t handler, uint8_t type_attr, uint8_t ist)
{
    struct idt_entry *e = &g_idt[vector];
    e->offset_low = (uint16_t)(handler & 0xFFFF);
    e->selector = GDT_KERNEL_CODE;
    e->ist = ist & 0x7;
    e->type_attr = type_attr;
    e->offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)(handler >> 32);
    e->reserved = 0;
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));

    for (unsigned v = 0; v < IDT_VECTORS; v++) {
        uintptr_t stub = (uintptr_t)x86_isr_stubs + (uintptr_t)v * X86_ISR_STUB_SIZE;
        uint8_t attr = GATE_INTERRUPT_DPL0;
        uint8_t ist = IST_NONE;

        if (v == X86_TRAP_BP)
            attr = GATE_INTERRUPT_DPL3;
        /* The paranoid vectors (isr.S): their own stacks, GS decided from
         * the MSR. isr.S routes exactly these four to isr_paranoid. */
        if (v == X86_TRAP_DF)
            ist = IST_DOUBLE_FAULT;
        else if (v == X86_TRAP_NMI)
            ist = IST_NMI;
        else if (v == X86_TRAP_MC)
            ist = IST_MACHINE_CHECK;
        else if (v == X86_TRAP_DB)
            ist = IST_DEBUG;

        set_gate(v, stub, attr, ist);
    }

    idt_load();
}

void idt_load(void)
{
    struct idtr idtr = {
        .limit = sizeof(g_idt) - 1,
        .base = (uint64_t)(uintptr_t)g_idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}
