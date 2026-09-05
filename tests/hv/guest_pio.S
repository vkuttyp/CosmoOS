/*
 * guest_pio.S - Real-mode guest: debug console, an unclaimed port, an IN
 * completion (docs/kernel-services/virtualization/testing.md).
 * Loaded at guest-physical 0x1000 with cs = 0, ip = 0x1000.
 *
 *   "HV" -> port 0xE9 (kernel debug console, no exit to the owner)
 *   0x1234 -> port 0x80 (IO exit: write, size 2)
 *   hlt (HLT exit #1)
 *   in port 0x81 -> al (IO exit: read, size 1; the owner supplies the value)
 *   al -> port 0xE9
 *   hlt (HLT exit #2), then halt forever
 */
    .code16
    .globl _start
_start:
    movb $'H', %al
    outb %al, $0xE9
    movb $'V', %al
    outb %al, $0xE9
    movw $0x1234, %ax
    outw %ax, $0x80
    hlt
    inb $0x81, %al
    outb %al, $0xE9
    hlt
1:  hlt
    jmp 1b
