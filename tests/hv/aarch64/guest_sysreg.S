/*
 * guest_sysreg.S - A guest reading an ID register
 * (docs/kernel-services/virtualization/testing.md).
 *
 * HCR_EL2.TID3 traps ID-register reads to EL2, which is how the manager
 * hides what its model does not implement -- the CPUID of this
 * architecture. The read below is reported as a SYSREG exit naming the
 * destination register.
 */
    .text
    .globl _start
_start:
    mrs x5, id_aa64pfr0_el1        /* traps: SYSREG exit, destination x5 */
    wfi
1:  b 1b
