/*
 * backtrace.c - Frame-pointer stack walk for x86-64.
 *
 * A frame is { saved rbp, return address }. The walk stops when the
 * next frame pointer is not strictly above the current one (loop or
 * corruption), not 16-byte aligned, or outside the kernel image, where
 * every stack lives until the memory manager exists. entry.S pushes a
 * zero frame at the base so a clean stack terminates naturally.
 */

#include <kernel/kernel.h>

#include <arch/backtrace.h>

#include <x86/trapframe.h>

struct stack_frame {
    struct stack_frame *rbp;
    uint64_t rip;
};

static bool frame_ok(const struct stack_frame *fp, const struct stack_frame *prev)
{
    uintptr_t a = (uintptr_t)fp;
    if (a == 0 || (a & 0xF) != 0)
        return false;
    if (!kernel_image_contains(a) || !kernel_image_contains(a + sizeof(*fp) - 1))
        return false;
    if (prev != NULL && fp <= prev)
        return false;
    return true;
}

size_t arch_backtrace(uintptr_t *pcs, size_t max, const struct arch_trap_frame *from)
{
    size_t n = 0;
    const struct stack_frame *fp;

    if (max == 0)
        return 0;

    if (from != NULL) {
        pcs[n++] = (uintptr_t)from->rip;
        fp = (const struct stack_frame *)(uintptr_t)from->rbp;
    } else {
        fp = __builtin_frame_address(0);
    }

    const struct stack_frame *prev = NULL;
    while (n < max && frame_ok(fp, prev)) {
        if (fp->rip == 0)
            break;
        pcs[n++] = (uintptr_t)fp->rip;
        prev = fp;
        fp = fp->rbp;
    }
    return n;
}
