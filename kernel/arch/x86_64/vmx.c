/*
 * vmx.c - Intel VT-x backend for arch/hv.h
 * (docs/kernel-services/virtualization/design.md, "The VMX backend").
 *
 * Structured like svm.c so the two read side by side. The differences
 * are the control structure (a VMCS reached only through vmread/vmwrite,
 * so the backend keeps a software shadow of everything generic code can
 * ask for and writes it in at entry), the interrupt model (external
 * interrupts exit rather than SVM's CLGI/STGI window), and the guest's
 * address space (EPT rather than NPT).
 *
 * Neither QEMU's TCG nor an AArch64 development host can execute a VMX
 * instruction, so nothing below runs in this repository's tests: the
 * pure parts are covered by tests/host/test_vmx.c and the rest is
 * compiled, reviewed against the SDM, and inert until the kernel boots
 * on Intel hardware (docs/.../testing.md records this).
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
#include <x86/fpu.h>
#include <x86/hvops.h>
#include <x86/hvseg.h>
#include <x86/paging.h>
#include <x86/vmx.h>

#define VMX_VPIDS_MAX 16u

/* Architectural bits and MSR numbers this backend needs; the SVM
 * backend's header has its own copies and is not included here. */
#ifndef CR0_ET
#define CR0_ET (1ull << 4)
#endif
#ifndef CR0_NW
#define CR0_NW (1ull << 29)
#endif
#ifndef CR0_CD
#define CR0_CD (1ull << 30)
#endif
#define CR4_VMXE     (1ull << 13)
#define CR4_OSXSAVE_ (1ull << 18)
#ifndef MSR_EFER
#define MSR_EFER 0xC0000080u
#endif
#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_CSTAR          0xC0000083u
#define MSR_SFMASK         0xC0000084u
#define MSR_FS_BASE        0xC0000100u
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u
#define MSR_SYSENTER_CS    0x174u
#define MSR_SYSENTER_ESP   0x175u
#define MSR_SYSENTER_EIP   0x176u
#define MSR_PAT            0x277u
#define PAT_DEFAULT        0x0007040600070406ull

/* Host descriptors, read where they are needed rather than cached: a
 * VM exit restores them from the VMCS, so they must describe the CPU
 * this vCPU is about to run on. */
static inline uint16_t read_cs(void)
{
    uint16_t v;
    __asm__ volatile("mov %%cs, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_ss(void)
{
    uint16_t v;
    __asm__ volatile("mov %%ss, %0" : "=r"(v));
    return v;
}

static inline uint16_t read_tr(void)
{
    uint16_t v;
    __asm__ volatile("str %0" : "=r"(v));
    return v;
}

struct desc_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static inline uint64_t gdt_base(void)
{
    struct desc_ptr d;
    __asm__ volatile("sgdt %0" : "=m"(d));
    return d.base;
}

static inline uint64_t idt_base(void)
{
    struct desc_ptr d;
    __asm__ volatile("sidt %0" : "=m"(d));
    return d.base;
}

/* The TSS descriptor is 16 bytes in long mode: base in three pieces. */
static uint64_t tss_base(void)
{
    uint16_t sel = read_tr() & ~7u;
    const uint8_t *e = (const uint8_t *)(uintptr_t)(gdt_base() + sel);
    uint64_t base = (uint64_t)e[2] | ((uint64_t)e[3] << 8) | ((uint64_t)e[4] << 16) | ((uint64_t)e[7] << 24);
    uint32_t high;
    memcpy(&high, e + 8, sizeof(high));
    return base | ((uint64_t)high << 32);
}

/* The guest register file the entry stub loads and the exit stub saves.
 * RSP and RIP are VMCS fields and are not here. The offsets are part of
 * the contract with vmx_run.S. */
struct vmx_gprs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

/* Everything generic code may read or write between runs. The VMCS is
 * only reachable while it is the current one on this CPU, so the shadow
 * is the source of truth and is written in at entry. */
struct vmx_state {
    struct cosmo_vcpu_seg cs, ds, es, fs, gs, ss, ldtr, tr, gdtr, idtr;
    uint64_t cr0, cr2, cr3, cr4, dr6, dr7, rflags, rip, rsp;
    uint64_t efer, pat, sysenter_cs, sysenter_esp, sysenter_eip;
    uint64_t star, lstar, cstar, sfmask, kernel_gs_base;
};

struct arch_hv_vm {
    paddr_t eptp_root;
    uint16_t vpid;
};

struct arch_hv_vcpu {
    struct arch_hv_vm *vm;
    paddr_t vmcs_pa;
    void *vmcs;
    struct vmx_gprs gprs;
    struct vmx_state st;
    bool launched;         /* VMLAUNCH once, VMRESUME after */
    int loaded_cpu;        /* the CPU whose current VMCS this is, -1 for none */
    int offered;           /* the vector the manager offered, -1 for none */
    bool irq_taken;
    uint32_t pending_event;   /* VM-entry interruption information, 0 = none */
    uint32_t pending_error;
    uint64_t guest_xcr0;
    unsigned unknown_exits;
    void *fpu, *fpu_raw;
    /* the last exit, cached for the decoder */
    uint32_t exit_reason, exit_intr_info, exit_instr_len;
    uint64_t exit_qual, exit_gpa;
};

struct vmx_cpu {
    bool enabled;
    paddr_t vmxon_pa;
};

static struct hv_caps g_caps = { .present = false, .name = "none" };
static struct vmx_cpu g_cpus[CONFIG_MAX_CPUS];
static paddr_t g_msr_bitmap_pa;
static uint32_t g_revision;
static bool g_true_ctls;
static uint32_t g_pin_ctls, g_cpu_ctls, g_cpu_ctls2, g_exit_ctls, g_entry_ctls;
static uint64_t g_cr0_fixed0, g_cr0_fixed1, g_cr4_fixed0, g_cr4_fixed1;
static uint32_t g_vpid_used = 1;   /* bit i: VPID i taken; 0 is the host's */
static spinlock_t g_vpid_lock = SPINLOCK_INIT("vmx-vpid");

/* --- VMCS access -------------------------------------------------------- */

static inline uint64_t vmread(uint32_t field)
{
    uint64_t value = 0;
    __asm__ volatile("vmread %1, %0" : "=rm"(value) : "r"((uint64_t)field) : "cc");
    return value;
}

static inline void vmwrite(uint32_t field, uint64_t value)
{
    __asm__ volatile("vmwrite %1, %0" : : "r"((uint64_t)field), "rm"(value) : "cc");
}

static inline bool vmptrld(paddr_t pa)
{
    bool fail;
    __asm__ volatile("vmptrld %1; setna %0" : "=q"(fail) : "m"(pa) : "cc");
    return !fail;
}

static inline void vmclear(paddr_t pa)
{
    __asm__ volatile("vmclear %0" : : "m"(pa) : "cc");
}

/* vmx_run.S: loads the guest GPRs, enters, and saves them on the way
 * out. Returns 0 for an exit, 1 when the entry itself failed. */
extern uint32_t vmx_run(struct vmx_gprs *gprs, uint32_t launched);

/* --- setup helpers ------------------------------------------------------ */

static paddr_t alloc_page_filled(int fill)
{
    struct page *pg = pmm_alloc_page(0);
    if (pg == NULL)
        return 0;
    memset(page_to_virt(pg), fill, PAGE_SIZE);
    return page_to_phys(pg);
}

static int vpid_alloc(uint16_t *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_vpid_lock);
    for (unsigned i = 1; i < VMX_VPIDS_MAX; i++) {
        if (!(g_vpid_used & (1u << i))) {
            g_vpid_used |= 1u << i;
            spin_unlock_irqrestore(&g_vpid_lock, s);
            *out = (uint16_t)i;
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_vpid_lock, s);
    return -EBUSY;
}

static void vpid_free(uint16_t vpid)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_vpid_lock);
    g_vpid_used &= ~(1u << vpid);
    spin_unlock_irqrestore(&g_vpid_lock, s);
}

static int vmx_be_probe(struct hv_caps *out)
{
    struct cpuid_regs r;
    cpuid(1, 0, &r);
    if (!(r.ecx & (1u << 5))) {   /* CPUID.1:ECX.VMX */
        *out = g_caps;
        return -ENOTSUP;
    }
    uint64_t fc = rdmsr(MSR_IA32_FEATURE_CONTROL);
    if (fc & FEATURE_CONTROL_LOCK) {
        if (!(fc & FEATURE_CONTROL_VMXON)) {
            kwarn("vmx: disabled by firmware (IA32_FEATURE_CONTROL locked without VMXON)");
            *out = g_caps;
            return -ENOTSUP;
        }
    } else {
        wrmsr(MSR_IA32_FEATURE_CONTROL, fc | FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMXON);
    }

    uint64_t basic = rdmsr(MSR_IA32_VMX_BASIC);
    g_revision = VMX_BASIC_REVISION(basic);
    g_true_ctls = (basic & VMX_BASIC_TRUE_CTLS) != 0;
    if (VMX_BASIC_SIZE(basic) > PAGE_SIZE || VMX_BASIC_MEMTYPE(basic) != 6) {
        kwarn("vmx: unusable VMCS (size %u, memory type %u)", VMX_BASIC_SIZE(basic), VMX_BASIC_MEMTYPE(basic));
        *out = g_caps;
        return -ENOTSUP;
    }

    uint64_t pin_cap = rdmsr(g_true_ctls ? MSR_IA32_VMX_TRUE_PINBASED : MSR_IA32_VMX_PINBASED_CTLS);
    uint64_t cpu_cap = rdmsr(g_true_ctls ? MSR_IA32_VMX_TRUE_PROCBASED : MSR_IA32_VMX_PROCBASED_CTLS);
    uint64_t exit_cap = rdmsr(g_true_ctls ? MSR_IA32_VMX_TRUE_EXIT : MSR_IA32_VMX_EXIT_CTLS);
    uint64_t entry_cap = rdmsr(g_true_ctls ? MSR_IA32_VMX_TRUE_ENTRY : MSR_IA32_VMX_ENTRY_CTLS);

    /* What the design requires of the CPU; anything missing means the
     * backend is not used, as SVM refuses a CPU without nested paging. */
    uint32_t pin_want = PIN_EXT_INTR_EXITING | PIN_NMI_EXITING;
    uint32_t cpu_want = CPU_HLT_EXITING | CPU_UNCOND_IO_EXITING | CPU_USE_MSR_BITMAPS | CPU_SECONDARY_CTLS |
                        CPU_MWAIT_EXITING | CPU_MONITOR_EXITING | CPU_RDPMC_EXITING | CPU_MOV_DR_EXITING |
                        CPU_INVLPG_EXITING;
    uint32_t exit_want = EXIT_CTL_HOST_ADDR_SPACE | EXIT_CTL_SAVE_EFER | EXIT_CTL_LOAD_EFER | EXIT_CTL_SAVE_PAT |
                         EXIT_CTL_LOAD_PAT | EXIT_CTL_ACK_INTR;
    uint32_t entry_want = ENTRY_CTL_LOAD_EFER | ENTRY_CTL_LOAD_PAT;
    if (!vmx_ctls_ok(pin_cap, pin_want) || !vmx_ctls_ok(cpu_cap, cpu_want) || !vmx_ctls_ok(exit_cap, exit_want) ||
        !vmx_ctls_ok(entry_cap, entry_want)) {
        kwarn("vmx: the CPU lacks a required execution control; not used");
        *out = g_caps;
        return -ENOTSUP;
    }

    uint64_t cpu2_cap = rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
    uint32_t cpu2_want = CPU2_ENABLE_EPT | CPU2_UNRESTRICTED | CPU2_WBINVD_EXITING | CPU2_DESC_TABLE_EXITING;
    uint64_t ept_cap = rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
    if (!vmx_ctls_ok(cpu2_cap, CPU2_ENABLE_EPT) || !(ept_cap & VMX_EPT_PAGE_WALK_4) ||
        !(ept_cap & VMX_EPT_MEMTYPE_WB)) {
        kwarn("vmx: present without four-level write-back EPT; not used");
        *out = g_caps;
        return -ENOTSUP;
    }
    bool unrestricted = vmx_ctls_ok(cpu2_cap, CPU2_UNRESTRICTED);
    bool vpid = vmx_ctls_ok(cpu2_cap, CPU2_ENABLE_VPID) && (ept_cap & VMX_EPT_INVVPID);
    if (vpid)
        cpu2_want |= CPU2_ENABLE_VPID;
    if (!unrestricted)
        cpu2_want &= ~(uint32_t)CPU2_UNRESTRICTED;

    g_pin_ctls = vmx_fix_ctls(pin_cap, pin_want);
    g_cpu_ctls = vmx_fix_ctls(cpu_cap, cpu_want);
    g_cpu_ctls2 = vmx_fix_ctls(cpu2_cap, cpu2_want);
    g_exit_ctls = vmx_fix_ctls(exit_cap, exit_want);
    g_entry_ctls = vmx_fix_ctls(entry_cap, entry_want);
    g_cr0_fixed0 = rdmsr(MSR_IA32_VMX_CR0_FIXED0);
    g_cr0_fixed1 = rdmsr(MSR_IA32_VMX_CR0_FIXED1);
    g_cr4_fixed0 = rdmsr(MSR_IA32_VMX_CR4_FIXED0);
    g_cr4_fixed1 = rdmsr(MSR_IA32_VMX_CR4_FIXED1);

    g_msr_bitmap_pa = alloc_page_filled(0xFF);   /* every MSR exits, as SVM's MSRPM */
    if (g_msr_bitmap_pa == 0)
        return -ENOMEM;

    g_caps.present = true;
    g_caps.name = "vmx";
    g_caps.max_asids = vpid ? VMX_VPIDS_MAX : 1;
    g_caps.nested_paging = true;
    g_caps.real_mode_guest = unrestricted;   /* without it, only a paged guest can start */
    g_caps.map_prot = true;
    g_caps.large_pages = (ept_cap & VMX_EPT_2MB) != 0;
    g_caps.max_vcpus = 0;
    kinfo("vmx: VT-x with EPT%s%s, revision %u", unrestricted ? ", unrestricted guest" : "",
          vpid ? ", VPID" : "", g_revision);
    *out = g_caps;
    return 0;
}

/* Interrupts disabled. VMXON on this CPU the first time it runs a guest. */
static struct vmx_cpu *enable_this_cpu(void)
{
    struct vmx_cpu *c = &g_cpus[this_cpu()->cpu_id];
    if (c->enabled)
        return c;
    paddr_t region = alloc_page_filled(0);
    if (region == 0)
        return NULL;
    *(uint32_t *)phys_to_virt(region) = g_revision;
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 | CR4_VMXE);
    /* CR0 and CR4 must satisfy the fixed-bit MSRs before VMXON. */
    write_cr0((read_cr0() | g_cr0_fixed0) & g_cr0_fixed1);
    write_cr4((read_cr4() | g_cr4_fixed0) & g_cr4_fixed1);
    bool fail;
    __asm__ volatile("vmxon %1; setna %0" : "=q"(fail) : "m"(region) : "cc");
    if (fail) {
        write_cr4(cr4);
        pmm_free_page(phys_to_page(region));
        kerror("vmx: VMXON refused on CPU %u", this_cpu()->cpu_id);
        return NULL;
    }
    c->vmxon_pa = region;
    c->enabled = true;
    return c;
}

/* --- VM --------------------------------------------------------------- */

static int vmx_be_vm_create(struct arch_hv_vm **out)
{
    if (!g_caps.present)
        return -ENOTSUP;
    struct arch_hv_vm *vm = kzalloc(sizeof(*vm));
    if (vm == NULL)
        return -ENOMEM;
    if (g_cpu_ctls2 & CPU2_ENABLE_VPID) {
        int rc = vpid_alloc(&vm->vpid);
        if (rc) {
            kfree(vm);
            return rc;
        }
    }
    vm->eptp_root = ept_create();
    if (vm->eptp_root == 0) {
        if (vm->vpid)
            vpid_free(vm->vpid);
        kfree(vm);
        return -ENOMEM;
    }
    *out = vm;
    return 0;
}

static void vmx_be_vm_destroy(struct arch_hv_vm *vm)
{
    if (vm == NULL)
        return;
    ept_destroy(vm->eptp_root);
    if (vm->vpid)
        vpid_free(vm->vpid);
    kfree(vm);
}

static int vmx_be_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot)
{
    return ept_map(vm->eptp_root, gpa, hpa, len, prot);
}

static int vmx_be_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len)
{
    return ept_unmap(vm->eptp_root, gpa, len);
}

static bool vmx_be_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa)
{
    return ept_query(vm->eptp_root, gpa, hpa);
}

/* --- vCPU ------------------------------------------------------------- */

static void seg_reset(struct cosmo_vcpu_seg *s, uint16_t attrib, uint32_t limit)
{
    s->selector = 0;
    s->attrib = attrib;
    s->limit = limit;
    s->base = 0;
}

/* The architectural reset state, which only an unrestricted guest can
 * enter directly (caps.real_mode_guest). */
static void vmx_state_reset(struct arch_hv_vcpu *v)
{
    struct vmx_state *s = &v->st;
    memset(s, 0, sizeof(*s));
    const uint16_t code_rm = COSMO_SEG_P | COSMO_SEG_S | 0xB;   /* execute/read, accessed */
    const uint16_t data_rm = COSMO_SEG_P | COSMO_SEG_S | 0x3;   /* read/write, accessed */
    seg_reset(&s->cs, code_rm, 0xFFFF);
    seg_reset(&s->ds, data_rm, 0xFFFF);
    seg_reset(&s->es, data_rm, 0xFFFF);
    seg_reset(&s->fs, data_rm, 0xFFFF);
    seg_reset(&s->gs, data_rm, 0xFFFF);
    seg_reset(&s->ss, data_rm, 0xFFFF);
    seg_reset(&s->ldtr, 0x82, 0xFFFF);        /* LDT, present */
    seg_reset(&s->tr, 0x8B, 0xFFFF);          /* 32-bit TSS, busy */
    seg_reset(&s->gdtr, 0, 0xFFFF);
    seg_reset(&s->idtr, 0, 0xFFFF);
    s->cr0 = CR0_ET | CR0_NW | CR0_CD;
    s->rflags = 0x2;
    s->dr6 = 0xFFFF0FF0ull;
    s->dr7 = 0x400;
    s->pat = PAT_DEFAULT;
    memset(&v->gprs, 0, sizeof(v->gprs));
    v->offered = -1;
    v->launched = false;
    v->loaded_cpu = -1;
    v->guest_xcr0 = XCR0_X87 | XCR0_SSE;
    memcpy(v->fpu, x86_fpu_reset_image(), x86_fpu_info()->area_size);
}

static int vmx_be_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out)
{
    if (!g_caps.present)
        return -ENOTSUP;
    struct arch_hv_vcpu *v = kzalloc(sizeof(*v));
    if (v == NULL)
        return -ENOMEM;
    v->fpu_raw = kmalloc(x86_fpu_info()->area_size + 64, 0);
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (v->fpu_raw == NULL || pg == NULL) {
        if (pg)
            pmm_free_page(pg);
        kfree(v->fpu_raw);
        kfree(v);
        return -ENOMEM;
    }
    v->fpu = (void *)(((uintptr_t)v->fpu_raw + 63) & ~(uintptr_t)63);
    v->vm = vm;
    v->vmcs_pa = page_to_phys(pg);
    v->vmcs = page_to_virt(pg);
    *(uint32_t *)v->vmcs = g_revision;   /* the VMCS revision identifier */
    vmx_state_reset(v);
    *out = v;
    return 0;
}

static void vmx_be_vcpu_destroy(struct arch_hv_vcpu *v)
{
    if (v == NULL)
        return;
    if (v->loaded_cpu >= 0)
        vmclear(v->vmcs_pa);
    pmm_free_page(phys_to_page(v->vmcs_pa));
    kfree(v->fpu_raw);
    kfree(v);
}

static void vmx_be_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *o)
{
    memset(o, 0, sizeof(*o));
    const struct vmx_gprs *g = &v->gprs;
    o->rax = g->rax; o->rbx = g->rbx; o->rcx = g->rcx; o->rdx = g->rdx;
    o->rsi = g->rsi; o->rdi = g->rdi; o->rbp = g->rbp;
    o->r8 = g->r8; o->r9 = g->r9; o->r10 = g->r10; o->r11 = g->r11;
    o->r12 = g->r12; o->r13 = g->r13; o->r14 = g->r14; o->r15 = g->r15;
    o->rsp = v->st.rsp;
    o->rip = v->st.rip;
    o->rflags = v->st.rflags;
    o->cs = v->st.cs; o->ds = v->st.ds; o->es = v->st.es; o->fs = v->st.fs;
    o->gs = v->st.gs; o->ss = v->st.ss; o->ldtr = v->st.ldtr; o->tr = v->st.tr;
    o->gdtr = v->st.gdtr; o->idtr = v->st.idtr;
    o->cr0 = v->st.cr0; o->cr2 = v->st.cr2; o->cr3 = v->st.cr3; o->cr4 = v->st.cr4;
    o->efer = v->st.efer;
    o->dr6 = v->st.dr6; o->dr7 = v->st.dr7;
    o->pending_irq = v->offered < 0 ? ~0ull : (uint64_t)v->offered;
}

static int check_state(const struct cosmo_vcpu_regs *i)
{
    if (i->cr0 >> 32)
        return -EINVAL;
    if ((i->cr0 & CR0_PG) && !(i->cr0 & CR0_PE))
        return -EINVAL;
    if ((i->cr0 & CR0_NW) && !(i->cr0 & CR0_CD))
        return -EINVAL;
    if (i->efer & ~(uint64_t)(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE))
        return -EINVAL;
    if ((i->efer & EFER_LME) && (i->cr0 & CR0_PG) && !(i->cr4 & CR4_PAE))
        return -EINVAL;
    const struct cosmo_vcpu_seg *segs[] = { &i->cs, &i->ds, &i->es, &i->fs, &i->gs, &i->ss, &i->ldtr, &i->tr };
    for (unsigned k = 0; k < sizeof(segs) / sizeof(segs[0]); k++)
        if (!hv_seg_attrib_valid(segs[k]->attrib))
            return -EINVAL;
    if ((i->efer & EFER_LME) && (i->cr0 & CR0_PG) && (i->cs.attrib & COSMO_SEG_L) && (i->cs.attrib & COSMO_SEG_DB))
        return -EINVAL;
    /* CR4.VMXE stays the host's: a guest may not become a hypervisor. */
    if (i->cr4 & CR4_VMXE)
        return -EINVAL;
    if ((i->dr6 >> 32) || (i->dr7 >> 32))
        return -EINVAL;
    return 0;
}

static int vmx_be_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *i)
{
    int rc = check_state(i);
    if (rc)
        return rc;
    struct vmx_gprs *g = &v->gprs;
    g->rax = i->rax; g->rbx = i->rbx; g->rcx = i->rcx; g->rdx = i->rdx;
    g->rsi = i->rsi; g->rdi = i->rdi; g->rbp = i->rbp;
    g->r8 = i->r8; g->r9 = i->r9; g->r10 = i->r10; g->r11 = i->r11;
    g->r12 = i->r12; g->r13 = i->r13; g->r14 = i->r14; g->r15 = i->r15;
    v->st.rsp = i->rsp;
    v->st.rip = i->rip;
    v->st.rflags = i->rflags | 0x2;   /* bit 1 reads as one */
    v->st.cs = i->cs; v->st.ds = i->ds; v->st.es = i->es; v->st.fs = i->fs;
    v->st.gs = i->gs; v->st.ss = i->ss; v->st.ldtr = i->ldtr; v->st.tr = i->tr;
    v->st.gdtr = i->gdtr; v->st.idtr = i->idtr;
    v->st.cr0 = i->cr0; v->st.cr2 = i->cr2; v->st.cr3 = i->cr3; v->st.cr4 = i->cr4;
    v->st.efer = i->efer;
    v->st.dr6 = i->dr6; v->st.dr7 = i->dr7;
    return 0;
}

static uint64_t vmx_be_vcpu_guest_efer(struct arch_hv_vcpu *v)
{
    return v->st.efer;
}

static int vmx_be_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer)
{
    if (efer & ~(uint64_t)(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE))
        return -EINVAL;
    v->st.efer = efer;
    return 0;
}

/* The MSRs this backend keeps for the guest. On VMX these live in VMCS
 * fields or the entry/exit MSR lists rather than in a save area, which
 * is exactly why the interface names MSRs and not a save area. */
static int vmx_be_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value)
{
    uint64_t *slot;
    switch (index) {
    case MSR_STAR: slot = &v->st.star; break;
    case MSR_LSTAR: slot = &v->st.lstar; break;
    case MSR_CSTAR: slot = &v->st.cstar; break;
    case MSR_SFMASK: slot = &v->st.sfmask; break;
    case MSR_KERNEL_GS_BASE: slot = &v->st.kernel_gs_base; break;
    case MSR_FS_BASE: slot = &v->st.fs.base; break;
    case MSR_GS_BASE: slot = &v->st.gs.base; break;
    case MSR_PAT: slot = &v->st.pat; break;
    case MSR_SYSENTER_CS: slot = &v->st.sysenter_cs; break;
    case MSR_SYSENTER_ESP: slot = &v->st.sysenter_esp; break;
    case MSR_SYSENTER_EIP: slot = &v->st.sysenter_eip; break;
    default:
        return -ENOENT;   /* not ours: the generic MSR model decides */
    }
    if (write)
        *slot = *value;
    else
        *value = *slot;
    return 0;
}

static bool vmx_be_vcpu_xstate_enabled(struct arch_hv_vcpu *v)
{
    return (v->st.cr4 & CR4_OSXSAVE_) != 0;
}

static uint64_t *gpr_slot(struct arch_hv_vcpu *v, unsigned index)
{
    struct vmx_gprs *g = &v->gprs;
    switch (index) {
    case HV_GPR_RAX: return &g->rax;
    case HV_GPR_RCX: return &g->rcx;
    case HV_GPR_RDX: return &g->rdx;
    case HV_GPR_RBX: return &g->rbx;
    case HV_GPR_RSP: return &v->st.rsp;
    case HV_GPR_RBP: return &g->rbp;
    case HV_GPR_RSI: return &g->rsi;
    case HV_GPR_RDI: return &g->rdi;
    default: return NULL;
    }
}

static uint64_t vmx_be_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index)
{
    uint64_t *p = gpr_slot(v, index);
    return p ? *p : 0;
}

static void vmx_be_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value)
{
    uint64_t *p = gpr_slot(v, index);
    if (p)
        *p = value;
}

static void vmx_be_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size)
{
    uint64_t mask = size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
    v->gprs.rax = (v->gprs.rax & ~mask) | (value & mask);
}

/* Advancing over an instruction also ends any interrupt shadow, as on
 * SVM: the shadow is a VMCS field written at the next entry. */
static void skip_instruction(struct arch_hv_vcpu *v, unsigned len)
{
    v->st.rip += len;
    if (v->loaded_cpu == (int)this_cpu()->cpu_id)
        vmwrite(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

static void vmx_be_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes)
{
    skip_instruction(v, bytes);
}

static void vmx_be_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip)
{
    v->st.rip = rip;
}

static uint64_t vmx_be_vcpu_rip(struct arch_hv_vcpu *v)
{
    return v->st.rip;
}

static void vmx_be_vcpu_set_irq(struct arch_hv_vcpu *v, int vector)
{
    v->offered = vector;
}

static bool vmx_be_vcpu_irq_taken(struct arch_hv_vcpu *v)
{
    return v->irq_taken;
}

static void vmx_be_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error)
{
    v->pending_event = VMX_ENTRY_INTR_VALID | VMX_ENTRY_INTR_TYPE_HW | vector |
                       (has_error ? VMX_ENTRY_INTR_ERR_VALID : 0);
    v->pending_error = error;
}

/* --- entry and exit ---------------------------------------------------- */

static void write_seg(uint32_t sel_f, uint32_t base_f, uint32_t limit_f, uint32_t ar_f,
                      const struct cosmo_vcpu_seg *s)
{
    vmwrite(sel_f, s->selector);
    vmwrite(base_f, s->base);
    vmwrite(limit_f, s->limit);
    vmwrite(ar_f, hv_seg_to_vmx(s->attrib));
}

static void read_seg(uint32_t sel_f, uint32_t base_f, uint32_t limit_f, uint32_t ar_f, struct cosmo_vcpu_seg *s)
{
    s->selector = (uint16_t)vmread(sel_f);
    s->base = vmread(base_f);
    s->limit = (uint32_t)vmread(limit_f);
    s->attrib = hv_seg_from_vmx((uint32_t)vmread(ar_f));
}

/* The host state a VM exit restores. Written whenever this VMCS becomes
 * current on a CPU, because it describes that CPU. */
static void write_host_state(void)
{
    vmwrite(VMCS_HOST_CR0, read_cr0());
    vmwrite(VMCS_HOST_CR3, read_cr3());
    vmwrite(VMCS_HOST_CR4, read_cr4());
    vmwrite(VMCS_HOST_CS_SEL, read_cs() & ~7u);
    vmwrite(VMCS_HOST_DS_SEL, 0);
    vmwrite(VMCS_HOST_ES_SEL, 0);
    vmwrite(VMCS_HOST_SS_SEL, read_ss() & ~7u);
    vmwrite(VMCS_HOST_FS_SEL, 0);
    vmwrite(VMCS_HOST_GS_SEL, 0);
    vmwrite(VMCS_HOST_TR_SEL, read_tr() & ~7u);
    vmwrite(VMCS_HOST_FS_BASE, rdmsr(MSR_FS_BASE));
    vmwrite(VMCS_HOST_GS_BASE, rdmsr(MSR_GS_BASE));
    vmwrite(VMCS_HOST_TR_BASE, tss_base());
    vmwrite(VMCS_HOST_GDTR_BASE, gdt_base());
    vmwrite(VMCS_HOST_IDTR_BASE, idt_base());
    vmwrite(VMCS_HOST_EFER, rdmsr(MSR_EFER));
    vmwrite(VMCS_HOST_PAT, rdmsr(MSR_PAT));
    vmwrite(VMCS_HOST_SYSENTER_CS, 0);
    vmwrite(VMCS_HOST_SYSENTER_ESP, 0);
    vmwrite(VMCS_HOST_SYSENTER_EIP, 0);
}

/* The controls and the parts of the VMCS that never change for a vCPU. */
static void write_controls(struct arch_hv_vcpu *v)
{
    vmwrite(VMCS_PIN_CTLS, g_pin_ctls);
    vmwrite(VMCS_CPU_CTLS, g_cpu_ctls);
    vmwrite(VMCS_CPU_CTLS2, g_cpu_ctls2);
    vmwrite(VMCS_EXIT_CTLS, g_exit_ctls);
    vmwrite(VMCS_ENTRY_CTLS, g_entry_ctls);
    vmwrite(VMCS_MSR_BITMAP, g_msr_bitmap_pa);
    vmwrite(VMCS_EPT_POINTER, vmx_eptp(v->vm->eptp_root, false));
    vmwrite(VMCS_VPID, v->vm->vpid);
    vmwrite(VMCS_LINK_POINTER, ~0ull);
    vmwrite(VMCS_EXCEPTION_BITMAP, 0);
    vmwrite(VMCS_PF_ERROR_MASK, 0);
    vmwrite(VMCS_PF_ERROR_MATCH, 0);
    vmwrite(VMCS_CR3_TARGET_COUNT, 0);
    vmwrite(VMCS_EXIT_MSR_STORE_COUNT, 0);
    vmwrite(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    /* The guest owns every CR0/CR4 bit except the ones that would make
     * it a hypervisor or break the host's view of it. */
    vmwrite(VMCS_CR0_MASK, CR0_PG | CR0_PE);
    vmwrite(VMCS_CR4_MASK, CR4_VMXE);
    vmwrite(VMCS_CR4_READ_SHADOW, 0);
}

static void load_guest_state(struct arch_hv_vcpu *v)
{
    const struct vmx_state *s = &v->st;
    write_seg(VMCS_GUEST_CS_SEL, VMCS_GUEST_CS_BASE, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, &s->cs);
    write_seg(VMCS_GUEST_DS_SEL, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, &s->ds);
    write_seg(VMCS_GUEST_ES_SEL, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, &s->es);
    write_seg(VMCS_GUEST_FS_SEL, VMCS_GUEST_FS_BASE, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR, &s->fs);
    write_seg(VMCS_GUEST_GS_SEL, VMCS_GUEST_GS_BASE, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR, &s->gs);
    write_seg(VMCS_GUEST_SS_SEL, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, &s->ss);
    write_seg(VMCS_GUEST_LDTR_SEL, VMCS_GUEST_LDTR_BASE, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, &s->ldtr);
    write_seg(VMCS_GUEST_TR_SEL, VMCS_GUEST_TR_BASE, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_AR, &s->tr);
    vmwrite(VMCS_GUEST_GDTR_BASE, s->gdtr.base);
    vmwrite(VMCS_GUEST_GDTR_LIMIT, s->gdtr.limit);
    vmwrite(VMCS_GUEST_IDTR_BASE, s->idtr.base);
    vmwrite(VMCS_GUEST_IDTR_LIMIT, s->idtr.limit);
    /* CR0 and CR4 must satisfy the fixed-bit MSRs; the guest sees what
     * it wrote through the read shadows. */
    vmwrite(VMCS_GUEST_CR0, (s->cr0 | g_cr0_fixed0) & g_cr0_fixed1);
    vmwrite(VMCS_CR0_READ_SHADOW, s->cr0);
    vmwrite(VMCS_GUEST_CR3, s->cr3);
    vmwrite(VMCS_GUEST_CR4, (s->cr4 | g_cr4_fixed0) & g_cr4_fixed1);
    vmwrite(VMCS_CR4_READ_SHADOW, s->cr4);
    vmwrite(VMCS_GUEST_DR7, s->dr7);
    vmwrite(VMCS_GUEST_RSP, s->rsp);
    vmwrite(VMCS_GUEST_RIP, s->rip);
    vmwrite(VMCS_GUEST_RFLAGS, s->rflags);
    vmwrite(VMCS_GUEST_EFER, s->efer);
    vmwrite(VMCS_GUEST_PAT, s->pat);
    vmwrite(VMCS_GUEST_SYSENTER_CS, s->sysenter_cs);
    vmwrite(VMCS_GUEST_SYSENTER_ESP, s->sysenter_esp);
    vmwrite(VMCS_GUEST_SYSENTER_EIP, s->sysenter_eip);
    vmwrite(VMCS_GUEST_ACTIVITY_STATE, 0);
    vmwrite(VMCS_GUEST_PENDING_DBG, 0);
    /* A 64-bit guest needs the entry control that says so. */
    uint32_t entry = g_entry_ctls;
    if ((s->efer & EFER_LMA) && (s->cr0 & CR0_PG))
        entry |= ENTRY_CTL_IA32E_GUEST;
    else
        entry &= ~(uint32_t)ENTRY_CTL_IA32E_GUEST;
    vmwrite(VMCS_ENTRY_CTLS, entry);
}

static void save_guest_state(struct arch_hv_vcpu *v)
{
    struct vmx_state *s = &v->st;
    read_seg(VMCS_GUEST_CS_SEL, VMCS_GUEST_CS_BASE, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, &s->cs);
    read_seg(VMCS_GUEST_DS_SEL, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, &s->ds);
    read_seg(VMCS_GUEST_ES_SEL, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, &s->es);
    read_seg(VMCS_GUEST_FS_SEL, VMCS_GUEST_FS_BASE, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR, &s->fs);
    read_seg(VMCS_GUEST_GS_SEL, VMCS_GUEST_GS_BASE, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR, &s->gs);
    read_seg(VMCS_GUEST_SS_SEL, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, &s->ss);
    read_seg(VMCS_GUEST_LDTR_SEL, VMCS_GUEST_LDTR_BASE, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, &s->ldtr);
    read_seg(VMCS_GUEST_TR_SEL, VMCS_GUEST_TR_BASE, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_AR, &s->tr);
    s->gdtr.base = vmread(VMCS_GUEST_GDTR_BASE);
    s->gdtr.limit = (uint32_t)vmread(VMCS_GUEST_GDTR_LIMIT);
    s->idtr.base = vmread(VMCS_GUEST_IDTR_BASE);
    s->idtr.limit = (uint32_t)vmread(VMCS_GUEST_IDTR_LIMIT);
    s->cr0 = vmread(VMCS_CR0_READ_SHADOW);
    s->cr3 = vmread(VMCS_GUEST_CR3);
    s->cr4 = vmread(VMCS_CR4_READ_SHADOW);
    s->dr7 = vmread(VMCS_GUEST_DR7);
    s->rsp = vmread(VMCS_GUEST_RSP);
    s->rip = vmread(VMCS_GUEST_RIP);
    s->rflags = vmread(VMCS_GUEST_RFLAGS);
    s->efer = vmread(VMCS_GUEST_EFER);
    s->pat = vmread(VMCS_GUEST_PAT);
    s->sysenter_cs = vmread(VMCS_GUEST_SYSENTER_CS);
    s->sysenter_esp = vmread(VMCS_GUEST_SYSENTER_ESP);
    s->sysenter_eip = vmread(VMCS_GUEST_SYSENTER_EIP);
}

/* Offer the manager's vector when the guest can take it. */
static void inject_pending(struct arch_hv_vcpu *v)
{
    v->irq_taken = false;
    if (v->pending_event) {
        vmwrite(VMCS_ENTRY_INTR_INFO, v->pending_event);
        if (v->pending_event & VMX_ENTRY_INTR_ERR_VALID)
            vmwrite(VMCS_ENTRY_EXCEPTION_ERR, v->pending_error);
        v->pending_event = 0;
        return;
    }
    if (v->offered < 0)
        return;
    bool interruptible = (v->st.rflags & 0x200) != 0 &&
                         (vmread(VMCS_GUEST_INTERRUPTIBILITY) & (VMX_INTR_SHADOW_STI | VMX_INTR_SHADOW_MOVSS)) == 0;
    if (!interruptible) {
        /* Ask to be told the moment it becomes interruptible. */
        vmwrite(VMCS_CPU_CTLS, g_cpu_ctls | CPU_INTR_WINDOW);
        return;
    }
    vmwrite(VMCS_CPU_CTLS, g_cpu_ctls);
    vmwrite(VMCS_ENTRY_INTR_INFO, VMX_ENTRY_INTR_VALID | VMX_ENTRY_INTR_TYPE_EXT | (uint32_t)v->offered);
    v->irq_taken = true;
}

static int decode_exit(struct arch_hv_vcpu *v, struct hv_exit *out)
{
    uint32_t reason = v->exit_reason & VMX_EXIT_REASON_MASK;
    if (v->exit_reason & VMX_EXIT_ENTRY_FAILURE) {
        out->kind = HV_EXIT_FAIL;
        out->fail.code = reason;
        out->fail.info1 = v->exit_qual;
        out->fail.info2 = vmread(VMCS_INSTRUCTION_ERROR);
        return 0;
    }
    switch (reason) {
    case VMX_EXIT_EXT_INTERRUPT:
    case VMX_EXIT_EXCEPTION_NMI:
    case VMX_EXIT_INIT:
    case VMX_EXIT_SIPI:
    case VMX_EXIT_INTR_WINDOW:
    case VMX_EXIT_NMI_WINDOW:
    case VMX_EXIT_PAUSE:
        out->kind = HV_EXIT_INTR;
        return 0;
    case VMX_EXIT_HLT:
        skip_instruction(v, v->exit_instr_len);
        out->kind = HV_EXIT_HLT;
        return 0;
    case VMX_EXIT_CPUID:
        out->kind = HV_EXIT_CPUID;
        return 0;
    case VMX_EXIT_INVD:
    case VMX_EXIT_WBINVD:
        skip_instruction(v, v->exit_instr_len);   /* a no-op: the guest's caches are coherent */
        out->kind = HV_EXIT_INTR;
        return 0;
    case VMX_EXIT_IO: {
        struct vmx_io io = vmx_decode_io(v->exit_qual);
        out->kind = HV_EXIT_IO;
        out->io.port = io.port;
        out->io.size = io.size;
        out->io.write = !io.in;
        out->io.string = io.string;
        out->io.rep = io.rep;
        out->io.next_rip = v->st.rip + v->exit_instr_len;
        return 0;
    }
    case VMX_EXIT_RDMSR:
    case VMX_EXIT_WRMSR:
        out->kind = HV_EXIT_MSR;
        out->msr.index = (uint32_t)v->gprs.rcx;
        out->msr.write = reason == VMX_EXIT_WRMSR;
        return 0;
    case VMX_EXIT_VMCALL:
        skip_instruction(v, v->exit_instr_len);
        out->kind = HV_EXIT_HYPERCALL;
        return 0;
    case VMX_EXIT_EPT_VIOLATION:
        out->kind = HV_EXIT_MMIO;
        out->mmio.gpa = v->exit_gpa;
        out->mmio.write = (v->exit_qual & (1u << 1)) != 0;
        return 0;
    case VMX_EXIT_TRIPLE_FAULT:
        out->kind = HV_EXIT_SHUTDOWN;
        return 0;
    case VMX_EXIT_XSETBV:
        /* The guest's XCR0 is validated by the same rule as on SVM. */
        {
            uint64_t want = (v->gprs.rdx << 32) | (uint32_t)v->gprs.rax;
            uint64_t host = arch_hv_host_xstate();
            if (v->gprs.rcx != 0 || (want & ~host) || !(want & XCR0_X87) ||
                ((want & XCR0_AVX) && !(want & XCR0_SSE))) {
                vmx_be_vcpu_inject_exception(v, 13, true, 0);   /* #GP */
            } else {
                v->guest_xcr0 = want;
                skip_instruction(v, v->exit_instr_len);
            }
        }
        out->kind = HV_EXIT_INTR;
        return 0;
    case VMX_EXIT_VMCLEAR:
    case VMX_EXIT_VMLAUNCH:
    case VMX_EXIT_VMPTRLD:
    case VMX_EXIT_VMPTRST:
    case VMX_EXIT_VMREAD:
    case VMX_EXIT_VMRESUME:
    case VMX_EXIT_VMWRITE:
    case VMX_EXIT_VMXOFF:
    case VMX_EXIT_VMXON:
    case VMX_EXIT_INVEPT:
    case VMX_EXIT_MWAIT:
    case VMX_EXIT_MONITOR:
    case VMX_EXIT_GDTR_IDTR:
    case VMX_EXIT_LDTR_TR:
        /* The guest is not a hypervisor and has no MONITOR: #UD. */
        vmx_be_vcpu_inject_exception(v, 6, false, 0);
        out->kind = HV_EXIT_INTR;
        return 0;
    default:
        out->kind = HV_EXIT_FAIL;
        out->fail.code = reason;
        out->fail.info1 = v->exit_qual;
        out->fail.info2 = v->exit_intr_info;
        if (v->unknown_exits++ == 0)
            kwarn("vmx: vpid %u: exit reason %u qualification 0x%llx rip 0x%llx", v->vm->vpid, reason,
                  (unsigned long long)v->exit_qual, (unsigned long long)v->st.rip);
        return 0;
    }
}

static int vmx_be_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out)
{
    const struct x86_fpu_info *fi = x86_fpu_info();
    arch_irq_state_t s = arch_irq_save();
    struct vmx_cpu *c = enable_this_cpu();
    if (c == NULL) {
        arch_irq_restore(s);
        return -ENOMEM;
    }
    int cpu = (int)this_cpu()->cpu_id;
    if (v->loaded_cpu != cpu) {
        /* Moving a VMCS between CPUs: clear it where it was current, and
         * launch rather than resume on the new one. */
        if (v->loaded_cpu >= 0)
            vmclear(v->vmcs_pa);
        if (!vmptrld(v->vmcs_pa)) {
            arch_irq_restore(s);
            kerror("vmx: VMPTRLD refused on CPU %d", cpu);
            return -EIO;
        }
        v->loaded_cpu = cpu;
        v->launched = false;
        write_controls(v);
    }
    write_host_state();   /* the CPU may have changed CR3 or a base since the last run */
    load_guest_state(v);
    inject_pending(v);

    /* The guest rule of arch/fpu.h, identical to SVM's. */
    bool owner = x86_fpu_save_current();
    x86_fpu_area_restore(v->fpu);
    if (fi->xsave)
        xsetbv(0, v->guest_xcr0);
    uint32_t failed = vmx_run(&v->gprs, v->launched ? 1u : 0u);
    if (fi->xsave)
        xsetbv(0, fi->xcr0);
    x86_fpu_area_save(v->fpu);
    if (!owner || !x86_fpu_restore_current())
        x86_fpu_area_restore(x86_fpu_reset_image());

    if (failed) {
        uint64_t err = vmread(VMCS_INSTRUCTION_ERROR);
        arch_irq_restore(s);
        out->kind = HV_EXIT_FAIL;
        out->fail.code = 0;
        out->fail.info1 = err;
        out->fail.info2 = 0;
        kerror("vmx: entry failed, VM-instruction error %llu", (unsigned long long)err);
        return 0;
    }
    v->launched = true;
    v->exit_reason = (uint32_t)vmread(VMCS_EXIT_REASON);
    v->exit_qual = vmread(VMCS_EXIT_QUALIFICATION);
    v->exit_instr_len = (uint32_t)vmread(VMCS_EXIT_INSTR_LEN);
    v->exit_intr_info = (uint32_t)vmread(VMCS_EXIT_INTR_INFO);
    v->exit_gpa = vmread(VMCS_GUEST_PHYS_ADDR);
    save_guest_state(v);
    /* An external interrupt exits with the host's IDT not yet consulted:
     * restoring the flags below runs the host handler. */
    arch_irq_restore(s);
    return decode_exit(v, out);
}

const struct hv_backend vmx_backend = {
    .probe = vmx_be_probe,
    .vm_create = vmx_be_vm_create,
    .vm_destroy = vmx_be_vm_destroy,
    .vm_map = vmx_be_vm_map,
    .vm_unmap = vmx_be_vm_unmap,
    .vm_query = vmx_be_vm_query,
    .vcpu_create = vmx_be_vcpu_create,
    .vcpu_destroy = vmx_be_vcpu_destroy,
    .vcpu_get_state = vmx_be_vcpu_get_state,
    .vcpu_set_state = vmx_be_vcpu_set_state,
    .vcpu_run = vmx_be_vcpu_run,
    .vcpu_set_irq = vmx_be_vcpu_set_irq,
    .vcpu_irq_taken = vmx_be_vcpu_irq_taken,
    .vcpu_inject_exception = vmx_be_vcpu_inject_exception,
    .vcpu_advance_rip = vmx_be_vcpu_advance_rip,
    .vcpu_set_rip = vmx_be_vcpu_set_rip,
    .vcpu_rip = vmx_be_vcpu_rip,
    .vcpu_guest_efer = vmx_be_vcpu_guest_efer,
    .vcpu_set_guest_efer = vmx_be_vcpu_set_guest_efer,
    .vcpu_msr = vmx_be_vcpu_msr,
    .vcpu_xstate_enabled = vmx_be_vcpu_xstate_enabled,
    .vcpu_write_rax = vmx_be_vcpu_write_rax,
    .vcpu_read_gpr = vmx_be_vcpu_read_gpr,
    .vcpu_write_gpr = vmx_be_vcpu_write_gpr,
};
