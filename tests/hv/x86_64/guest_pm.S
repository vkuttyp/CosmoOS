/*
 * guest_pm.S - 32-bit protected-mode guest: the segment state path and an
 * MMIO exit (docs/kernel-services/virtualization/testing.md).
 * Loaded at 0x1000 and entered with cr0.PE, flat 32-bit cs/ds set by the
 * owner through vcpu_regs (no GDT is needed while the guest does not
 * reload segments).
 *
 *   'P' -> port 0xE9
 *   mov %eax, 0x10000000   store to guest-physical memory with no region: MMIO exit
 *                          (write, gpa 0x10000000); the owner skips the 5-byte instruction
 *   'Q' -> port 0xE9
 *   hlt
 */
    .code32
    .globl _start
_start:
    movb $'P', %al
    outb %al, $0xE9
    movl $0x5A5A5A5A, %eax
    movl %eax, 0x10000000
    movb $'Q', %al
    outb %al, $0xE9
    hlt
1:  hlt
    jmp 1b
