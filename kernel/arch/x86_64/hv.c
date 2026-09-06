/*
 * hv.c - Backend selection for x86-64 virtualization
 * (docs/kernel-services/virtualization/design.md).
 *
 * AMD-V and VT-x are never both present, so the probe takes whichever
 * the CPU has and every entry point of arch/hv.h forwards to it. With
 * neither, `caps.present` is false and no other entry point is ever
 * reached: the generic layer refuses to create a VM.
 */

#include <kernel/errno.h>
#include <kernel/log.h>

#include <arch/hv.h>
#include <x86/cpu.h>
#include <x86/fpu.h>
#include <x86/hvops.h>

static const struct hv_backend *g_be;

int arch_hv_probe(struct hv_caps *out)
{
    static const struct hv_backend *const backends[] = { &svm_backend, &vmx_backend };
    for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        struct hv_caps caps;
        if (backends[i]->probe(&caps) == 0 && caps.present) {
            g_be = backends[i];
            *out = caps;
            return 0;
        }
    }
    out->present = false;
    out->name = "none";
    out->max_asids = 0;
    out->nested_paging = false;
    out->real_mode_guest = false;
    out->map_prot = false;
    out->large_pages = false;
    out->max_vcpus = 0;
    return -ENOTSUP;
}

/* Every call below happens only after a successful probe: the manager
 * refuses everything when caps.present is false (invariant V2). */

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
bool arch_hv_vcpu_irq_taken(struct arch_hv_vcpu *v) { return g_be->vcpu_irq_taken(v); }
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

/* The two host queries are the CPU's, not a backend's. */

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

/* The extended-state components the host lets a guest enable. */
uint64_t arch_hv_host_xstate(void)
{
    const struct x86_fpu_info *fi = x86_fpu_info();
    return fi->xsave ? fi->xcr0 : 0;
}
