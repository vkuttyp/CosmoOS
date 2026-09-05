/* backtrace.c - Walk the x29 frame chain (docs/kernel/arch/aarch64/design.md). */

#include <kernel/kernel.h>
#include <kernel/percpu.h>
#include <kernel/thread.h>
#include <arch/backtrace.h>
#include <aarch64/trapframe.h>

struct stack_frame {
    struct stack_frame *fp;   /* saved x29 */
    uint64_t lr;              /* saved x30 */
};

static bool in_known_stack(uintptr_t a, size_t len)
{
    if (kernel_image_contains(a) && kernel_image_contains(a + len - 1))
        return true;
    struct thread *cur = this_cpu()->current;
    return cur != NULL && thread_stack_contains(cur, a) && thread_stack_contains(cur, a + len - 1);
}

static bool frame_ok(const struct stack_frame *fp, const struct stack_frame *prev)
{
    uintptr_t a = (uintptr_t)fp;
    if (a == 0 || (a & 0xF) != 0)
        return false;
    if (!in_known_stack(a, sizeof(*fp)))
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
        pcs[n++] = (uintptr_t)from->elr;
        fp = (const struct stack_frame *)(uintptr_t)from->x[29];
    } else {
        fp = __builtin_frame_address(0);
    }
    const struct stack_frame *prev = NULL;
    while (n < max && frame_ok(fp, prev)) {
        if (fp->lr == 0)
            break;
        pcs[n++] = (uintptr_t)fp->lr;
        prev = fp;
        fp = fp->fp;
    }
    return n;
}
