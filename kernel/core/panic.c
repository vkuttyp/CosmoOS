/*
 * panic.c - Fatal error reporting.
 *
 * Output order is chosen so that the most useful line comes first even if
 * everything after it is lost: reason, then CPU/context, then registers,
 * then the stack. Symbolisation of addresses is a later diagnostics task;
 * until then the addresses are resolved offline against kernel.map.
 */

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/kernel.h>

#include <arch/backtrace.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/shutdown.h>
#include <arch/trap.h>

#define BACKTRACE_MAX 32

static volatile int g_panicking;

void backtrace_print(const struct arch_trap_frame *from)
{
    uintptr_t pcs[BACKTRACE_MAX];
    size_t n = arch_backtrace(pcs, BACKTRACE_MAX, from);

    kprintf("stack trace:\n");
    for (size_t i = 0; i < n; i++) {
        const char *where = kernel_text_contains(pcs[i]) ? "" : " (outside kernel text)";
        kprintf("  #%-2zu %p%s\n", i, (void *)pcs[i], where);
    }
    if (n == 0)
        kprintf("  (no frames)\n");
    else if (n == BACKTRACE_MAX)
        kprintf("  ... (truncated)\n");
}

static void __noreturn __printf(2, 0)
panic_common(const struct arch_trap_frame *frame, const char *fmt, va_list ap)
{
    arch_irq_disable();

    if (g_panicking) {
        kprintf("\nKERNEL PANIC (recursive) on CPU %u: ", arch_cpu_id());
        kvprintf(fmt, ap);
        kprintf("\n");
        arch_emulator_exit(ARCH_EMULATOR_EXIT_FAILURE);
        arch_cpu_halt_forever();
    }
    g_panicking = 1;

    kprintf("\nKERNEL PANIC: ");
    kvprintf(fmt, ap);
    kprintf("\nCPU: %u  context: boot (no threads yet)\n", arch_cpu_id());

    if (frame != NULL)
        arch_trap_frame_dump(frame);

    backtrace_print(frame);

    kprintf("halting.\n");
    arch_emulator_exit(ARCH_EMULATOR_EXIT_FAILURE);
    arch_cpu_halt_forever();
}

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panic_common(NULL, fmt, ap);
    /* not reached; va_end omitted deliberately */
}

void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panic_common(frame, fmt, ap);
}
