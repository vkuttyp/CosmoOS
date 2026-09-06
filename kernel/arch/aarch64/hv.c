/*
 * hv.c - No hardware virtualization backend on AArch64 in this phase
 * (docs/kernel/arch/aarch64/design.md, "Stubs and exclusions"). An EL2
 * backend with stage-2 translation is a later phase; until then the VM
 * manager reports `none` and refuses to create VMs.
 */

#include <kernel/errno.h>
#include <kernel/string.h>
#include <arch/hv.h>

static const struct hv_caps g_none = { .present = false, .name = "none", .max_asids = 0, .nested_paging = false };

int arch_hv_probe(struct hv_caps *out)
{
    *out = g_none;
    return -ENOTSUP;
}

int arch_hv_vm_create(struct arch_hv_vm **out) { (void)out; return -ENOTSUP; }
void arch_hv_vm_destroy(struct arch_hv_vm *vm) { (void)vm; }
int arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot)
{
    (void)vm; (void)gpa; (void)hpa; (void)len; (void)prot;
    return -ENOTSUP;
}
int arch_hv_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len) { (void)vm; (void)gpa; (void)len; return -ENOTSUP; }
bool arch_hv_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa) { (void)vm; (void)gpa; (void)hpa; return false; }

int arch_hv_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out) { (void)vm; (void)out; return -ENOTSUP; }
void arch_hv_vcpu_destroy(struct arch_hv_vcpu *v) { (void)v; }
void arch_hv_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *out) { (void)v; memset(out, 0, sizeof(*out)); }
int arch_hv_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *in) { (void)v; (void)in; return -ENOTSUP; }
int arch_hv_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out) { (void)v; memset(out, 0, sizeof(*out)); return -ENOTSUP; }
void arch_hv_vcpu_set_irq(struct arch_hv_vcpu *v, int vector) { (void)v; (void)vector; }
bool arch_hv_vcpu_irq_taken(struct arch_hv_vcpu *v) { (void)v; return false; }
void arch_hv_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error)
{
    (void)v; (void)vector; (void)has_error; (void)error;
}
void arch_hv_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes) { (void)v; (void)bytes; }
void arch_hv_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip) { (void)v; (void)rip; }
uint64_t arch_hv_vcpu_rip(struct arch_hv_vcpu *v) { (void)v; return 0; }
void arch_hv_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size) { (void)v; (void)value; (void)size; }
uint64_t arch_hv_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index) { (void)v; (void)index; return 0; }
void arch_hv_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value) { (void)v; (void)index; (void)value; }
uint64_t arch_hv_vcpu_guest_efer(struct arch_hv_vcpu *v) { (void)v; return 0; }
int arch_hv_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer) { (void)v; (void)efer; return -ENOTSUP; }
int arch_hv_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value)
{
    (void)v; (void)index; (void)write; (void)value;
    return -ENOENT;
}
void arch_hv_host_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    (void)leaf; (void)subleaf;
    *eax = *ebx = *ecx = *edx = 0;
}
uint64_t arch_hv_host_tsc(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
uint64_t arch_hv_host_xstate(void) { return 0; }
bool arch_hv_vcpu_xstate_enabled(struct arch_hv_vcpu *v) { (void)v; return false; }
