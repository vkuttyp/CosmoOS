/*
 * arch/hv_backend.h - A hypervisor backend behind arch/hv.h.
 *
 * More than one implementation of the same interface can exist in one
 * kernel (x86-64: SVM and VMX; AArch64: EL2): `arch_hv_probe` picks the
 * one this machine has and every `arch_hv_*` entry point forwards
 * through this table. A machine with none leaves it NULL and the generic
 * layer sees `caps.present == false`.
 */

#ifndef ARCH_HV_BACKEND_H
#define ARCH_HV_BACKEND_H

#include <arch/hv.h>

struct hv_backend {
    int (*probe)(struct hv_caps *out);
    int (*vm_create)(struct arch_hv_vm **out);
    void (*vm_destroy)(struct arch_hv_vm *vm);
    int (*vm_map)(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot);
    int (*vm_unmap)(struct arch_hv_vm *vm, uint64_t gpa, size_t len);
    bool (*vm_query)(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa);
    int (*vcpu_create)(struct arch_hv_vm *vm, struct arch_hv_vcpu **out);
    void (*vcpu_destroy)(struct arch_hv_vcpu *v);
    void (*vcpu_get_state)(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *out);
    int (*vcpu_set_state)(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *in);
    int (*vcpu_run)(struct arch_hv_vcpu *v, struct hv_exit *out);
    void (*vcpu_set_irq)(struct arch_hv_vcpu *v, int vector);
    bool (*vcpu_irq_taken)(struct arch_hv_vcpu *v);
    void (*vcpu_inject_exception)(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error);
    void (*vcpu_advance_rip)(struct arch_hv_vcpu *v, unsigned bytes);
    void (*vcpu_set_rip)(struct arch_hv_vcpu *v, uint64_t rip);
    uint64_t (*vcpu_rip)(struct arch_hv_vcpu *v);
    uint64_t (*vcpu_guest_efer)(struct arch_hv_vcpu *v);
    int (*vcpu_set_guest_efer)(struct arch_hv_vcpu *v, uint64_t efer);
    int (*vcpu_msr)(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value);
    bool (*vcpu_xstate_enabled)(struct arch_hv_vcpu *v);
    void (*vcpu_write_rax)(struct arch_hv_vcpu *v, uint64_t value, unsigned size);
    uint64_t (*vcpu_read_gpr)(struct arch_hv_vcpu *v, unsigned index);
    void (*vcpu_write_gpr)(struct arch_hv_vcpu *v, unsigned index, uint64_t value);
};

#if defined(ARCH_X86_64)
extern const struct hv_backend svm_backend;
extern const struct hv_backend vmx_backend;
#elif defined(ARCH_AARCH64)
extern const struct hv_backend el2_backend;
#endif

#endif /* ARCH_HV_BACKEND_H */
