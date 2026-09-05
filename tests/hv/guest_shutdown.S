/*
 * guest_shutdown.S - Real-mode guest that triple-faults
 * (docs/kernel-services/virtualization/testing.md). The owner sets the
 * IDTR limit to 0 before running: `int $3` then finds no IVT entry,
 * the #GP and #DF that follow find none either, and the vCPU shuts down
 * (SHUTDOWN exit; the vCPU is dead afterwards).
 */
    .code16
    .globl _start
_start:
    movb $'S', %al
    outb %al, $0xE9
    int $3
    hlt
1:  hlt
    jmp 1b
