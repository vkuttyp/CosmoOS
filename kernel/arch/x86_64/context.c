/*
 * context.c - Initial stack layout for a new thread context.
 *
 * Layout at stack_top (16-byte aligned), growing down:
 *   top-8   entry function            (popped by x86_context_start)
 *   top-16  &x86_context_start        (the `ret` target of the first switch)
 *   top-24  rbp = 0
 *   top-32  rbx = 0
 *   top-40  r12 = 0
 *   top-48  r13 = 0
 *   top-56  r14 = 0
 *   top-64  r15 = 0                   <- saved sp
 *
 * After the six pops and the ret, rsp = top-8, so x86_context_start pops
 * the entry, pushes a zero return address, and jumps with rsp = top-8:
 * 8 mod 16, exactly what the ABI expects at function entry.
 */

#include <kernel/panic.h>
#include <kernel/string.h>

#include <arch/context.h>

void x86_context_start(void);
extern char x86_boot_stack_bottom[], x86_boot_stack_top[];

void arch_boot_stack(uintptr_t *base, size_t *size)
{
    *base = (uintptr_t)x86_boot_stack_bottom;
    *size = (size_t)(x86_boot_stack_top - x86_boot_stack_bottom);
}

void arch_context_init(struct arch_context *ctx, uintptr_t stack_top, void (*entry)(void))
{
    KASSERT((stack_top & 0xF) == 0);

    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)(uintptr_t)entry;
    *--sp = (uint64_t)(uintptr_t)x86_context_start;
    for (int i = 0; i < 6; i++)
        *--sp = 0;

    ctx->sp = (uintptr_t)sp;
}
