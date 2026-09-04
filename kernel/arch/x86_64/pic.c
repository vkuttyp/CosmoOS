/*
 * pic.c - Legacy 8259A PIC: remap and mask.
 */

#include <x86/idt.h>
#include <x86/io.h>
#include <x86/pic.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define OCW3_READ_ISR 0x0B
#define PIC_EOI 0x20

void pic_init_masked(void)
{
    /* ICW1: start initialisation, expect ICW4. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    /* ICW2: vector offsets. */
    outb(PIC1_DATA, X86_VECTOR_IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, X86_VECTOR_IRQ_BASE + 8);
    io_wait();
    /* ICW3: master has slave on IRQ2; slave identity 2. */
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask everything. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

bool pic_is_spurious(unsigned irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, OCW3_READ_ISR);
        return (inb(PIC1_CMD) & 0x80) == 0;
    }
    if (irq == 15) {
        outb(PIC2_CMD, OCW3_READ_ISR);
        return (inb(PIC2_CMD) & 0x80) == 0;
    }
    return false;
}

void pic_eoi(unsigned irq)
{
    if (irq >= 8) {
        /* A spurious IRQ 15 still needs the master acknowledged for the
         * cascade; a real one needs both. */
        if (!pic_is_spurious(irq))
            outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }
    if (irq == 7 && pic_is_spurious(irq))
        return;
    outb(PIC1_CMD, PIC_EOI);
}
