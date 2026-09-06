/*
 * el2_stub.S - The resident EL2 stub the loader leaves behind
 * (docs/kernel/arch/aarch64/design.md, "Exception level 2").
 *
 * Copied into one reserved page and pointed at by VBAR_EL2 before the
 * loader drops to EL1, so that EL1 keeps a way back into EL2. It runs
 * with the EL2 MMU off, is position independent, and touches nothing
 * but the registers of the call it answers.
 *
 * ABI: HVC #0 from EL1 with the selector in x0.
 *   0 -> the stub's version in x0
 *   1 -> VBAR_EL2 = x1 (physical), x0 = 0
 *   2 -> VBAR_EL2 = this stub again, x0 = 0
 *   3 -> SP_EL2 = x1, x0 = 0 (a hypervisor's EL2 vectors need a stack
 *        before their first exception, and only code at EL2 can set it)
 *   anything else -> x0 = -1
 * Any other exception at EL2 returns where it came from: this is a door,
 * not a hypervisor.
 */

#define EL2_STUB_VERSION 2

    /* The loader is PE/COFF: plain .text, no ELF section attributes. */
    .text
    .balign 2048
    .globl el2_stub_begin
el2_stub_begin:

/* A vector table is 16 entries of 0x80 bytes; only "lower EL, AArch64,
 * synchronous" (offset 0x400) does anything. */
.macro VECTOR_ENTRY label
    .balign 0x80
    b \label
.endm

    /* current EL, SP0 */
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    /* current EL, SPx */
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    /* lower EL, AArch64 */
    VECTOR_ENTRY el2_stub_sync
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    /* lower EL, AArch32 */
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return
    VECTOR_ENTRY el2_stub_return

    .balign 16
el2_stub_sync:
    mrs x2, esr_el2
    lsr x2, x2, #26              /* EC */
    cmp x2, #0x16                /* HVC from AArch64 */
    b.ne el2_stub_return

    cmp x0, #0
    b.eq 1f
    cmp x0, #1
    b.eq 2f
    cmp x0, #2
    b.eq 3f
    cmp x0, #3
    b.eq 4f
    mov x0, #-1
    b el2_stub_return

1:  mov x0, #EL2_STUB_VERSION
    b el2_stub_return

2:  msr vbar_el2, x1
    isb
    mov x0, #0
    b el2_stub_return

3:  adr x1, el2_stub_begin       /* within a page: no page-relative relocation */
    msr vbar_el2, x1
    isb
    mov x0, #0
    b el2_stub_return

4:  mov x2, sp                   /* SP_EL2 is `sp` while executing at EL2 */
    mov sp, x1
    mov x0, #0
    /* fall through */

el2_stub_return:
    eret

    .globl el2_stub_end
el2_stub_end:
