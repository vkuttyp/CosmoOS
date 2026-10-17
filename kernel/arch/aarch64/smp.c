/*
 * smp.c - Secondary CPUs through PSCI CPU_ON (docs/kernel/arch/aarch64/design.md, "SMP").
 *
 * The trampoline (trampoline.S) starts with the MMU off at the physical address
 * of its page, which a temporary TTBR0 table identity-maps; it loads the
 * kernel's translation registers from a per-CPU mailbox, enables the MMU
 * and jumps to aarch64_ap_entry at its higher-half address.
 */

#include <kernel/acpi.h>
#include <kernel/bootinfo.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>
#include <arch/irqc.h>
#include <arch/el2.h>
#include <arch/percpu.h>
#include <arch/smp.h>
#include <arch/user.h>
#include <aarch64/platform.h>
#include <aarch64/fpu.h>
#include <aarch64/sysreg.h>

struct aarch64_ap_mailbox {
    uint64_t ttbr0, ttbr1, mair, tcr, sctlr;   /* 0x00 .. 0x20 */
    uint64_t stack_top, entry, cpu;            /* 0x28 .. 0x38 */
    uint64_t el2_stub;                         /* 0x40: PA of the EL2 stub, 0 without EL2 */
} __attribute__((aligned(64)));

extern const uint8_t aarch64_ap_trampoline[], aarch64_ap_trampoline_end[];
extern const uint8_t aarch64_vectors[];

static struct aarch64_ap_mailbox g_mailbox[CONFIG_MAX_CPUS];
static volatile uint32_t g_ap_started[CONFIG_MAX_CPUS];
static struct arch_mmu_context g_tramp_ctx;
static bool g_tramp_ready;
static bool g_ap_stranded;
static bool g_use_hvc;

static int64_t psci_call(uint32_t fn, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a0;
    register uint64_t x2 __asm__("x2") = a1;
    register uint64_t x3 __asm__("x3") = a2;
    if (g_use_hvc)
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

/*
 * The PSCI conduit, from the FADT's ARM boot flags: PSCI compliant, and
 * HVC or SMC. Nothing is guessed: QEMU's firmware alone, whose virt
 * machine answers on HVC, keeps that default when its FADT says nothing
 * (docs/kernel/arch/aarch64/design.md, "Platform discovery"). Returns 1
 * for HVC, 0 for SMC, -1 when there is no PSCI to call.
 */
int aarch64_psci_conduit(void)
{
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    uint16_t flags = 0;
    if (fadt && fadt->length >= 131)
        memcpy(&flags, (const uint8_t *)fadt + 129, 2);
    if (flags & FADT_ARM_PSCI_COMPLIANT)
        return (flags & FADT_ARM_PSCI_USE_HVC) ? 1 : 0;
    return acpi_platform_is_qemu() ? 1 : -1;
}

void aarch64_psci_system_off(void)
{
    /* The conduit afresh: a one-CPU boot never ran psci_probe. */
    int conduit = aarch64_psci_conduit();
    if (conduit < 0) {
        kinfo("shutdown: no PSCI to power off with");
        return;
    }
    g_use_hvc = conduit == 1;
    kinfo("shutdown: power off through PSCI SYSTEM_OFF (%s)", g_use_hvc ? "HVC" : "SMC");
    int64_t rc = psci_call(PSCI_FN_SYSTEM_OFF, 0, 0, 0);
    kerror("shutdown: PSCI SYSTEM_OFF returned %lld", (long long)rc);
}

/* Shell completeness S5: the machine restarts (QEMU without -no-reboot
 * resets; with it, QEMU exits). Returns only where there is no PSCI or
 * the call failed. */
void aarch64_psci_system_reset(void)
{
    int conduit = aarch64_psci_conduit();
    if (conduit < 0) {
        kinfo("reboot: no PSCI to reset with");
        return;
    }
    g_use_hvc = conduit == 1;
    kinfo("reboot: restart through PSCI SYSTEM_RESET (%s)", g_use_hvc ? "HVC" : "SMC");
    int64_t rc = psci_call(PSCI_FN_SYSTEM_RESET, 0, 0, 0);
    kerror("reboot: PSCI SYSTEM_RESET returned %lld", (long long)rc);
}

static bool g_have_psci;

static void psci_probe(void)
{
    static bool probed;
    if (probed)
        return;
    probed = true;
    el2_init(bootinfo_get());   /* before any AP starts: they need the stub's address */
    int conduit = aarch64_psci_conduit();
    if (conduit < 0) {
        kerror("smp: the FADT declares no PSCI and this firmware is not QEMU's: no secondary CPUs");
        return;
    }
    g_use_hvc = conduit == 1;
    g_have_psci = true;
    int64_t ver = psci_call(PSCI_FN_VERSION, 0, 0, 0);
    kdebug("smp: PSCI %lld.%lld via %s", (long long)(ver >> 16), (long long)(ver & 0xFFFF), g_use_hvc ? "HVC" : "SMC");
}

uint32_t arch_smp_boot_hw_id(void)
{
    return (uint32_t)MPIDR_AFFINITY(READ_SYSREG(mpidr_el1));
}

int arch_smp_prepare_cpu(unsigned cpu)
{
    (void)cpu;
    return 0;
}

static paddr_t kernel_va_to_pa(const void *va)
{
    const struct cosmoboot_info *info = bootinfo_get();
    return info->kernel_phys_base + ((uintptr_t)va - info->kernel_virt_base);
}

static void trampoline_install(void)
{
    if (g_tramp_ready)
        return;
    psci_probe();
    if (arch_mmu_context_init_user(&g_tramp_ctx, &kernel_space.mmu))
        panic("smp: cannot allocate the trampoline table");
    paddr_t lo = page_align_down(kernel_va_to_pa(aarch64_ap_trampoline));
    paddr_t hi = page_align_up(kernel_va_to_pa(aarch64_ap_trampoline_end));
    int rc = arch_mmu_map(&g_tramp_ctx, (vaddr_t)lo, lo, (size_t)(hi - lo), VM_PROT_RX, VM_CACHE_WB, 0);
    if (rc)
        panic("smp: cannot identity-map the trampoline (%d)", rc);
    g_tramp_ready = true;
}

static void clean_lines(const void *p, size_t len)
{
    uintptr_t a = (uintptr_t)p & ~63ull;
    for (; a < (uintptr_t)p + len; a += 64)
        __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    dsb_sy();
}

int arch_smp_start_cpu(unsigned cpu, uint32_t hw_id, uintptr_t stack_top)
{
    KASSERT(cpu > 0 && cpu < CONFIG_MAX_CPUS);
    trampoline_install();
    if (!g_have_psci)
        return -ENODEV;   /* psci_probe said why, once */
    struct aarch64_ap_mailbox *mb = &g_mailbox[cpu];
    mb->ttbr0 = g_tramp_ctx.root;
    mb->ttbr1 = kernel_space.mmu.root;
    mb->mair = READ_SYSREG(mair_el1);
    mb->tcr = READ_SYSREG(tcr_el1);
    mb->sctlr = READ_SYSREG(sctlr_el1);
    mb->stack_top = stack_top;
    mb->entry = (uint64_t)(uintptr_t)aarch64_ap_entry;
    mb->cpu = cpu;
    mb->el2_stub = el2_stub_phys();   /* PSCI starts it at EL2 too, if there is one */
    clean_lines(mb, sizeof(*mb));
    __atomic_store_n(&g_ap_started[cpu], 0u, __ATOMIC_RELEASE);
    /* MPIDR affinity fields back into the register layout PSCI expects. */
    uint64_t mpidr = (hw_id & 0xFFFFFFull) | ((uint64_t)(hw_id >> 24) << 32);
    /* The mailbox is a static in the kernel image (.bss), not in the direct map. */
    int64_t rc = psci_call(PSCI_FN_CPU_ON, mpidr, kernel_va_to_pa(aarch64_ap_trampoline), kernel_va_to_pa(mb));
    if (rc != PSCI_RET_SUCCESS) {
        kwarn("smp: PSCI CPU_ON for MPIDR 0x%llx failed (%lld)", (unsigned long long)mpidr, (long long)rc);
        return -EIO;
    }
    for (unsigned i = 0; i < 2000; i++) {
        if (__atomic_load_n(&g_ap_started[cpu], __ATOMIC_ACQUIRE))
            return 0;
        udelay(100);
    }
    g_ap_stranded = true;
    return -ETIMEDOUT;
}

void arch_smp_finish(void)
{
    if (!g_tramp_ready)
        return;
    if (g_ap_stranded) {
        kwarn("smp: a CPU never reached the kernel; trampoline table kept");
        return;
    }
    arch_mmu_context_destroy(&g_tramp_ctx);
    g_tramp_ready = false;
}

void aarch64_ap_entry(unsigned cpu)
{
    __atomic_store_n(&g_ap_started[cpu], 1u, __ATOMIC_RELEASE);
    WRITE_SYSREG(vbar_el1, (uint64_t)(uintptr_t)aarch64_vectors);
    struct percpu *pc = percpu_get(cpu);
    KASSERT(pc != NULL);
    arch_percpu_install(pc);
    arch_mmu_activate(&kernel_space.mmu, true);   /* leaves the trampoline's TTBR0 behind */
    if (aarch64_cpu_info()->has_pan) {
        uint64_t sctlr = READ_SYSREG(sctlr_el1);
        WRITE_SYSREG(sctlr_el1, sctlr | SCTLR_SPAN);
        __asm__ volatile(".inst 0xd500419f" ::: "memory");   /* msr pan, #1 */
        isb();
    }
    aarch64_fpu_init_cpu();   /* CPACR_EL1 is per-CPU: this one traps FP until told otherwise */
    arch_syscall_init_cpu();
    arch_irqc_init_cpu();
    pc->hw_id = (uint32_t)MPIDR_AFFINITY(READ_SYSREG(mpidr_el1));
    timer_init_cpu();
    kdebug("smp: CPU %u (MPIDR 0x%x) up", cpu, pc->hw_id);
    sched_start_cpu();
}
