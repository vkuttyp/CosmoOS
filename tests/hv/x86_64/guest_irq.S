/*
 * guest_irq.S - Real-mode guest: virtual interrupt delivery
 * (docs/kernel-services/virtualization/testing.md).
 * Loaded at 0x1000, cs = 0. Installs IVT[0x20] -> handler, then:
 *
 *   cli; hlt        HLT exit #1 with IF clear: an injected vector must stay pending
 *   sti; hlt        HLT exit #2: IF set but the halt comes first (sti shadow)
 *   (interrupt)     the handler writes 'I' to port 0xE9 and returns
 *   hlt             HLT exit #3: delivered on the re-entry after #2 (skipping the
 *                   intercepted hlt ends the sti shadow), the handler ran
 */
    .code16
    .globl _start
_start:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0x7000, %sp
    movw $handler, 0x80        /* IVT[0x20].offset */
    movw $0, 0x82              /* IVT[0x20].segment */
    cli
    hlt
    sti
    hlt
    hlt
1:  hlt
    jmp 1b

handler:
    pushw %ax
    movb $'I', %al
    outb %al, $0xE9
    popw %ax
    iretw
