/*
 * Host shim for arch/cpu.h. One CPU, no halting.
 *
 * No <stdlib.h> here: on Darwin it drags in <machine/endian.h>, whose
 * htons/ntohs macros break the kernel's inline ones (kernel/net/inet.h)
 * in any file that includes this header first (arp.c, ipv6.c).
 */

#ifndef HOST_SHIM_ARCH_CPU_H
#define HOST_SHIM_ARCH_CPU_H

#include <stddef.h>
void abort(void) __attribute__((noreturn));

static inline const char *arch_name(void) { return "host"; }
static inline void arch_cpu_brand_string(char *buf, size_t len) { if (len) buf[0] = '\0'; }
static inline unsigned arch_cpu_id(void) { return 0; }
static inline void arch_cpu_relax(void) {}
static inline void arch_cpu_wait_for_interrupt(void) {}
static inline void arch_cpu_halt_forever(void) { abort(); }

#endif /* HOST_SHIM_ARCH_CPU_H */
