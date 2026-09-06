/*
 * guest_fpu.S - Real-mode guest that reveals and replaces its xmm0
 * (docs/kernel-services/virtualization/testing.md, "hv-guest-fpu").
 * Loaded at guest-physical 0x1000 with cs = 0, ip = 0x1000.
 *
 * Enables SSE for itself (CR0.EM = 0, CR0.MP = 1, CR4.OSFXSR = 1), stores
 * whatever xmm0 holds at 0x3000 (a fresh vCPU must show zeros: nothing
 * of the owner's registers leaked in), loads xmm0 from the sixteen bytes
 * the test placed at 0x3010 (which must not leak out to the owner), then
 * halts.
 */
    .code16
    .globl _start
_start:
    movl %cr0, %eax
    andl $~0x4, %eax            /* EM = 0 */
    orl $0x2, %eax              /* MP = 1 */
    movl %eax, %cr0
    movl %cr4, %eax
    orl $0x200, %eax            /* OSFXSR */
    movl %eax, %cr4
    movups %xmm0, 0x3000
    movups 0x3010, %xmm0
    hlt
1:  hlt
    jmp 1b
