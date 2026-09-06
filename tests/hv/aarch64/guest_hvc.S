/*
 * guest_hvc.S - A guest that calls its hypervisor
 * (docs/kernel-services/virtualization/testing.md).
 *
 *   x0..x4 = 0x2A, 1, 2, 3, 4
 *   hvc #0 (HYPERCALL exit carrying those registers)
 *   x0 = x0 + 1, so the owner can see the guest ran on
 *   wfi (WFI exit)
 */
    .text
    .globl _start
_start:
    mov x0, #0x2A
    mov x1, #1
    mov x2, #2
    mov x3, #3
    mov x4, #4
    hvc #0
    add x0, x0, #1
    wfi
1:  b 1b
