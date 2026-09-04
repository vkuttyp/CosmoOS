/*
 * cpu.c - x86-64 control-register and MSR access for the loader.
 *
 * UEFI applications on x86-64 run at CPL 0 in long mode, so privileged
 * instructions are available. Nothing here is called after the jump to
 * the kernel.
 */

#include "loader.h"

#define MSR_EFER      0xC0000080u
#define EFER_NXE      (1u << 11)
#define CR0_WP        (1ULL << 16)
#define CPUID_EXT_NX  (1u << 20)

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

bool cpu_has_nx(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u)
        return false;
    cpuid(0x80000001u, &a, &b, &c, &d);
    return (d & CPUID_EXT_NX) != 0;
}

void cpu_enable_nx(void)
{
    uint64_t efer = rdmsr(MSR_EFER);
    if ((efer & EFER_NXE) == 0)
        wrmsr(MSR_EFER, efer | EFER_NXE);
}

void cpu_enable_wp(void)
{
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if ((cr0 & CR0_WP) == 0) {
        cr0 |= CR0_WP;
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    }
}

void cpu_jump_to_kernel(uint64_t cr3, uint64_t stack_top, uint64_t info, uint64_t entry)
{
    /*
     * Register roles are fixed by constraints so the sequence cannot
     * clobber an input before it is consumed:
     *   rax = cr3, rdx = stack, rdi = info (kernel's first argument),
     *   rsi = entry.
     * The kernel entry is a jump target, not a call, so there is no return
     * address and no ABI on the stack.
     */
    __asm__ volatile(
        "cli\n\t"
        "mov %0, %%cr3\n\t"
        "mov %1, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "jmp *%3\n\t"
        :
        : "a"(cr3), "d"(stack_top), "D"(info), "S"(entry)
        : "memory");
    __builtin_unreachable();
}
