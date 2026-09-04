/*
 * arch/backtrace.h - Stack unwinding.
 *
 * Frame-pointer based; the kernel is built with -fno-omit-frame-pointer
 * on every architecture. The walk stops at the first frame pointer that
 * is not inside a known kernel stack or the kernel image, so a corrupted
 * stack yields a short trace rather than a fault inside the unwinder.
 *
 * Safe in interrupt and panic context. Never allocates.
 */

#ifndef ARCH_BACKTRACE_H
#define ARCH_BACKTRACE_H

#include <kernel/compiler.h>

struct arch_trap_frame;

/* Fill `pcs` with up to `max` return addresses, innermost first. When
 * `from` is NULL the walk starts at the caller; otherwise at the
 * interrupted context described by the trap frame (its PC is entry 0).
 * Returns the number of entries written. */
size_t arch_backtrace(uintptr_t *pcs, size_t max, const struct arch_trap_frame *from);

#endif /* ARCH_BACKTRACE_H */
