/*
 * hv.c - Backend selection for AArch64 virtualization
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend").
 *
 * One backend today: EL2, when the loader kept it. Without it the probe
 * reports `none` and the generic layer refuses to create a VM.
 */

#include <kernel/errno.h>
#include <kernel/log.h>

#include <kernel/bootinfo.h>
#include <arch/el2.h>
#include <arch/hv.h>
#include <arch/hv_backend.h>
#include <aarch64/hv_el2.h>
#include <aarch64/sysreg.h>

static const struct hv_backend *g_be;

int arch_hv_probe(struct hv_caps *out)
{
    el2_init(bootinfo_get());   /* the SMP probe may not have run yet */
    struct hv_caps caps;
    if (el2_backend.probe(&caps) == 0 && caps.present) {
        g_be = &el2_backend;
        *out = caps;
        return 0;
    }
    out->present = false;
    out->name = "none";
    out->max_asids = 0;
    out->nested_paging = false;
    out->real_mode_guest = false;
    out->map_prot = false;
    out->large_pages = false;
    out->max_vcpus = 0;
    out->inject_irq = false;
    return -ENOTSUP;
}

/* Every call below happens only after a successful probe: the manager
 * refuses everything when caps.present is false (invariant V2). */

void arch_hv_vintr_range(unsigned *lo, unsigned *hi)
{
    /* Every INTID a list register can carry. SGIs (0..15) and PPIs
     * (16..31) are private to a CPU and are what a guest's own software
     * uses; SPIs run to 1019. LPIs are above 8192 and need an ITS
     * translation nothing here can give a guest. */
    *lo = 0;
    *hi = HV_VINTR_MAX;
}

/* Not a backend op: only this architecture has a virtual GIC, and only
 * one backend here can drive one. */
bool arch_hv_vcpu_vgic_state(struct arch_hv_vcpu *v, uint64_t *lr0, uint64_t *elrsr)
{
    return el2_vcpu_vgic_state(v, lr0, elrsr);
}

bool arch_hv_vcpu_timer_state(struct arch_hv_vcpu *v, uint64_t *ctl, uint64_t *cntvoff)
{
    return el2_vcpu_timer_state(v, ctl, cntvoff);
}

uint64_t arch_hv_vcpu_host_vtimer_after(struct arch_hv_vcpu *v)
{
    return el2_vcpu_host_cntv_after(v);
}

int arch_hv_vm_create(struct arch_hv_vm **out) { return g_be->vm_create(out); }
void arch_hv_vm_destroy(struct arch_hv_vm *vm) { g_be->vm_destroy(vm); }
int arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot)
{
    return g_be->vm_map(vm, gpa, hpa, len, prot);
}
int arch_hv_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len) { return g_be->vm_unmap(vm, gpa, len); }
bool arch_hv_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa) { return g_be->vm_query(vm, gpa, hpa); }

int arch_hv_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out) { return g_be->vcpu_create(vm, out); }
void arch_hv_vcpu_destroy(struct arch_hv_vcpu *v) { g_be->vcpu_destroy(v); }
void arch_hv_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *o) { g_be->vcpu_get_state(v, o); }
int arch_hv_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *i)
{
    return g_be->vcpu_set_state(v, i);
}
int arch_hv_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out) { return g_be->vcpu_run(v, out); }
void arch_hv_vcpu_set_irq(struct arch_hv_vcpu *v, int vector) { g_be->vcpu_set_irq(v, vector); }
int arch_hv_vcpu_irq_delivered(struct arch_hv_vcpu *v) { return g_be->vcpu_irq_delivered(v); }
bool arch_hv_vcpu_timer_expired(struct arch_hv_vcpu *v) { return g_be->vcpu_timer_expired(v); }
bool arch_hv_vcpu_timer_deadline(struct arch_hv_vcpu *v, uint64_t *t) { return g_be->vcpu_timer_deadline(v, t); }
unsigned arch_hv_guest_timer_intid(void) { return el2_guest_timer_intid(); }
void arch_hv_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error)
{
    g_be->vcpu_inject_exception(v, vector, has_error, error);
}
void arch_hv_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes) { g_be->vcpu_advance_rip(v, bytes); }
void arch_hv_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip) { g_be->vcpu_set_rip(v, rip); }
uint64_t arch_hv_vcpu_rip(struct arch_hv_vcpu *v) { return g_be->vcpu_rip(v); }
uint64_t arch_hv_vcpu_guest_efer(struct arch_hv_vcpu *v) { return g_be->vcpu_guest_efer(v); }
int arch_hv_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer) { return g_be->vcpu_set_guest_efer(v, efer); }
int arch_hv_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value)
{
    return g_be->vcpu_msr(v, index, write, value);
}
bool arch_hv_vcpu_xstate_enabled(struct arch_hv_vcpu *v) { return g_be->vcpu_xstate_enabled(v); }
void arch_hv_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size)
{
    g_be->vcpu_write_rax(v, value, size);
}
uint64_t arch_hv_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index) { return g_be->vcpu_read_gpr(v, index); }
void arch_hv_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value)
{
    g_be->vcpu_write_gpr(v, index, value);
}

/* Host queries the emulation filters. AArch64 has no CPUID; the ID
 * registers are read through the trap path instead, so this is only the
 * counter. */

void arch_hv_host_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                        uint32_t *edx)
{
    (void)leaf;
    (void)subleaf;
    *eax = *ebx = *ecx = *edx = 0;
}

uint64_t arch_hv_host_tsc(void)
{
    return READ_SYSREG(cntvct_el0);
}

uint64_t arch_hv_host_xstate(void)
{
    return 0;   /* no XSAVE here: FP state is the guest's CPACR business */
}
