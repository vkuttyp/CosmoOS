/*
 * compiler.h - Compiler attributes and small utilities for kernel code.
 *
 * The kernel is built with clang only. Anything here that would need a
 * per-compiler variant is a signal that the toolchain policy changed and
 * this file must be revisited.
 */

#ifndef KERNEL_COMPILER_H
#define KERNEL_COMPILER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define __noreturn      __attribute__((noreturn))
#define __maybe_unused  __attribute__((unused))
#define __always_used   __attribute__((used))
#define __packed        __attribute__((packed))
#define __aligned(x)    __attribute__((aligned(x)))
#define __section(s)    __attribute__((section(s)))
#define __printf(a, b)  __attribute__((format(printf, a, b)))
#define __must_check    __attribute__((warn_unused_result))
#define __noinline      __attribute__((noinline))
/* Not named __always_inline: glibc's sys/cdefs.h defines that, and the
 * host unit tests compile these headers against libc. */
#define __force_inline  inline __attribute__((always_inline))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Compiler-only barrier: prevents reordering of memory accesses across it
 * in generated code. Not a CPU fence. */
#define barrier() __asm__ volatile("" ::: "memory")

#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + ((__typeof__(x))(a) - 1), (a))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

#define KiB(n) ((uint64_t)(n) << 10)
#define MiB(n) ((uint64_t)(n) << 20)
#define GiB(n) ((uint64_t)(n) << 30)

#endif /* KERNEL_COMPILER_H */
