/*
 * guest_idreg.S - A guest reading its feature registers
 * (docs/kernel-services/virtualization/testing.md).
 *
 * A real kernel reads these to learn what it is running on; this guest
 * reads the ones the model shapes and reports each through a hypercall,
 * x0 = a tag (the CRm and Op2 of the register), x1 = the value it read.
 * A read of an ID register in the trapped space that the model does not
 * name (S3_0_c0_c4_4, ID_AA64ZFR0_EL1 -- SVE) is read too, to show it is
 * answered as zero rather than reaching the owner. Then hvc x0 = 0, done.
 *
 * With HCR_EL2.TID3 every read below traps to EL2; none reaches the
 * owner, because the model completes each in the kernel. If the model
 * did not, the first read would be a SYSREG exit and this guest would
 * never make its first hypercall.
 */
    .text
    .globl _start
_start:
    mrs  x1, id_aa64pfr0_el1
    movz x0, #0x40
    hvc  #0
    mrs  x1, id_aa64pfr1_el1
    movz x0, #0x41
    hvc  #0
    mrs  x1, id_aa64dfr0_el1
    movz x0, #0x50
    hvc  #0
    mrs  x1, id_aa64isar1_el1
    movz x0, #0x61
    hvc  #0
    mrs  x1, S3_0_c0_c6_2          /* ID_AA64ISAR2_EL1 */
    movz x0, #0x62
    hvc  #0
    mrs  x1, id_aa64mmfr0_el1
    movz x0, #0x70
    hvc  #0
    mrs  x1, id_aa64mmfr1_el1
    movz x0, #0x71
    hvc  #0
    mrs  x1, S3_0_c0_c4_4          /* ID_AA64ZFR0_EL1: SVE, in the space, not named -> 0 */
    movz x0, #0x44
    hvc  #0
    mov  x0, #0
    hvc  #0
1:  b    1b
