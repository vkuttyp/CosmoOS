/*
 * uaccess.S - The user copy primitive with exception fixups
 * (docs/kernel/memory/design.md §6.1).
 *
 * size_t arch_copy_user_raw(void *dst, const void *src, size_t n)
 *   rdi = dst, rsi = src, rdx = n; returns bytes not copied in rax.
 *
 * One `rep movsb` (fast with ERMS, correct everywhere). A fault inside
 * it leaves rcx = the bytes still to copy and rip at the instruction; the
 * fault handler resumes at the fixup, which returns that count. The
 * caller has opened the STAC window; nothing here changes it.
 */
    .section .text, "ax", @progbits
    .globl arch_copy_user_raw
    .type arch_copy_user_raw, @function
arch_copy_user_raw:
    mov %rdx, %rcx
    cld
1:  rep movsb
    xor %eax, %eax
    ret
9:  mov %rcx, %rax
    ret
    .size arch_copy_user_raw, . - arch_copy_user_raw

    .pushsection .ex_table, "a", @progbits
    .balign 4
    .long 1b - ., 9b - .
    .popsection

    .section .note.GNU-stack, "", @progbits
