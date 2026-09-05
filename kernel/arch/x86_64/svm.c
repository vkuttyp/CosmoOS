/*
 * svm.c - AMD-V backend for arch/hv.h
 * (docs/kernel-services/virtualization/design.md, "The SVM backend").
 *
 * Per CPU: SVM enabled on first use (EFER.SVME, VM_HSAVE_PA), a host
 * save area and a host VMCB for VMSAVE/VMLOAD. Per VM: a nested page
 * table and an ASID. Per vCPU: one VMCB and the GPR spill block. Every
 * port and MSR is intercepted (all-ones permission maps); the TLB is
 * flushed on every entry. Nothing here is visible above arch/hv.h.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <arch/hv.h>
#include <arch/irq.h>

#include <x86/cpu.h>
#include <x86/svm.h>

#define SVM_ASIDS_MAX 64u
#define IOPM_BYTES    (12u * 1024u)   /* 65536 ports, one bit each (+ slack the hardware reads) */
#define MSRPM_BYTES   (8u * 1024u)

#ifndef CR0_ET
#define CR0_ET (1ull << 4)
#endif
#ifndef CR0_NW
#define CR0_NW (1ull << 29)
#endif
#ifndef CR0_CD
#define CR0_CD (1ull << 30)
#endif
#define EFER_KNOWN (EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE | EFER_SVME | (1ull << 13) | (1ull << 14) | (1ull << 15))
#define SEG_ATTR_L  (1u << 13)
#define SEG_ATTR_DB (1u << 14)

struct arch_hv_vm {
    paddr_t ncr3;
    uint32_t asid;
};

struct arch_hv_vcpu {
    struct vmcb *vmcb;
    paddr_t vmcb_pa;
    struct svm_gprs gprs;
    struct arch_hv_vm *vm;
    uint64_t guest_efer;      /* the guest's view: never SVME */
    int offered;
    unsigned unknown_exits;
};

struct svm_cpu {
    bool enabled;
    paddr_t hsave_pa;
    paddr_t host_vmcb_pa;
};

static struct hv_caps g_caps = { .present = false, .name = "none" };
static paddr_t g_iopm_pa, g_msrpm_pa;
static uint64_t g_asid_used;               /* bit i: ASID i in use (bit 0 always set) */
static spinlock_t g_asid_lock = SPINLOCK_INIT("svm-asid");
static struct svm_cpu g_cpus[CONFIG_MAX_CPUS];

/* --- probe and per-CPU enable --- */

static paddr_t alloc_pages_filled(unsigned order, size_t bytes, int fill)
{
    struct page *pg = pmm_alloc_pages(order, PMM_FLAGS_ZERO);
    if (pg == NULL)
        return 0;
    memset(phys_to_virt(page_to_phys(pg)), fill, bytes);
    return page_to_phys(pg);
}

int arch_hv_probe(struct hv_caps *out)
{
    struct cpuid_regs r;
    cpuid(0x80000000u, 0, &r);
    bool svm = false;
    if (r.eax >= CPUID_SVM_FEATURES) {
        cpuid(CPUID_EXT_FEATURES, 0, &r);
        svm = (r.ecx & CPUID_EXT_ECX_SVM) != 0;
    }
    if (!svm) {
        *out = g_caps;
        return -ENOTSUP;
    }
    if (rdmsr(MSR_VM_CR) & VM_CR_SVMDIS) {
        kwarn("svm: disabled by firmware (VM_CR.SVMDIS)");
        *out = g_caps;
        return -ENOTSUP;
    }
    cpuid(CPUID_SVM_FEATURES, 0, &r);
    unsigned nasid = r.ebx;
    bool np = (r.edx & CPUID_SVM_EDX_NP) != 0;
    if (!np || nasid < 2) {
        kwarn("svm: present but without nested paging (%u ASIDs); not used", nasid);
        *out = g_caps;
        return -ENOTSUP;
    }
    g_iopm_pa = alloc_pages_filled(2, IOPM_BYTES, 0xFF);    /* 16 KiB, 12 used */
    g_msrpm_pa = alloc_pages_filled(1, MSRPM_BYTES, 0xFF);  /* 8 KiB */
    if (g_iopm_pa == 0 || g_msrpm_pa == 0)
        return -ENOMEM;
    g_asid_used = 1;   /* ASID 0 is the host's */
    g_caps.present = true;
    g_caps.name = "svm";
    g_caps.max_asids = nasid < SVM_ASIDS_MAX ? nasid : SVM_ASIDS_MAX;
    g_caps.nested_paging = true;
    kinfo("svm: AMD-V with nested paging, %u ASIDs usable%s%s", g_caps.max_asids - 1,
          (r.edx & CPUID_SVM_EDX_NRIP) ? ", nrip" : "", (r.edx & CPUID_SVM_EDX_DECODE) ? ", decode-assists" : "");
    *out = g_caps;
    return 0;
}

/* Interrupts disabled. Enables SVM on this CPU the first time it runs a guest. */
static struct svm_cpu *enable_this_cpu(void)
{
    struct svm_cpu *c = &g_cpus[this_cpu()->cpu_id];
    if (c->enabled)
        return c;
    struct page *hsave = pmm_alloc_page(PMM_FLAGS_ZERO);
    struct page *hvmcb = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (hsave == NULL || hvmcb == NULL) {
        if (hsave)
            pmm_free_page(hsave);
        if (hvmcb)
            pmm_free_page(hvmcb);
        return NULL;
    }
    c->hsave_pa = page_to_phys(hsave);
    c->host_vmcb_pa = page_to_phys(hvmcb);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
    wrmsr(MSR_VM_HSAVE_PA, c->hsave_pa);
    c->enabled = true;
    return c;
}

/* --- VM: nested table + ASID --- */

static int asid_alloc(uint32_t *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_asid_lock);
    for (uint32_t i = 1; i < g_caps.max_asids; i++) {
        if (!(g_asid_used & (1ull << i))) {
            g_asid_used |= 1ull << i;
            spin_unlock_irqrestore(&g_asid_lock, s);
            *out = i;
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_asid_lock, s);
    return -ENOSPC;
}

static void asid_free(uint32_t asid)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_asid_lock);
    g_asid_used &= ~(1ull << asid);
    spin_unlock_irqrestore(&g_asid_lock, s);
}

int arch_hv_vm_create(struct arch_hv_vm **out)
{
    if (!g_caps.present)
        return -ENOTSUP;
    struct arch_hv_vm *vm = kzalloc(sizeof(*vm));
    if (vm == NULL)
        return -ENOMEM;
    int rc = asid_alloc(&vm->asid);
    if (rc) {
        kfree(vm);
        return rc;
    }
    vm->ncr3 = npt_create();
    if (vm->ncr3 == 0) {
        asid_free(vm->asid);
        kfree(vm);
        return -ENOMEM;
    }
    *out = vm;
    return 0;
}

void arch_hv_vm_destroy(struct arch_hv_vm *vm)
{
    if (vm == NULL)
        return;
    npt_destroy(vm->ncr3);
    asid_free(vm->asid);
    kfree(vm);
}

int arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len)
{
    return npt_map(vm->ncr3, gpa, hpa, len);
}

int arch_hv_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len)
{
    return npt_unmap(vm->ncr3, gpa, len);
}

bool arch_hv_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa)
{
    return npt_query(vm->ncr3, gpa, hpa);
}

/* --- vCPU: the VMCB --- */

static void seg_set(struct svm_seg *s, uint16_t sel, uint16_t attrib, uint32_t limit, uint64_t base)
{
    s->selector = sel;
    s->attrib = attrib;
    s->limit = limit;
    s->base = base;
}

static void vmcb_reset(struct arch_hv_vcpu *v)
{
    struct vmcb *b = v->vmcb;
    memset(b, 0, sizeof(*b));
    struct vmcb_control *c = &b->control;
    c->intercept_misc1 = SVM_INTERCEPT_INTR | SVM_INTERCEPT_NMI | SVM_INTERCEPT_SMI | SVM_INTERCEPT_INIT |
                         SVM_INTERCEPT_CPUID | SVM_INTERCEPT_INVD | SVM_INTERCEPT_HLT | SVM_INTERCEPT_INVLPGA |
                         SVM_INTERCEPT_IOIO | SVM_INTERCEPT_MSR | SVM_INTERCEPT_SHUTDOWN;
    c->intercept_misc2 = SVM_INTERCEPT_VMRUN | SVM_INTERCEPT_VMMCALL | SVM_INTERCEPT_VMLOAD | SVM_INTERCEPT_VMSAVE |
                         SVM_INTERCEPT_STGI | SVM_INTERCEPT_CLGI | SVM_INTERCEPT_SKINIT | SVM_INTERCEPT_MONITOR |
                         SVM_INTERCEPT_MWAIT | SVM_INTERCEPT_MWAIT_ARM;
    c->iopm_base_pa = g_iopm_pa;
    c->msrpm_base_pa = g_msrpm_pa;
    c->asid = v->vm->asid;
    c->tlb_control = SVM_TLB_FLUSH_ALL;
    c->v_intr_prio = 0x1F;           /* V_IGN_TPR | priority 15 */
    c->v_intr_masking = 1;           /* host IF (set at VMRUN) governs physical interrupts, not the guest's */
    c->np_enable = 1;
    c->n_cr3 = v->vm->ncr3;

    struct vmcb_save *s = &b->save;
    seg_set(&s->cs, 0, SVM_ATTR_CODE_RM, 0xFFFF, 0);
    seg_set(&s->ds, 0, SVM_ATTR_DATA_RM, 0xFFFF, 0);
    seg_set(&s->es, 0, SVM_ATTR_DATA_RM, 0xFFFF, 0);
    seg_set(&s->fs, 0, SVM_ATTR_DATA_RM, 0xFFFF, 0);
    seg_set(&s->gs, 0, SVM_ATTR_DATA_RM, 0xFFFF, 0);
    seg_set(&s->ss, 0, SVM_ATTR_DATA_RM, 0xFFFF, 0);
    seg_set(&s->gdtr, 0, 0, 0xFFFF, 0);
    seg_set(&s->idtr, 0, 0, 0xFFFF, 0);
    seg_set(&s->ldtr, 0, SVM_ATTR_LDT, 0xFFFF, 0);
    seg_set(&s->tr, 0, SVM_ATTR_TSS_BUSY, 0xFFFF, 0);
    s->cr0 = CR0_ET | CR0_NW | CR0_CD;
    s->efer = EFER_SVME;
    s->rflags = 0x2;
    s->rip = 0;
    s->dr6 = 0xFFFF0FF0ull;
    s->dr7 = 0x400;
    s->g_pat = PAT_DEFAULT;
    v->guest_efer = 0;
    memset(&v->gprs, 0, sizeof(v->gprs));
    v->offered = -1;
}

int arch_hv_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out)
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
    v->vmcb_pa = page_to_phys(pg);
    v->vmcb = phys_to_virt(v->vmcb_pa);
    v->vm = vm;
    vmcb_reset(v);
    *out = v;
    return 0;
}

void arch_hv_vcpu_destroy(struct arch_hv_vcpu *v)
{
    if (v == NULL)
        return;
    pmm_free_page(phys_to_page(v->vmcb_pa));
    kfree(v);
}

static void seg_out(struct cosmo_vcpu_seg *o, const struct svm_seg *s)
{
    o->selector = s->selector;
    o->attrib = s->attrib;
    o->limit = s->limit;
    o->base = s->base;
}

static void seg_in(struct svm_seg *s, const struct cosmo_vcpu_seg *i)
{
    s->selector = i->selector;
    s->attrib = i->attrib;
    s->limit = i->limit;
    s->base = i->base;
}

void arch_hv_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *o)
{
    const struct vmcb_save *s = &v->vmcb->save;
    const struct svm_gprs *g = &v->gprs;
    memset(o, 0, sizeof(*o));
    o->rax = s->rax; o->rbx = g->rbx; o->rcx = g->rcx; o->rdx = g->rdx;
    o->rsi = g->rsi; o->rdi = g->rdi; o->rbp = g->rbp; o->rsp = s->rsp;
    o->r8 = g->r8; o->r9 = g->r9; o->r10 = g->r10; o->r11 = g->r11;
    o->r12 = g->r12; o->r13 = g->r13; o->r14 = g->r14; o->r15 = g->r15;
    o->rip = s->rip;
    o->rflags = s->rflags;
    seg_out(&o->cs, &s->cs); seg_out(&o->ds, &s->ds); seg_out(&o->es, &s->es);
    seg_out(&o->fs, &s->fs); seg_out(&o->gs, &s->gs); seg_out(&o->ss, &s->ss);
    seg_out(&o->ldtr, &s->ldtr); seg_out(&o->tr, &s->tr);
    seg_out(&o->gdtr, &s->gdtr); seg_out(&o->idtr, &s->idtr);
    o->cr0 = s->cr0; o->cr2 = s->cr2; o->cr3 = s->cr3; o->cr4 = s->cr4;
    o->cr8 = v->vmcb->control.v_tpr & 0xF;
    o->efer = v->guest_efer;
    o->dr6 = s->dr6; o->dr7 = s->dr7;
    o->pending_irq = ~0ull;
}

static int check_state(const struct cosmo_vcpu_regs *i)
{
    if (i->cr0 >> 32)
        return -EINVAL;
    if ((i->cr0 & CR0_PG) && !(i->cr0 & CR0_PE))
        return -EINVAL;
    if ((i->cr0 & CR0_NW) && !(i->cr0 & CR0_CD))
        return -EINVAL;
    if ((i->efer & ~EFER_KNOWN) || (i->efer & EFER_SVME))
        return -EINVAL;             /* the guest may not become a hypervisor */
    if ((i->efer & EFER_LME) && (i->cr0 & CR0_PG) && !(i->cr4 & CR4_PAE))
        return -EINVAL;
    if ((i->efer & EFER_LME) && (i->cr0 & CR0_PG) && (i->cs.attrib & SEG_ATTR_L) && (i->cs.attrib & SEG_ATTR_DB))
        return -EINVAL;
    if ((i->dr6 >> 32) || (i->dr7 >> 32))
        return -EINVAL;
    return 0;
}

int arch_hv_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *i)
{
    int rc = check_state(i);
    if (rc)
        return rc;
    struct vmcb_save *s = &v->vmcb->save;
    struct svm_gprs *g = &v->gprs;
    s->rax = i->rax; g->rbx = i->rbx; g->rcx = i->rcx; g->rdx = i->rdx;
    g->rsi = i->rsi; g->rdi = i->rdi; g->rbp = i->rbp; s->rsp = i->rsp;
    g->r8 = i->r8; g->r9 = i->r9; g->r10 = i->r10; g->r11 = i->r11;
    g->r12 = i->r12; g->r13 = i->r13; g->r14 = i->r14; g->r15 = i->r15;
    s->rip = i->rip;
    s->rflags = (i->rflags | 0x2) & ~(uint64_t)(1ull << 3 | 1ull << 5 | 1ull << 15);   /* fixed bits */
    seg_in(&s->cs, &i->cs); seg_in(&s->ds, &i->ds); seg_in(&s->es, &i->es);
    seg_in(&s->fs, &i->fs); seg_in(&s->gs, &i->gs); seg_in(&s->ss, &i->ss);
    seg_in(&s->ldtr, &i->ldtr); seg_in(&s->tr, &i->tr);
    seg_in(&s->gdtr, &i->gdtr); seg_in(&s->idtr, &i->idtr);
    s->cr0 = i->cr0; s->cr2 = i->cr2; s->cr3 = i->cr3; s->cr4 = i->cr4;
    v->vmcb->control.v_tpr = (uint8_t)(i->cr8 & 0xF);
    s->dr6 = i->dr6; s->dr7 = i->dr7;
    s->cpl = (i->cr0 & CR0_PE) ? (uint8_t)(i->ss.attrib >> 5 & 3) : 0;
    uint64_t efer = i->efer & ~EFER_SVME & ~EFER_LMA;
    if ((efer & EFER_LME) && (i->cr0 & CR0_PG))
        efer |= EFER_LMA;
    v->guest_efer = efer;
    s->efer = efer | EFER_SVME;
    return 0;
}

uint64_t arch_hv_vcpu_guest_efer(struct arch_hv_vcpu *v)
{
    return v->guest_efer;
}

int arch_hv_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer)
{
    if (efer & ~EFER_KNOWN)
        return -EINVAL;
    if (efer & EFER_SVME)
        return -EINVAL;   /* the guest may not become a hypervisor */
    efer &= ~EFER_LMA;
    if ((efer & EFER_LME) && (v->vmcb->save.cr0 & CR0_PG))
        efer |= EFER_LMA;
    v->guest_efer = efer;
    v->vmcb->save.efer = efer | EFER_SVME;
    return 0;
}

int arch_hv_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value)
{
    struct vmcb_save *s = &v->vmcb->save;
    uint64_t *slot;
    switch (index) {
    case MSR_STAR: slot = &s->star; break;
    case MSR_LSTAR: slot = &s->lstar; break;
    case MSR_CSTAR: slot = &s->cstar; break;
    case MSR_SFMASK: slot = &s->sfmask; break;
    case MSR_FS_BASE_: slot = &s->fs.base; break;
    case MSR_GS_BASE_: slot = &s->gs.base; break;
    case MSR_KERNEL_GS_BASE: slot = &s->kernel_gs_base; break;
    case MSR_SYSENTER_CS: slot = &s->sysenter_cs; break;
    case MSR_SYSENTER_ESP: slot = &s->sysenter_esp; break;
    case MSR_SYSENTER_EIP: slot = &s->sysenter_eip; break;
    case MSR_PAT: slot = &s->g_pat; break;
    default: return -ENOENT;
    }
    if (write)
        *slot = *value;
    else
        *value = *slot;
    return 0;
}

/* GPR index in the x86 encoding order (rax rcx rdx rbx rsp rbp rsi rdi r8..r15). */
static uint64_t *gpr_slot(struct arch_hv_vcpu *v, unsigned index)
{
    struct svm_gprs *g = &v->gprs;
    switch (index & 15) {
    case 0: return &v->vmcb->save.rax;
    case 1: return &g->rcx;
    case 2: return &g->rdx;
    case 3: return &g->rbx;
    case 4: return &v->vmcb->save.rsp;
    case 5: return &g->rbp;
    case 6: return &g->rsi;
    case 7: return &g->rdi;
    case 8: return &g->r8;
    case 9: return &g->r9;
    case 10: return &g->r10;
    case 11: return &g->r11;
    case 12: return &g->r12;
    case 13: return &g->r13;
    case 14: return &g->r14;
    default: return &g->r15;
    }
}

uint64_t arch_hv_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index)
{
    return *gpr_slot(v, index);
}

void arch_hv_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value)
{
    *gpr_slot(v, index) = value;
}

/* Completing an intercepted instruction on the guest's behalf also ends
 * an STI/MOV-SS interrupt shadow that was active when it was intercepted;
 * left in place, the shadow would inhibit the first instruction after
 * every re-entry and a pending interrupt would never be delivered. */
static void skip_instruction(struct arch_hv_vcpu *v, uint64_t next_rip)
{
    v->vmcb->save.rip = next_rip;
    v->vmcb->control.interrupt_shadow = 0;
}

void arch_hv_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes)
{
    skip_instruction(v, v->vmcb->save.rip + bytes);
}

void arch_hv_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip)
{
    skip_instruction(v, rip);
}

uint64_t arch_hv_vcpu_rip(struct arch_hv_vcpu *v)
{
    return v->vmcb->save.rip;
}

void arch_hv_host_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    struct cpuid_regs r;
    cpuid(leaf, subleaf, &r);
    *eax = r.eax;
    *ebx = r.ebx;
    *ecx = r.ecx;
    *edx = r.edx;
}

uint64_t arch_hv_host_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void arch_hv_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size)
{
    uint64_t *rax = &v->vmcb->save.rax;
    switch (size) {
    case 1: *rax = (*rax & ~0xFFull) | (value & 0xFF); break;
    case 2: *rax = (*rax & ~0xFFFFull) | (value & 0xFFFF); break;
    default: *rax = value & 0xFFFFFFFFull; break;   /* 32-bit IN zero-extends */
    }
}

void arch_hv_vcpu_set_irq(struct arch_hv_vcpu *v, int vector)
{
    struct vmcb_control *c = &v->vmcb->control;
    if (vector < 0) {
        c->v_irq = 0;
        c->v_intr_vector = 0;
    } else {
        c->v_irq = 1;
        c->v_intr_vector = (uint8_t)vector;
        c->v_intr_prio = 0x1F;
    }
    v->offered = vector;
}

bool arch_hv_vcpu_irq_taken(struct arch_hv_vcpu *v)
{
    return v->offered >= 0 && (v->vmcb->control.v_irq & 1) == 0;
}

void arch_hv_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error)
{
    uint64_t e = vector | SVM_EVTINJ_TYPE_EXCP | SVM_EVTINJ_VALID;
    if (has_error)
        e |= SVM_EVTINJ_EV | ((uint64_t)error << 32);
    v->vmcb->control.eventinj = e;
}

/* --- run and decode --- */

int arch_hv_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out)
{
    struct vmcb *b = v->vmcb;
    arch_irq_state_t s = arch_irq_save();
    struct svm_cpu *c = enable_this_cpu();
    if (c == NULL) {
        arch_irq_restore(s);
        return -ENOMEM;
    }
    svm_run(v->vmcb_pa, c->host_vmcb_pa, &v->gprs);
    arch_irq_restore(s);

    /* An event that was being delivered when the exit happened is redelivered. */
    if (b->control.exitintinfo & SVM_EVTINJ_VALID)
        b->control.eventinj = b->control.exitintinfo;

    uint64_t code = b->control.exitcode;
    memset(out, 0, sizeof(*out));
    switch (code) {
    case SVM_EXIT_INTR:
    case SVM_EXIT_NMI:
    case SVM_EXIT_SMI:
    case SVM_EXIT_INIT:
        out->kind = HV_EXIT_INTR;
        return 0;
    case SVM_EXIT_HLT:
        arch_hv_vcpu_advance_rip(v, 1);
        out->kind = HV_EXIT_HLT;
        return 0;
    case SVM_EXIT_CPUID:
        out->kind = HV_EXIT_CPUID;
        return 0;
    case SVM_EXIT_INVD:
        arch_hv_vcpu_advance_rip(v, 2);                 /* treated as a no-op (caches are coherent for the guest) */
        out->kind = HV_EXIT_INTR;
        return 0;
    case SVM_EXIT_IOIO: {
        struct svm_ioio io = svm_decode_ioio(b->control.exitinfo1);
        out->kind = HV_EXIT_IO;
        out->io.port = io.port;
        out->io.size = io.size;
        out->io.write = !io.in;
        out->io.string = io.string;
        out->io.rep = io.rep;
        out->io.next_rip = b->control.exitinfo2;   /* architectural for IOIO, NRIP or not */
        return 0;
    }
    case SVM_EXIT_MSR:
        out->kind = HV_EXIT_MSR;
        out->msr.index = (uint32_t)v->gprs.rcx;
        out->msr.write = (b->control.exitinfo1 & 1) != 0;
        return 0;
    case SVM_EXIT_VMMCALL:
        arch_hv_vcpu_advance_rip(v, 3);
        out->kind = HV_EXIT_HYPERCALL;
        return 0;
    case SVM_EXIT_NPF:
        out->kind = HV_EXIT_MMIO;
        out->mmio.gpa = b->control.exitinfo2;
        out->mmio.write = (b->control.exitinfo1 & SVM_NPF_WRITE) != 0;
        return 0;
    case SVM_EXIT_SHUTDOWN:
        out->kind = HV_EXIT_SHUTDOWN;
        return 0;
    case SVM_EXIT_VMRUN:
    case SVM_EXIT_VMLOAD:
    case SVM_EXIT_VMSAVE:
    case SVM_EXIT_STGI:
    case SVM_EXIT_CLGI:
    case SVM_EXIT_SKINIT:
    case SVM_EXIT_INVLPGA:
    case SVM_EXIT_MONITOR:
    case SVM_EXIT_MWAIT:
    case SVM_EXIT_MWAIT_COND:
        /* The guest is not a hypervisor and has no MONITOR: #UD, like a CPU without them. */
        arch_hv_vcpu_inject_exception(v, 6, false, 0);
        out->kind = HV_EXIT_INTR;
        return 0;
    default:
        out->kind = HV_EXIT_FAIL;
        out->fail.code = code;
        out->fail.info1 = b->control.exitinfo1;
        out->fail.info2 = b->control.exitinfo2;
        if (v->unknown_exits++ == 0)
            kwarn("svm: asid %u: exit code 0x%llx info1 0x%llx info2 0x%llx rip 0x%llx", v->vm->asid,
                  (unsigned long long)code, (unsigned long long)b->control.exitinfo1,
                  (unsigned long long)b->control.exitinfo2, (unsigned long long)b->save.rip);
        return 0;
    }
}
