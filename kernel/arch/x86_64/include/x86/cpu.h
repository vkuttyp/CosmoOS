/*
 * x86/cpu.h - Control registers, MSRs, CPUID, and the x86 start path.
 * Private to the x86-64 architecture layer.
 */

#ifndef X86_CPU_H
#define X86_CPU_H

#include <kernel/compiler.h>

struct cosmoboot_info;

/* Called from entry.S with the boot info pointer; never returns. */
void x86_start(const struct cosmoboot_info *info) __noreturn;

struct x86_cpu_info {
    char vendor[13];
    char brand[49];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_basic_leaf;
    uint32_t max_ext_leaf;
    bool has_nx;
    bool has_smep;
    bool has_smap;
    bool has_umip;
    bool has_pge;
    bool has_apic;
    bool has_x2apic;
    bool has_fsgsbase;
    bool has_invariant_tsc;
};

/* Identify the CPU and enable the protection features it supports
 * (WP, PGE, SMEP, SMAP, UMIP). Called once per CPU during start-up. */
void x86_cpu_init(void);
const struct x86_cpu_info *x86_cpu_info(void);

/* CR0 */
#define CR0_PE (1ULL << 0)
#define CR0_WP (1ULL << 16)
#define CR0_PG (1ULL << 31)

/* CR4 */
#define CR4_PAE      (1ULL << 5)
#define CR4_PGE      (1ULL << 7)
#define CR4_UMIP     (1ULL << 11)
#define CR4_FSGSBASE (1ULL << 16)
#define CR4_SMEP     (1ULL << 20)
#define CR4_SMAP     (1ULL << 21)

/* EFER */
#define MSR_EFER  0xC0000080u
#define EFER_SCE  (1ULL << 0)
#define EFER_LME  (1ULL << 8)
#define EFER_LMA  (1ULL << 10)
#define EFER_NXE  (1ULL << 11)

#define MSR_APIC_BASE 0x0000001Bu

/* RFLAGS */
#define RFLAGS_IF (1ULL << 9)

struct cpuid_regs {
    uint32_t eax, ebx, ecx, edx;
};

static inline void cpuid(uint32_t leaf, uint32_t subleaf, struct cpuid_regs *r)
{
    __asm__ volatile("cpuid"
                     : "=a"(r->eax), "=b"(r->ebx), "=c"(r->ecx), "=d"(r->edx)
                     : "a"(leaf), "c"(subleaf));
}

static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
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

static inline uint64_t read_rflags(void)
{
    uint64_t v;
    __asm__ volatile("pushfq; popq %0" : "=r"(v) : : "memory");
    return v;
}

#endif /* X86_CPU_H */
