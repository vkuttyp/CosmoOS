/*
 * arch/percpu.h - Fast per-CPU pointer, implemented per architecture.
 *
 * arch_percpu_install() makes `pc` the calling CPU's instance; it must
 * run before any code that calls arch_cpu_id(), spin_lock(), or
 * this_cpu() on that CPU. arch_percpu_get() is the hot path and must be
 * a few instructions with no memory allocation or locking.
 */

#ifndef ARCH_PERCPU_H
#define ARCH_PERCPU_H

struct percpu;

void arch_percpu_install(struct percpu *pc);
struct percpu *arch_percpu_get(void);

#endif /* ARCH_PERCPU_H */
