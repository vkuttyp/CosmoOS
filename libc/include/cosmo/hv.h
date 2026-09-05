/*
 * cosmo/hv.h - Virtual machines (docs/kernel-services/virtualization/api.md).
 *
 * Thin wrappers over the vm_ and vcpu_ system calls. A VM handle is
 * obtained from vm_create with a handle to /dev/vmm open for writing;
 * reading the VM handle drains the guest's debug console (port 0xE9).
 */

#ifndef COSMO_HV_H
#define COSMO_HV_H

#include <cosmo/syscall.h>
#include <stddef.h>
#include <stdint.h>

static inline int cosmo_vm_create(int vmm_handle)
{
    return (int)cosmo_syscall1(SYS_vm_create, vmm_handle);
}

static inline int cosmo_vm_mem(int vm, uint64_t gpa, uint64_t len)
{
    return (int)cosmo_syscall3(SYS_vm_mem, vm, (long)gpa, (long)len);
}

static inline long cosmo_vm_mem_read(int vm, uint64_t gpa, void *buf, size_t len)
{
    return cosmo_syscall5(SYS_vm_mem_rw, vm, (long)gpa, (long)buf, (long)len, 0);
}

static inline long cosmo_vm_mem_write(int vm, uint64_t gpa, const void *buf, size_t len)
{
    return cosmo_syscall5(SYS_vm_mem_rw, vm, (long)gpa, (long)buf, (long)len, 1);
}

static inline int cosmo_vcpu_create(int vm, unsigned index)
{
    return (int)cosmo_syscall2(SYS_vcpu_create, vm, (long)index);
}

static inline int cosmo_vcpu_get_regs(int vcpu, struct cosmo_vcpu_regs *regs)
{
    return (int)cosmo_syscall3(SYS_vcpu_regs, vcpu, (long)regs, 0);
}

static inline int cosmo_vcpu_set_regs(int vcpu, const struct cosmo_vcpu_regs *regs)
{
    return (int)cosmo_syscall3(SYS_vcpu_regs, vcpu, (long)regs, 1);
}

/* Runs until an exit; `exit` carries an IN completion in (io.value) and the exit out. */
static inline int cosmo_vcpu_run(int vcpu, struct cosmo_vm_exit *exit)
{
    return (int)cosmo_syscall2(SYS_vcpu_run, vcpu, (long)exit);
}

static inline int cosmo_vcpu_irq(int vcpu, unsigned vector)
{
    return (int)cosmo_syscall2(SYS_vcpu_irq, vcpu, (long)vector);
}

#endif /* COSMO_HV_H */
