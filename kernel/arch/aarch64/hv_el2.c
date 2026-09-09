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
#include <kernel/irq.h>
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

#include <aarch64/fpu.h>
#include <aarch64/hv_ctx.h>
#include <aarch64/hv_el2.h>
#include <aarch64/irqc.h>
#include <aarch64/platform.h>
#include <aarch64/vgic.h>
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

/* CNTV_CTL_EL0 */
#define CNTV_CTL_ENABLE  (1ull << 0)
#define CNTV_CTL_IMASK   (1ull << 1)
#define CNTV_CTL_ISTATUS (1ull << 2)

/*
 * A list register (ICH_LR<n>_EL2): the state in the top two bits, the
 * group and priority the guest's own mask is compared against, and the
 * interrupt number the guest will see in ICC_IAR1_EL1.
 *
 * State matters more than it looks. Invalid means the register is free;
 * Pending means placed and not yet taken; **Active means the guest has
 * acknowledged it** and has not completed it, which is delivery as much
 * as Invalid is. A hypervisor that waits for Invalid re-injects
 * everything a guest was still handling when it exited.
 */
#define LR_STATE_MASK    (3ull << 62)
#define LR_STATE_INVALID (0ull << 62)
#define LR_STATE_PENDING (1ull << 62)
#define LR_GROUP1        (1ull << 60)
#define LR_PRIORITY(p)   ((uint64_t)(p) << 48)
#define ICH_HCR_EN       (1ull << 0)

/* Below the guest's own PMR of 0xF0, so an interrupt it has not masked
 * is delivered; the same number the host's driver uses for its own. */
#define VGIC_PRIORITY 0xA0u

static uint64_t lr_state(uint64_t lr)
{
    return lr & LR_STATE_MASK;
}


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
    uint64_t cntvoff;        /* CNTVOFF_EL2 for every vCPU: the VM's clock starts here */
};

struct arch_hv_vcpu {
    struct arch_hv_vm *vm;
    struct hv_ctx *ctx;      /* the page EL2 reads and writes */
    paddr_t ctx_pa;
    int offered;
    int lr_vector;           /* the INTID list register 0 holds for us, -1 if none */
    bool lr_reported;        /* its delivery has already been told to the owner */
    bool timer_reported;     /* this expiry has already been offered */
    uint64_t irq_delivered;  /* interrupts the guest took */
    uint64_t irq_deferred;   /* entries where one was offered and no list register was free */
    unsigned unknown_exits;
    uint32_t pending_event;  /* a queued exception vector, ~0 for none */
    struct aarch64_fpu_area fpu;   /* the guest's vector registers (arch/fpu.h, guest rule) */
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

/* How many virtual interrupts this implementation can hold at once, and
 * whether it can hold any: ICH_VTR_EL2 read through the switch. Set once
 * at probe on the boot CPU; the count is a property of the
 * implementation, not of a CPU. */
static unsigned g_vgic_lrs;

/* Ask EL2 to open the virtual interface and say how big it is. Only
 * valid on a machine with a GICv3 CPU interface; the registers do not
 * exist otherwise. */
static int64_t el2_vgic_query(void)
{
    register uint64_t x0 __asm__("x0") = HV_EL2_CALL_VGIC;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory", "x1", "x2", "cc");
    return (int64_t)x0;
}

unsigned aarch64_vgic_lr_count(void)
{
    return g_vgic_lrs;
}

bool aarch64_vgic_available(void)
{
    return g_vgic_lrs > 0;
}

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

/*
 * The virtual timer's PPI is the hypervisor's. A guest's CNTV expiry has
 * to become a physical interrupt for `HCR_EL2.IMO` to turn it into an
 * exit, and a PPI that is not enabled in the redistributor raises
 * nothing -- so the line is bound once here and enabled on each CPU as
 * that CPU's switch is installed. The handler has nothing to do: by the
 * time the host is back at EL1 the switch has disarmed the timer, the
 * level source is down, and the decision to inject was already made from
 * the CNTV_CTL the switch saved before disarming it. It exists so that
 * a physical interrupt that does still arrive is acknowledged rather
 * than left to storm.
 */
static unsigned g_vtimer_intid;
static bool g_vtimer_bound;

static void el2_vtimer_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
}

static void el2_vtimer_bind(void)
{
    g_vtimer_intid = aarch64_timer_virt_intid();
    int rc = irq_request(g_vtimer_intid, el2_vtimer_irq, NULL, "hv-vtimer", IRQ_TRIGGER_LEVEL, IRQ_CPU_ANY);
    if (rc == 0)
        rc = irq_enable(g_vtimer_intid);
    if (rc) {
        kwarn("hv: cannot take the virtual timer PPI %u (%d); guest timers will not fire", g_vtimer_intid, rc);
        return;
    }
    g_vtimer_bound = true;
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
    /* PPI enables are banked: this CPU's copy, so a guest's timer expiry
     * here is an interrupt and therefore an exit. */
    if (g_vtimer_bound)
        gic_enable_local(g_vtimer_intid);
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

    /*
     * Interrupts for guests. Only a GICv3's virtual interface can raise
     * one, and its registers are EL2-only, so both the enabling and the
     * capability come back from the switch -- which means the switch has
     * to own EL2 on this CPU before the question can be asked.
     */
    if (aarch64_irqc_is_v3() && el2_ready_here()) {
        int64_t vtr = el2_vgic_query();
        if (vtr >= 0) {
            g_vgic_lrs = (unsigned)(vtr & 0x1F) + 1;
            g_caps.inject_irq = true;
            /* ICH_VTR_EL2.PRIbits: with more than five priority bits an
             * implementation has more than one active-priority register
             * per group, and the switch moves only the first of each. */
            unsigned pribits = (unsigned)((vtr >> 29) & 0x7) + 1;
            if (pribits > 5)
                kwarn("hv: %u priority bits; only ICH_AP{0,1}R0_EL2 are saved per vCPU", pribits);
        }
    }
    el2_vtimer_bind();
    kinfo("hv: EL2 with stage-2 translation, %u-bit addresses, %u VMIDs, guest interrupts %s",
          pa_bits[parange], HV_VMIDS_MAX - 1,
          g_caps.inject_irq ? "through the virtual GIC" : "unavailable (no GICv3 virtual interface)");
    if (g_caps.inject_irq)
        kinfo("hv: the virtual GIC has %u list register(s)", g_vgic_lrs);
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
    /* One offset per VM, taken once: a vCPU created later than its
     * siblings must see the same clock they do, so the value is the
     * VM's and not the counter's at each vCPU's creation. */
    vm->cntvoff = READ_SYSREG(cntpct_el0);
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
    /*
     * The guest's interrupt state, on a machine that has one. The
     * interface starts disabled and every list register empty: nothing
     * is pending until something is injected, and `ICH_VMCR_EL2` zero
     * is a guest whose own PMR masks everything -- which is what a
     * guest that has not configured its CPU interface should see.
     */
    /* The guest's timer starts disarmed and its clock at the VM's zero. */
    c->cntv_ctl = 0;
    c->cntv_cval = 0;
    c->cntvoff = v->vm->cntvoff;
    c->vgic_on = g_caps.inject_irq ? 1 : 0;
    v->lr_vector = -1;
    v->lr_reported = false;
    v->timer_reported = false;
    c->vgic_hcr = c->vgic_on ? ICH_HCR_EN : 0;
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
    /* What one list register cost: `deferred` counts entries where a
     * *different* interrupt had to wait because the register was still
     * holding one. A second is worth writing EL2 assembly for only if
     * this is not zero in practice. */
    if (v->irq_delivered || v->irq_deferred)
        kdebug("hv-el2: vmid %u: %llu interrupt(s) delivered, %llu deferred for want of a list register",
               v->vm->vmid, (unsigned long long)v->irq_delivered, (unsigned long long)v->irq_deferred);
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

bool el2_vcpu_vgic_state(struct arch_hv_vcpu *v, uint64_t *lr0, uint64_t *elrsr)
{
    if (!v->ctx->vgic_on)
        return false;
    *lr0 = v->ctx->vgic_lr0;
    *elrsr = v->ctx->vgic_elrsr;
    return true;
}

bool el2_vcpu_timer_state(struct arch_hv_vcpu *v, uint64_t *ctl, uint64_t *cntvoff)
{
    *ctl = v->ctx->cntv_ctl;
    *cntvoff = v->ctx->cntvoff;
    return true;
}

/*
 * Whether the guest's timer expired during the last run. Read from the
 * CNTV_CTL the switch saved *before* disarming: ENABLE with IMASK clear
 * and ISTATUS set is the timer's own statement that its condition was
 * met and it was allowed to say so. Independent of which exit occurred
 * and of whether the host ever saw the physical interrupt.
 *
 * Reported once per expiry: the guest's handler masks or re-arms, which
 * clears the condition, and until it does the same expiry would read
 * true on every exit and be injected again.
 */

static bool el2_vcpu_timer_expired(struct arch_hv_vcpu *v)
{
    uint64_t ctl = v->ctx->cntv_ctl;
    bool firing = (ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK | CNTV_CTL_ISTATUS)) ==
                  (CNTV_CTL_ENABLE | CNTV_CTL_ISTATUS);
    if (!firing) {
        v->timer_reported = false;
        return false;
    }
    if (v->timer_reported)
        return false;
    v->timer_reported = true;
    return true;
}

/*
 * The guest's compare, in the host's counter. CNTV compares CNTVCT --
 * CNTPCT minus the VM's offset -- against CVAL, so the host-counter
 * value at which it fires is CVAL plus the offset. Only while the timer
 * is armed, unmasked and has not fired: an expired timer is an
 * interrupt to deliver, not a deadline to wait for.
 */
static bool el2_vcpu_timer_deadline(struct arch_hv_vcpu *v, uint64_t *host_ticks)
{
    uint64_t ctl = v->ctx->cntv_ctl;
    if ((ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK | CNTV_CTL_ISTATUS)) != CNTV_CTL_ENABLE)
        return false;
    *host_ticks = v->ctx->cntv_cval + v->ctx->cntvoff;
    return true;
}

unsigned el2_guest_timer_intid(void)
{
    return g_vtimer_bound ? g_vtimer_intid : 0;
}

static void el2_vcpu_set_irq(struct arch_hv_vcpu *v, int vector)
{
    v->offered = vector;
    if (!v->ctx->vgic_on || vector < 0)
        return;
    /*
     * One list register, so one interrupt at a time. If the last one is
     * still in it -- Pending because the guest has it masked, Active
     * because the guest is in its handler -- overwriting would lose a
     * state the guest is about to act on, and a guest that then
     * completed an interrupt nobody had given it would take a spurious
     * EOI. Whatever is in there is the interrupt whose fate the exit
     * path reports; this offer waits its turn.
     */
    if (lr_state(v->ctx->vgic_lr0) != LR_STATE_INVALID) {
        /* A *different* interrupt had to wait: the number that says
         * whether one register is enough. Offering the resident one
         * again is the ordinary case and costs nothing. */
        if (v->lr_vector != vector)
            v->irq_deferred++;
        return;
    }
    v->ctx->vgic_lr0 = LR_STATE_PENDING | LR_GROUP1 | LR_PRIORITY(VGIC_PRIORITY) | (uint32_t)vector;
    v->lr_vector = vector;
    v->lr_reported = false;
}

static int el2_vcpu_irq_delivered(struct arch_hv_vcpu *v)
{
    if (!v->ctx->vgic_on || v->lr_vector < 0)
        return -1;
    /*
     * The list register's own occupant is what the guest can have
     * taken, and it is not always what this entry offered: one placed
     * while the guest had interrupts masked sits there across every
     * entry until the guest unmasks, and a lower-numbered vector may be
     * the current offer all the while.
     *
     * Pending is "placed, not yet taken". Active is taken and not yet
     * completed -- delivery, and the guest is in its handler. Invalid is
     * taken and completed, and the register is free again. Reported
     * once, because Active can persist across several runs and the
     * owner must clear it exactly one time.
     */
    uint64_t st = lr_state(v->ctx->vgic_lr0);
    if (st == LR_STATE_PENDING)
        return -1;
    int taken = v->lr_vector;
    if (st == LR_STATE_INVALID)
        v->lr_vector = -1;
    if (v->lr_reported)
        return -1;
    v->lr_reported = true;
    v->irq_delivered++;
    return taken;
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
    /* Guest rule (arch/fpu.h): the owner thread's vector registers are
     * saved, the guest's are loaded, and afterwards the guest's are
     * captured and the owner's put back. A kernel-thread owner holds no
     * state and gets zeros, so no guest register stays live in the
     * kernel. Interrupts are off from here to the restore, so nothing
     * can switch threads in between. */
    /*
     * An expired timer would storm. The switch restores the guest's
     * CNTV_CTL on entry, and if its condition already holds the PPI is
     * asserted before the guest executes an instruction -- IMO makes
     * that an exit, and the next entry does it again, forever, with the
     * guest's handler never reached. So while an expiry is queued the
     * PPI is disabled in this CPU's redistributor: the guest's own
     * CNTV_CTL is untouched and reads what it wrote, the *virtual*
     * interrupt in the list register is unaffected, and the physical one
     * cannot exit. It is enabled again once the guest's handler has
     * masked or re-armed, which is when the saved CTL stops saying
     * ISTATUS. Banked per CPU, so this is the running CPU's copy.
     */
    if (g_vtimer_bound) {
        uint64_t ctl = v->ctx->cntv_ctl;
        bool firing = (ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK | CNTV_CTL_ISTATUS)) ==
                      (CNTV_CTL_ENABLE | CNTV_CTL_ISTATUS);
        if (firing)
            gic_disable_local(g_vtimer_intid);
        else
            gic_enable_local(g_vtimer_intid);
    }
    bool owner = aarch64_fpu_save_current();
    aarch64_fpu_area_restore(&v->fpu);
    int64_t rc = el2_run(v->ctx_pa);
    aarch64_fpu_area_save(&v->fpu);
    if (!owner || !aarch64_fpu_restore_current()) {
        static const struct aarch64_fpu_area zero;
        aarch64_fpu_area_restore(&zero);
    }
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
    .vcpu_irq_delivered = el2_vcpu_irq_delivered,
    .vcpu_timer_expired = el2_vcpu_timer_expired,
    .vcpu_timer_deadline = el2_vcpu_timer_deadline,
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
