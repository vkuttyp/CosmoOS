/*
 * hv_el2.c - The AArch64 EL2 backend for arch/hv.h
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend").
 *
 * The kernel runs at EL1, so a guest cannot be entered from here: this
 * file prepares a context page and asks EL2 to do the switch
 * (hv_el2_switch.S, installed through the stub the loader left). Every address
 * EL2 sees is physical, because its MMU is off.
 */

#include <kernel/bootinfo.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <arch/el2.h>
#include <arch/hv.h>
#include <arch/hv_backend.h>
#include <arch/irq.h>

#include <aarch64/hv_ctx.h>
#include <aarch64/hv_s2.h>
#include <aarch64/sysreg.h>

#define HV_VMIDS_MAX 16u

/* HCR_EL2 while a guest runs. */
#define HCR_VM   (1ull << 0)    /* stage 2 on */
#define HCR_FMO  (1ull << 3)    /* FIQ to EL2 */
#define HCR_IMO  (1ull << 4)    /* IRQ to EL2: a host interrupt exits the guest */
#define HCR_AMO  (1ull << 5)    /* SError to EL2 */
#define HCR_TWI  (1ull << 13)   /* WFI exits */
#define HCR_TWE  (1ull << 14)   /* WFE exits */
#define HCR_TID3 (1ull << 18)   /* ID register reads trap */
#define HCR_TSC  (1ull << 19)   /* SMC exits */
#define HCR_RW   (1ull << 31)   /* EL1 is AArch64 */

/* ESR_EL2.EC values this backend decodes. */
#define EC_WFX        0x01u
#define EC_HVC64      0x16u
#define EC_SMC64      0x17u
#define EC_SYSREG     0x18u
#define EC_IABT_LOWER 0x20u
#define EC_DABT_LOWER 0x24u

struct arch_hv_vm {
    paddr_t s2_root;
    uint16_t vmid;
    cpumask_t ran_on;
};

struct arch_hv_vcpu {
    struct arch_hv_vm *vm;
    struct hv_ctx *ctx;      /* the page EL2 reads and writes */
    paddr_t ctx_pa;
    int offered;
    bool irq_taken;
    unsigned unknown_exits;
    uint32_t pending_event;  /* a queued exception vector, ~0 for none */
};

static struct hv_caps g_caps = { .present = false, .name = "none" };
static uint64_t g_vtcr;
static uint32_t g_vmid_used = 1;
static spinlock_t g_vmid_lock = SPINLOCK_INIT("hv-vmid");
static bool g_el2_ready[CONFIG_MAX_CPUS];
static paddr_t g_el2_stack[CONFIG_MAX_CPUS];

static paddr_t kernel_va_to_pa(const void *va)
{
    const struct cosmoboot_info *info = bootinfo_get();
    return info->kernel_phys_base + ((uintptr_t)va - info->kernel_virt_base);
}

static bool el2_ready_here(void);

static int64_t el2_run(paddr_t ctx)
{
    register uint64_t x0 __asm__("x0") = HV_EL2_CALL_RUN;
    register uint64_t x1 __asm__("x1") = ctx;
    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1)
                     /* x18 is fixed by the ABI and cannot be clobbered here: the
                      * switch saves and restores the host's copy itself. */
                     : "memory", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                       "x14", "x15", "x16", "x17", "x30", "cc");
    return (int64_t)x0;
}

/* Every CPU installs the switch for itself: VBAR_EL2 and SP_EL2 are
 * per-CPU registers, and the stub is the only way to set either. The
 * stack comes first, because our own vectors do not implement that
 * call: after this the stub is no longer reachable on this CPU. */
static bool el2_ready_here(void)
{
    unsigned cpu = this_cpu()->cpu_id;
    if (g_el2_ready[cpu])
        return true;
    if (g_el2_stack[cpu] == 0) {
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (pg == NULL)
            return false;
        g_el2_stack[cpu] = page_to_phys(pg);
    }
    if (el2_set_stack(g_el2_stack[cpu] + PAGE_SIZE) != 0)
        return false;
    if (el2_set_vectors(kernel_va_to_pa(hv_el2_vectors)) != 0)
        return false;
    g_el2_ready[cpu] = true;
    return true;
}

static int vmid_alloc(uint16_t *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_vmid_lock);
    for (unsigned i = 1; i < HV_VMIDS_MAX; i++) {
        if (!(g_vmid_used & (1u << i))) {
            g_vmid_used |= 1u << i;
            spin_unlock_irqrestore(&g_vmid_lock, s);
            *out = (uint16_t)i;
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_vmid_lock, s);
    return -EBUSY;
}

static void vmid_free(uint16_t vmid)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_vmid_lock);
    g_vmid_used &= ~(1u << vmid);
    spin_unlock_irqrestore(&g_vmid_lock, s);
}

/* Drop everything the hardware cached for this VM, now, on every CPU.
 * `TLBI VMALLS12E1IS` is inner-shareable, so one execution reaches them
 * all -- including a CPU inside this guest at this moment -- but its
 * VMID comes from VTTBR_EL2, which only EL2 can write: hence the call.
 * This is the rule the IOMMU layer states as IOM6, in this
 * architecture's terms. Deferring it to the next entry would be wrong
 * twice over: a vCPU already running would keep its stale translations
 * while the caller frees the pages, and a VMID handed to the next VM
 * would carry the old one's entries. */
static bool invalidate_vm(struct arch_hv_vm *vm)
{
    uint64_t vttbr = (uint64_t)vm->s2_root | ((uint64_t)vm->vmid << 48);
    /* The descriptors were cleared with ordinary stores. A table walk is
     * a memory access like any other, so those stores must be visible to
     * every walker before the invalidation runs -- otherwise a walk on
     * another CPU can refill the entry the TLBI was meant to remove and
     * the guest keeps a translation to memory the caller is about to
     * free. DSB ISHST orders them against the inner-shareable domain,
     * which is where the walkers are; the TLBI's own DSB ISH inside the
     * switch then waits for the invalidation itself. */
    __asm__ volatile("dsb ishst" ::: "memory");
    arch_irq_state_t s = arch_irq_save();
    bool ok = el2_ready_here();
    if (ok) {
        register uint64_t x0 __asm__("x0") = HV_EL2_CALL_TLBI;
        register uint64_t x1 __asm__("x1") = vttbr;
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1) : "memory", "x2", "cc");
        ok = (int64_t)x0 == 0;
    }
    arch_irq_restore(s);
    if (!ok)
        kerror("hv-el2: cannot reach EL2 to invalidate VMID %u; nothing of this VM may be reused", vm->vmid);
    return ok;
}

static int el2_probe(struct hv_caps *out)
{
    if (!el2_available()) {
        *out = g_caps;
        return -ENOTSUP;
    }
    uint64_t mmfr0 = READ_SYSREG(id_aa64mmfr0_el1);
    unsigned parange = (unsigned)(mmfr0 & 0xF);
    static const unsigned pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    if (parange > 6)
        parange = 6;
    g_vtcr = hv_s2_vtcr(pa_bits[parange], parange);
    g_caps.present = true;
    g_caps.name = "el2";
    g_caps.max_asids = HV_VMIDS_MAX;
    g_caps.nested_paging = true;
    g_caps.real_mode_guest = true;   /* a guest starts with its MMU off, which is the reset state */
    g_caps.map_prot = true;
    g_caps.large_pages = true;
    g_caps.max_vcpus = 0;
    kinfo("hv: EL2 with stage-2 translation, %u-bit addresses, %u VMIDs", pa_bits[parange], HV_VMIDS_MAX - 1);
    *out = g_caps;
    return 0;
}

static int el2_vm_create(struct arch_hv_vm **out)
{
    if (!g_caps.present)
        return -ENOTSUP;
    struct arch_hv_vm *vm = kzalloc(sizeof(*vm));
    if (vm == NULL)
        return -ENOMEM;
    int rc = vmid_alloc(&vm->vmid);
    if (rc) {
        kfree(vm);
        return rc;
    }
    vm->s2_root = hv_s2_create();
    if (vm->s2_root == 0) {
        vmid_free(vm->vmid);
        kfree(vm);
        return -ENOMEM;
    }
    *out = vm;
    return 0;
}

static void el2_vm_destroy(struct arch_hv_vm *vm)
{
    if (vm == NULL)
        return;
    if (!invalidate_vm(vm)) {
        /* The hardware may still translate for this VMID: its tables
         * stay allocated and its VMID is never handed out again, because
         * either would give the next VM this one's memory. The cost is
         * one leaked VMID and a few pages for the life of the boot
         * (docs/kernel/iommu/invariants.md IOM6, in this architecture's
         * terms). */
        kerror("hv-el2: VMID %u retired unrevoked; its tables are kept", vm->vmid);
        kfree(vm);
        return;
    }
    hv_s2_destroy(vm->s2_root);
    vmid_free(vm->vmid);
    kfree(vm);
}

static int el2_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot)
{
    int rc = hv_s2_map(vm->s2_root, gpa, hpa, len, prot);
    if (rc == 0)
        __asm__ volatile("dsb ishst" ::: "memory");   /* visible to the walkers before a guest runs */
    return rc;
}

static int el2_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len)
{
    int rc = hv_s2_unmap(vm->s2_root, gpa, len);
    if (rc == 0 && !invalidate_vm(vm))
        return -EIO;   /* the entries are gone, the caches are not: do not reuse the pages */
    return rc;
}

static bool el2_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa)
{
    return hv_s2_query(vm->s2_root, gpa, hpa);
}

/* The architectural reset state of an EL1 guest: MMU off, interrupts
 * masked, PC where the owner puts it. */
static void ctx_reset(struct arch_hv_vcpu *v)
{
    struct hv_ctx *c = v->ctx;
    memset(c, 0, sizeof(*c));
    c->guest_pstate = 0x3C5;          /* EL1h, DAIF masked */
    c->guest.sctlr = 0x00C50838ull;   /* the reset value: MMU, caches and alignment off */
    c->guest.cpacr = 0;
    c->vttbr = (uint64_t)v->vm->s2_root | ((uint64_t)v->vm->vmid << 48);
    c->vtcr = g_vtcr;
    c->hcr = HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO | HCR_TWI | HCR_TWE | HCR_TID3 | HCR_TSC;
    v->offered = -1;
    v->pending_event = ~0u;
}

static int el2_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out)
{
    if (!g_caps.present)
        return -ENOTSUP;
    struct arch_hv_vcpu *v = kzalloc(sizeof(*v));
    if (v == NULL)
        return -ENOMEM;
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (pg == NULL) {
        kfree(v);
        return -ENOMEM;
    }
    v->vm = vm;
    v->ctx = page_to_virt(pg);
    v->ctx_pa = page_to_phys(pg);
    ctx_reset(v);
    *out = v;
    return 0;
}

static void el2_vcpu_destroy(struct arch_hv_vcpu *v)
{
    if (v == NULL)
        return;
    pmm_free_page(phys_to_page(v->ctx_pa));
    kfree(v);
}

static void el2_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *o)
{
    const struct hv_ctx *c = v->ctx;
    memset(o, 0, sizeof(*o));
    for (unsigned i = 0; i < 31; i++)
        o->x[i] = c->guest_x[i];
    o->sp_el1 = c->guest_sp_el1;
    o->sp_el0 = c->guest.sp_el0;
    o->pc = c->guest_pc;
    o->pstate = c->guest_pstate;
    o->sctlr_el1 = c->guest.sctlr;
    o->ttbr0_el1 = c->guest.ttbr0;
    o->ttbr1_el1 = c->guest.ttbr1;
    o->tcr_el1 = c->guest.tcr;
    o->mair_el1 = c->guest.mair;
    o->amair_el1 = c->guest.amair;
    o->vbar_el1 = c->guest.vbar;
    o->esr_el1 = c->guest.esr;
    o->far_el1 = c->guest.far;
    o->elr_el1 = c->guest.elr;
    o->spsr_el1 = c->guest.spsr;
    o->tpidr_el0 = c->guest.tpidr_el0;
    o->tpidrro_el0 = c->guest.tpidrro_el0;
    o->tpidr_el1 = c->guest.tpidr_el1;
    o->contextidr_el1 = c->guest.contextidr;
    o->cpacr_el1 = c->guest.cpacr;
    o->par_el1 = c->guest.par;
    o->mdscr_el1 = c->guest.mdscr;
    o->pending_irq = v->offered < 0 ? ~0ull : (uint64_t)v->offered;
}

static int el2_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *i)
{
    /* PSTATE must name an AArch64 EL1 or EL0 mode, and the guest may not
     * ask to run at EL2. */
    uint64_t mode = i->pstate & 0x1F;
    if (mode != 0x0 && mode != 0x4 && mode != 0x5)
        return -EINVAL;
    if (i->pstate & (1ull << 4))   /* M[4]: AArch32 */
        return -EINVAL;
    struct hv_ctx *c = v->ctx;
    for (unsigned k = 0; k < 31; k++)
        c->guest_x[k] = i->x[k];
    c->guest_sp_el1 = i->sp_el1;
    c->guest.sp_el0 = i->sp_el0;
    c->guest_pc = i->pc;
    c->guest_pstate = i->pstate;
    c->guest.sctlr = i->sctlr_el1;
    c->guest.ttbr0 = i->ttbr0_el1;
    c->guest.ttbr1 = i->ttbr1_el1;
    c->guest.tcr = i->tcr_el1;
    c->guest.mair = i->mair_el1;
    c->guest.amair = i->amair_el1;
    c->guest.vbar = i->vbar_el1;
    c->guest.esr = i->esr_el1;
    c->guest.far = i->far_el1;
    c->guest.elr = i->elr_el1;
    c->guest.spsr = i->spsr_el1;
    c->guest.tpidr_el0 = i->tpidr_el0;
    c->guest.tpidrro_el0 = i->tpidrro_el0;
    c->guest.tpidr_el1 = i->tpidr_el1;
    c->guest.contextidr = i->contextidr_el1;
    c->guest.cpacr = i->cpacr_el1;
    c->guest.par = i->par_el1;
    c->guest.mdscr = i->mdscr_el1;
    return 0;
}

static uint64_t el2_vcpu_guest_efer(struct arch_hv_vcpu *v)
{
    (void)v;
    return 0;   /* no such register here */
}

static int el2_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer)
{
    (void)v;
    return efer == 0 ? 0 : -EINVAL;
}

static int el2_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value)
{
    (void)v;
    (void)index;
    (void)write;
    (void)value;
    return -ENOENT;   /* system registers are the EL1 state, not an MSR space */
}

static bool el2_vcpu_xstate_enabled(struct arch_hv_vcpu *v)
{
    return (v->ctx->guest.cpacr & (3ull << 20)) != 0;   /* CPACR_EL1.FPEN */
}

static void el2_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size)
{
    /* The IN completion of a port read; there is no port space here, but
     * the interface's shape is shared. x0 is the first result register. */
    uint64_t mask = size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    v->ctx->guest_x[0] = value & mask;
}

static uint64_t el2_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index)
{
    return index < 31 ? v->ctx->guest_x[index] : 0;
}

static void el2_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value)
{
    if (index < 31)
        v->ctx->guest_x[index] = value;
}

static void el2_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes)
{
    v->ctx->guest_pc += bytes;
}

static void el2_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t pc)
{
    v->ctx->guest_pc = pc;
}

static uint64_t el2_vcpu_rip(struct arch_hv_vcpu *v)
{
    return v->ctx->guest_pc;
}

static void el2_vcpu_set_irq(struct arch_hv_vcpu *v, int vector)
{
    v->offered = vector;   /* recorded; delivery needs the GIC list registers */
}

static bool el2_vcpu_irq_taken(struct arch_hv_vcpu *v)
{
    return v->irq_taken;
}

static void el2_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error)
{
    (void)has_error;
    (void)error;
    v->pending_event = vector;   /* delivered as a guest exception at the next entry */
}

static int decode_exit(struct arch_hv_vcpu *v, struct hv_exit *out)
{
    const struct hv_ctx *c = v->ctx;
    if (c->exit_kind != 0) {
        /* IRQ, FIQ or SError: the host takes it when interrupts are
         * restored below the call. */
        out->kind = HV_EXIT_INTR;
        return 0;
    }
    uint32_t ec = (uint32_t)(c->exit_esr >> 26) & 0x3F;
    uint32_t il = (c->exit_esr & (1u << 25)) ? 4u : 2u;
    switch (ec) {
    case EC_HVC64:
        out->kind = HV_EXIT_HYPERCALL;   /* HVC does not advance PC itself */
        return 0;
    case EC_SMC64:
        v->ctx->guest_pc += il;
        out->kind = HV_EXIT_HYPERCALL;
        return 0;
    case EC_WFX:
        v->ctx->guest_pc += il;
        out->kind = HV_EXIT_WFI;
        return 0;
    case EC_SYSREG:
        out->kind = HV_EXIT_SYSREG;
        out->sysreg.iss = (uint32_t)(c->exit_esr & 0x1FFFFFFu);
        out->sysreg.reg = (uint8_t)((c->exit_esr >> 5) & 0x1F);
        out->sysreg.write = (c->exit_esr & 1) == 0;   /* ISS bit 0: 0 = write */
        return 0;
    case EC_DABT_LOWER:
    case EC_IABT_LOWER:
        out->kind = HV_EXIT_MMIO;
        /* HPFAR_EL2 holds the faulting intermediate physical address
         * shifted right by 8, with the page offset from FAR_EL2. */
        out->mmio.gpa = ((c->exit_hpfar & ~0xFull) << 8) | (c->exit_far & (PAGE_SIZE - 1));
        out->mmio.write = ec == EC_DABT_LOWER && (c->exit_esr & (1u << 6)) != 0;
        return 0;
    default:
        out->kind = HV_EXIT_FAIL;
        out->fail.code = ec;
        out->fail.info1 = c->exit_esr;
        out->fail.info2 = c->exit_far;
        if (v->unknown_exits++ == 0)
            kwarn("hv-el2: vmid %u: EC 0x%x ESR 0x%llx FAR 0x%llx PC 0x%llx", v->vm->vmid, ec,
                  (unsigned long long)c->exit_esr, (unsigned long long)c->exit_far,
                  (unsigned long long)c->guest_pc);
        return 0;
    }
}

static int el2_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out)
{
    arch_irq_state_t s = arch_irq_save();
    if (!el2_ready_here()) {
        arch_irq_restore(s);
        return -ENOMEM;
    }
    __atomic_or_fetch(&v->vm->ran_on, CPUMASK_OF(this_cpu()->cpu_id), __ATOMIC_RELEASE);
    v->irq_taken = false;
    int64_t rc = el2_run(v->ctx_pa);
    arch_irq_restore(s);
    if (rc != 0) {
        out->kind = HV_EXIT_FAIL;
        out->fail.code = (uint64_t)rc;
        out->fail.info1 = 0;
        out->fail.info2 = 0;
        kerror("hv-el2: the switch refused the call (%lld)", (long long)rc);
        return 0;
    }
    return decode_exit(v, out);
}

const struct hv_backend el2_backend = {
    .probe = el2_probe,
    .vm_create = el2_vm_create,
    .vm_destroy = el2_vm_destroy,
    .vm_map = el2_vm_map,
    .vm_unmap = el2_vm_unmap,
    .vm_query = el2_vm_query,
    .vcpu_create = el2_vcpu_create,
    .vcpu_destroy = el2_vcpu_destroy,
    .vcpu_get_state = el2_vcpu_get_state,
    .vcpu_set_state = el2_vcpu_set_state,
    .vcpu_run = el2_vcpu_run,
    .vcpu_set_irq = el2_vcpu_set_irq,
    .vcpu_irq_taken = el2_vcpu_irq_taken,
    .vcpu_inject_exception = el2_vcpu_inject_exception,
    .vcpu_advance_rip = el2_vcpu_advance_rip,
    .vcpu_set_rip = el2_vcpu_set_rip,
    .vcpu_rip = el2_vcpu_rip,
    .vcpu_guest_efer = el2_vcpu_guest_efer,
    .vcpu_set_guest_efer = el2_vcpu_set_guest_efer,
    .vcpu_msr = el2_vcpu_msr,
    .vcpu_xstate_enabled = el2_vcpu_xstate_enabled,
    .vcpu_write_rax = el2_vcpu_write_rax,
    .vcpu_read_gpr = el2_vcpu_read_gpr,
    .vcpu_write_gpr = el2_vcpu_write_gpr,
};
