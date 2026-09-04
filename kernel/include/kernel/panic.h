/*
 * panic.h - Fatal-error and invariant-check primitives.
 *
 * Semantics, in decreasing severity:
 *
 *   panic()    Unrecoverable state. Prints reason, CPU, trap frame when
 *              available, register state, and a stack trace; then requests
 *              emulator exit with a failure code and halts every CPU.
 *              Never returns.
 *   BUG()      An invariant the code relies on was violated. Same as panic
 *              with a "BUG:" prefix and source location. Never returns.
 *   KASSERT()  Checked invariant, active in every build. Calls panic with
 *              the expression text. Use for conditions whose violation
 *              would corrupt state if execution continued.
 *   WARN()     Something unexpected but survivable. Logs at WARN level with
 *              source location and returns the condition so callers can
 *              take a fallback path. Never halts.
 *
 * All of these are usable from interrupt context. panic disables
 * interrupts on the current CPU and is re-entrancy safe: a panic while
 * panicking prints a short note and halts.
 */

#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <kernel/compiler.h>

struct arch_trap_frame;

void panic(const char *fmt, ...) __noreturn __printf(1, 2);

/* Panic that originated in a trap; `frame` is dumped and the backtrace
 * starts from the interrupted context rather than from panic itself. */
void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...) __noreturn __printf(2, 3);

/* Print a stack trace to the log. `from` may be NULL for "here". */
void backtrace_print(const struct arch_trap_frame *from);

/* Taint: conditions under which a panic report is not from a pristine
 * kernel. Set once, never cleared, printed by the panic path. */
#define TAINT_UNSIGNED_MODULE (1u << 0)
void kernel_taint(unsigned flag);
unsigned kernel_taint_flags(void);

#define BUG() panic("BUG: at %s:%d (%s)", __FILE__, __LINE__, __func__)

#define BUG_ON(cond)                                                           \
    do {                                                                       \
        if (unlikely(cond))                                                    \
            panic("BUG: %s at %s:%d (%s)", #cond, __FILE__, __LINE__, __func__); \
    } while (0)

#define KASSERT(cond)                                                          \
    do {                                                                       \
        if (unlikely(!(cond)))                                                 \
            panic("assertion failed: %s at %s:%d (%s)", #cond, __FILE__, __LINE__, __func__); \
    } while (0)

#define WARN(cond, fmt, ...)                                                   \
    ({                                                                         \
        bool __warn_hit = unlikely(!!(cond));                                  \
        if (__warn_hit)                                                        \
            klog(KLOG_WARN, "WARN at %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
        __warn_hit;                                                            \
    })

#endif /* KERNEL_PANIC_H */
