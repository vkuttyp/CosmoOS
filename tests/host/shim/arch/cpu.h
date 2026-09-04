/*
 * Host shim for arch/cpu.h. One CPU, no halting.
 */

#ifndef HOST_SHIM_ARCH_CPU_H
#define HOST_SHIM_ARCH_CPU_H

#include <stddef.h>
#include <stdlib.h>

static inline const char *arch_name(void) { return "host"; }
static inline void arch_cpu_brand_string(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline unsigned arch_cpu_id(void) { return 0; }
static inline void arch_cpu_relax(void) {}
static inline void arch_cpu_wait_for_interrupt(void) {}
static inline void arch_cpu_halt_forever(void) { abort(); }

#endif /* HOST_SHIM_ARCH_CPU_H */
