/*
 * uaccess.S - The user copy primitive with exception fixups
 * (docs/kernel/memory/design.md §6.1).
 *
 * size_t arch_copy_user_raw(void *dst, const void *src, size_t n)
 *   x0 = dst, x1 = src, x2 = n; returns bytes not copied in x0.
 *
 * Eight bytes at a time when both pointers are 8-byte aligned, else one
 * byte at a time (SCTLR_EL1.A is not assumed clear). Every load and
 * store is listed in the exception table; the fixup computes what is
 * left from the destination cursor, which the faulting element has not
 * advanced. The caller has cleared PAN; nothing here changes it.
 */
    .section .text, "ax", @progbits
    .globl arch_copy_user_raw
    .type arch_copy_user_raw, @function
arch_copy_user_raw:
    add x3, x0, x2              /* end of the destination */
    mov x4, x0                  /* destination cursor */
    orr x6, x0, x1
    tst x6, #7
    b.ne 3f
    cmp x2, #8
    b.lo 3f
1:  ldr x5, [x1]
2:  str x5, [x4]
    add x1, x1, #8
    add x4, x4, #8
    sub x2, x2, #8
    cmp x2, #8
    b.hs 1b
3:  cbz x2, 6f
4:  ldrb w5, [x1]
5:  strb w5, [x4]
    add x1, x1, #1
    add x4, x4, #1
    subs x2, x2, #1
    b.ne 4b
6:  mov x0, #0
    ret
9:  sub x0, x3, x4              /* bytes not copied */
    ret
    .size arch_copy_user_raw, . - arch_copy_user_raw

    .pushsection .ex_table, "a", @progbits
    .balign 4
    .long 1b - ., 9b - .
    .long 2b - ., 9b - .
    .long 4b - ., 9b - .
    .long 5b - ., 9b - .
    .popsection
