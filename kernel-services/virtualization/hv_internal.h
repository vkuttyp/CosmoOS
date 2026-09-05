/* hv_internal.h - Shared between the files of kernel-services/virtualization/. */

#ifndef HV_INTERNAL_H
#define HV_INTERNAL_H

#include <kernel/hv.h>

/* vmm.c */
int hv_register_vm(struct vm *vm);
void hv_unregister_vm(struct vm *vm);

/* guestmem.c */
void guestmem_release(struct vm *vm);

/* vmdev.c */
void vmdev_init(struct vm *vm);
/* 0 handled, -ENODEV no device claims the port. */
int vmdev_pio(struct vm *vm, uint16_t port, bool write, unsigned size, uint32_t *value);
void vmdev_mmio(struct vm *vm, uint64_t gpa, bool write);
void vm_console_put(struct vm *vm, const uint8_t *bytes, size_t n);

/* vintr.c */
void vintr_init(struct vcpu *v);
int vintr_take_lowest(struct vcpu *v);        /* the vector to offer, -1 none (not cleared) */
void vintr_clear(struct vcpu *v, int vector);
bool vintr_any(struct vcpu *v);

/* vcpu.c */
void vcpu_emulate_cpuid(struct vcpu *v);
/* 0 handled (rip advanced), -ENOENT: #GP was injected. */
int vcpu_emulate_msr(struct vcpu *v, uint32_t index, bool write);

#endif /* HV_INTERNAL_H */
