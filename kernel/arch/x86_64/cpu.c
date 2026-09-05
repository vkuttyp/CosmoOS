/*
 * cpu.c - CPU identification, protection features, and the generic
 * arch/cpu.h + arch/irq.h interfaces for x86-64.
 */

#include <kernel/panic.h>
#include <kernel/string.h>

#include <arch/cpu.h>
#include <arch/irq.h>

#include <x86/cpu.h>
#include <x86/fpu.h>

static struct x86_cpu_info g_cpu;

static void read_brand_string(void)
{
    if (g_cpu.max_ext_leaf < 0x80000004u) {
        strlcpy(g_cpu.brand, "(brand string unsupported)", sizeof(g_cpu.brand));
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        struct cpuid_regs r;
        cpuid(0x80000002u + i, 0, &r);
        memcpy(g_cpu.brand + i * 16 + 0, &r.eax, 4);
        memcpy(g_cpu.brand + i * 16 + 4, &r.ebx, 4);
        memcpy(g_cpu.brand + i * 16 + 8, &r.ecx, 4);
        memcpy(g_cpu.brand + i * 16 + 12, &r.edx, 4);
    }
    g_cpu.brand[48] = '\0';

    /* Strip leading spaces some vendors pad with. */
    char *p = g_cpu.brand;
    while (*p == ' ')
        p++;
    if (p != g_cpu.brand)
        memmove(g_cpu.brand, p, strlen(p) + 1);
}

void x86_cpu_init(void)
{
    struct cpuid_regs r;

    memset(&g_cpu, 0, sizeof(g_cpu));

    cpuid(0, 0, &r);
    g_cpu.max_basic_leaf = r.eax;
    memcpy(g_cpu.vendor + 0, &r.ebx, 4);
    memcpy(g_cpu.vendor + 4, &r.edx, 4);
    memcpy(g_cpu.vendor + 8, &r.ecx, 4);
    g_cpu.vendor[12] = '\0';

    cpuid(0x80000000u, 0, &r);
    g_cpu.max_ext_leaf = r.eax;

    if (g_cpu.max_basic_leaf >= 1) {
        cpuid(1, 0, &r);
        uint32_t family = (r.eax >> 8) & 0xF;
        uint32_t model = (r.eax >> 4) & 0xF;
        g_cpu.stepping = r.eax & 0xF;
        if (family == 0xF)
            family += (r.eax >> 20) & 0xFF;
        if (family == 0x6 || family == 0xF)
            model |= ((r.eax >> 16) & 0xF) << 4;
        g_cpu.family = family;
        g_cpu.model = model;
        g_cpu.has_pge = (r.edx & (1u << 13)) != 0;
        g_cpu.has_apic = (r.edx & (1u << 9)) != 0;
        g_cpu.has_x2apic = (r.ecx & (1u << 21)) != 0;
    }
    if (g_cpu.max_basic_leaf >= 7) {
        cpuid(7, 0, &r);
        g_cpu.has_fsgsbase = (r.ebx & (1u << 0)) != 0;
        g_cpu.has_smep = (r.ebx & (1u << 7)) != 0;
        g_cpu.has_smap = (r.ebx & (1u << 20)) != 0;
        g_cpu.has_umip = (r.ecx & (1u << 2)) != 0;
    }
    if (g_cpu.max_ext_leaf >= 0x80000001u) {
        cpuid(0x80000001u, 0, &r);
        g_cpu.has_nx = (r.edx & (1u << 20)) != 0;
    }
    if (g_cpu.max_ext_leaf >= 0x80000007u) {
        cpuid(0x80000007u, 0, &r);
        g_cpu.has_invariant_tsc = (r.edx & (1u << 8)) != 0;
    }
    read_brand_string();

    x86_cpu_enable_features();
}

void x86_cpu_enable_features(void)
{
    /* Protection features. The loader already set WP and NXE on the
     * boot CPU; asserting them here makes every CPU independent of who
     * started it. */
    write_cr0(read_cr0() | CR0_WP);
    if (g_cpu.has_nx)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

    uint64_t cr4 = read_cr4();
    if (g_cpu.has_pge)
        cr4 |= CR4_PGE;
    if (g_cpu.has_smep)
        cr4 |= CR4_SMEP;
    if (g_cpu.has_smap)
        cr4 |= CR4_SMAP;
    if (g_cpu.has_umip)
        cr4 |= CR4_UMIP;
    /* The paranoid entry (isr.S) recognises the kernel's GS base by its
     * sign bit; that is sound only while user mode cannot choose a GS base
     * (no FSGSBASE, no ARCH_SET_GS). Assert the first half here. */
    cr4 &= ~CR4_FSGSBASE;
    write_cr4(cr4);
    KASSERT((read_cr4() & CR4_FSGSBASE) == 0);

    /* Floating-point/SIMD configuration: identical on every CPU, never
     * inherited from the firmware or the AP trampoline (fpu.c). */
    x86_fpu_init_cpu();
}

const struct x86_cpu_info *x86_cpu_info(void)
{
    return &g_cpu;
}

/* --- arch/cpu.h --- */

void arch_dma_barrier(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

const char *arch_name(void)
{
    return "x86_64";
}

void arch_cpu_brand_string(char *buf, size_t len)
{
    strlcpy(buf, g_cpu.brand[0] ? g_cpu.brand : "(unidentified)", len);
}

/* arch_cpu_id lives in percpu.c: it reads the per-CPU block through GS. */

void arch_cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

void arch_cpu_wait_for_interrupt(void)
{
    if (arch_irq_enabled())
        __asm__ volatile("hlt" ::: "memory");
    else
        __asm__ volatile("sti; hlt; cli" ::: "memory");
}

void arch_cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

/* --- arch/irq.h --- */

arch_irq_state_t arch_irq_save(void)
{
    uint64_t flags = read_rflags();
    __asm__ volatile("cli" ::: "memory");
    return (arch_irq_state_t)flags;
}

void arch_irq_restore(arch_irq_state_t state)
{
    if (state & RFLAGS_IF)
        __asm__ volatile("sti" ::: "memory");
}

void arch_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

void arch_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

bool arch_irq_enabled(void)
{
    return (read_rflags() & RFLAGS_IF) != 0;
}
