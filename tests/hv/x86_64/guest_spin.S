/*
 * guest_spin.S - A guest that never exits on its own
 * (docs/kernel-services/virtualization/testing.md). Only the host's timer
 * interrupt (intercepted) takes the CPU back; the self-test bounds the run
 * by those exits and the owner-kill path ends it in user space.
 */
    .code16
    .globl _start
_start:
    movb $'.', %al
    outb %al, $0xE9
1:  jmp 1b
