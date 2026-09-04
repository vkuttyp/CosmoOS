/*
 * panic.c - Fatal error reporting.
 *
 * Output order is chosen so that the most useful line comes first even if
 * everything after it is lost: reason, then CPU/context, then registers,
 * then the stack. Symbolisation of addresses is a later diagnostics task;
 * until then the addresses are resolved offline against kernel.map.
 */

#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/smp.h>

#include <arch/backtrace.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/shutdown.h>
#include <arch/trap.h>

static unsigned g_taint;

void kernel_taint(unsigned flag)
{
    __atomic_fetch_or(&g_taint, flag, __ATOMIC_RELAXED);
}

unsigned kernel_taint_flags(void)
{
    return __atomic_load_n(&g_taint, __ATOMIC_RELAXED);
}

#define BACKTRACE_MAX 32

static volatile int g_panicking;      /* 0 = none, else 1 + CPU id of the first panicker */

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

    unsigned cpu = arch_cpu_id();
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_panicking, &expected, (int)cpu + 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        /* Someone else is panicking. If it is this CPU, the panic path
         * itself faulted; otherwise we lost the race and stay out of
         * the report. */
        console_set_panic_mode();
        kprintf("\nKERNEL PANIC (%s) on CPU %u: ", expected == (int)cpu + 1 ? "recursive" : "concurrent", cpu);
        kvprintf(fmt, ap);
        kprintf("\n");
        if (expected == (int)cpu + 1)
            arch_emulator_exit(ARCH_EMULATOR_EXIT_FAILURE);
        arch_cpu_halt_forever();
    }

    /* Stop the other CPUs before printing so they cannot interleave or
     * keep mutating the state being reported, then drop the console lock
     * (one of them may have been holding it). */
    smp_stop_others();
    console_set_panic_mode();

    kprintf("\nKERNEL PANIC: ");
    kvprintf(fmt, ap);
    kprintf("\nCPU: %u  context: boot (no threads yet)\n", arch_cpu_id());

    if (frame != NULL)
        arch_trap_frame_dump(frame);

    backtrace_print(frame);

    if (g_taint)
        kprintf("taint: 0x%x%s\n", g_taint, (g_taint & TAINT_UNSIGNED_MODULE) ? " (unsigned module loaded)" : "");

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

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(panic);
