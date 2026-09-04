/*
 * trampoline.S - Application processor start code.
 *
 * Copied by the boot CPU to physical TRAMPOLINE_BASE (0x8000) and entered
 * by a SIPI with vector 0x08 in real mode at CS:IP = 0x0800:0000. Every
 * address is computed as (symbol - x86_trampoline_start + TRAMPOLINE_BASE)
 * at assembly time, so the blob runs at 0x8000 no matter where the
 * kernel image places it. The data block at the end is patched by
 * smp.c before each start.
 *
 * Sequence: real mode -> protected mode (GDT with 32-bit code/data) ->
 * PAE + kernel CR3 + EFER.LME|NXE -> paging on -> 64-bit code -> load
 * stack and CPU index -> jump to x86_ap_entry.
 */

#define TRAMPOLINE_BASE 0x8000

#define TR_OFF(sym) ((sym) - x86_trampoline_start)
#define TR_ABS(sym) (TR_OFF(sym) + TRAMPOLINE_BASE)

    .section .rodata.trampoline, "a", @progbits
    .balign 16
    .globl x86_trampoline_start
    .globl x86_trampoline_end
x86_trampoline_start:

    .code16
tr_start16:
    cli
    cld
    /* CS = 0x0800; make the data segments match so blob-relative
     * offsets address the copied blob. */
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    lgdtl TR_OFF(tr_gdtr)

    movl %cr0, %eax
    orl $0x1, %eax              /* PE */
    movl %eax, %cr0

    ljmpl $0x08, $TR_ABS(tr_pm32)

    .code32
tr_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl %cr4, %eax
    orl $0x20, %eax             /* PAE */
    movl %eax, %cr4

    movl TR_ABS(tr_cr3), %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx      /* EFER */
    rdmsr
    orl $0x900, %eax            /* LME | NXE */
    wrmsr

    movl %cr0, %eax
    orl $0x80010000, %eax       /* PG | WP */
    movl %eax, %cr0

    ljmpl $0x18, $TR_ABS(tr_lm64)

    .code64
tr_lm64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs

    movq TR_ABS(tr_stack), %rsp
    movl TR_ABS(tr_cpu), %edi
    movq TR_ABS(tr_entry), %rax
    xorl %ebp, %ebp
    pushq $0
    pushq $0
    jmp *%rax

    /* --- GDT for the transition --- */
    .balign 8
tr_gdt:
    .quad 0x0000000000000000    /* null */
    .quad 0x00CF9A000000FFFF    /* 0x08: 32-bit code, base 0, 4 GiB */
    .quad 0x00CF92000000FFFF    /* 0x10: 32-bit data, base 0, 4 GiB */
    .quad 0x00AF9A000000FFFF    /* 0x18: 64-bit code */
tr_gdt_end:
tr_gdtr:
    .word tr_gdt_end - tr_gdt - 1
    .long TR_ABS(tr_gdt)

    /* --- data block patched by smp.c (offsets exported below) --- */
    .balign 8
tr_cr3:
    .quad 0
tr_entry:
    .quad 0
tr_stack:
    .quad 0
tr_cpu:
    .quad 0
x86_trampoline_end:

    /* Offsets of the patchable fields, readable from C. */
    .globl x86_trampoline_off_cr3
    .globl x86_trampoline_off_entry
    .globl x86_trampoline_off_stack
    .globl x86_trampoline_off_cpu
    .balign 4
x86_trampoline_off_cr3:
    .long TR_OFF(tr_cr3)
x86_trampoline_off_entry:
    .long TR_OFF(tr_entry)
x86_trampoline_off_stack:
    .long TR_OFF(tr_stack)
x86_trampoline_off_cpu:
    .long TR_OFF(tr_cpu)

    .section .note.GNU-stack, "", @progbits
