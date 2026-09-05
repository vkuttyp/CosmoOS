/*
 * hvsys.c - The virtualization system calls
 * (docs/kernel-services/virtualization/design.md, "The system calls").
 *
 * Handles carry VM and vCPU kobjects; every call validates the object
 * type and the rights. vm_create needs a handle to /dev/vmm opened for
 * writing: the node's mode (0600) is the policy.
 */

#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/hv.h>
#include <kernel/kmalloc.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

static struct vm *vm_of(int h, unsigned rights)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, rights);
    if (obj == NULL)
        return NULL;
    struct vm *vm = vm_from_kobject(obj);
    if (vm == NULL)
        kobject_put(obj);
    return vm;
}

static struct vcpu *vcpu_of(int h, unsigned rights)
{
    struct kobject *obj = handle_lookup(&process_current()->handles, h, rights);
    if (obj == NULL)
        return NULL;
    struct vcpu *v = vcpu_from_kobject(obj);
    if (v == NULL)
        kobject_put(obj);
    return v;
}

int64_t sys_vm_create(struct syscall_args *a)
{
    struct process *p = process_current();
    struct kobject *obj = handle_lookup(&p->handles, (int)a->a[0], HANDLE_RIGHT_WRITE);
    if (obj == NULL)
        return -EBADF;
    struct file *f = file_from_kobject(obj);
    bool ok = f != NULL && hv_is_vmm_vnode(f->vn) && (f->flags & COSMO_O_ACCMODE) != COSMO_O_RDONLY;
    kobject_put(obj);
    if (!ok)
        return -EPERM;
    struct vm *vm;
    int rc = vm_create(p->cred.uid, &vm);
    if (rc)
        return rc;
    int h = handle_install(&p->handles, &vm->obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    kobject_put(&vm->obj);
    return h;
}

int64_t sys_vm_mem(struct syscall_args *a)
{
    struct vm *vm = vm_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (vm == NULL)
        return -EBADF;
    int rc = vm_mem_add(vm, a->a[1], a->a[2]);
    kobject_put(&vm->obj);
    return rc;
}

int64_t sys_vm_mem_rw(struct syscall_args *a)
{
    uint64_t gpa = a->a[1], ubuf = a->a[2];
    size_t len = (size_t)a->a[3];
    bool write = a->a[4] != 0;
    if (len > (64u << 20))
        return -EINVAL;
    if (!user_range_ok(ubuf, len))
        return -EFAULT;
    struct vm *vm = vm_of((int)a->a[0], write ? HANDLE_RIGHT_WRITE : HANDLE_RIGHT_READ);
    if (vm == NULL)
        return -EBADF;
    uint8_t *bounce = kmalloc(PAGE_SIZE, 0);
    if (bounce == NULL) {
        kobject_put(&vm->obj);
        return -ENOMEM;
    }
    int rc = 0;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < PAGE_SIZE ? len - done : PAGE_SIZE;
        if (write) {
            if (copy_from_user(bounce, ubuf + done, n)) {
                rc = -EFAULT;
                break;
            }
            rc = vm_mem_write(vm, gpa + done, bounce, n);
        } else {
            rc = vm_mem_read(vm, gpa + done, bounce, n);
            if (rc == 0 && copy_to_user(ubuf + done, bounce, n))
                rc = -EFAULT;
        }
        if (rc)
            break;
        done += n;
    }
    kfree(bounce);
    kobject_put(&vm->obj);
    return rc ? rc : (int64_t)done;
}

int64_t sys_vcpu_create(struct syscall_args *a)
{
    struct vm *vm = vm_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (vm == NULL)
        return -EBADF;
    struct vcpu *v;
    int rc = vcpu_create(vm, (unsigned)a->a[1], &v);
    kobject_put(&vm->obj);
    if (rc)
        return rc;
    int h = handle_install(&process_current()->handles, &v->obj, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    kobject_put(&v->obj);
    return h;
}

int64_t sys_vcpu_regs(struct syscall_args *a)
{
    bool set = a->a[2] != 0;
    if (!user_range_ok(a->a[1], sizeof(struct cosmo_vcpu_regs)))
        return -EFAULT;
    struct vcpu *v = vcpu_of((int)a->a[0], set ? HANDLE_RIGHT_WRITE : HANDLE_RIGHT_READ);
    if (v == NULL)
        return -EBADF;
    struct cosmo_vcpu_regs regs;
    int rc;
    if (set) {
        rc = copy_from_user(&regs, a->a[1], sizeof(regs)) ? -EFAULT : vcpu_set_regs(v, &regs);
    } else {
        rc = vcpu_get_regs(v, &regs);
        if (rc == 0 && copy_to_user(a->a[1], &regs, sizeof(regs)))
            rc = -EFAULT;
    }
    kobject_put(&v->obj);
    return rc;
}

int64_t sys_vcpu_run(struct syscall_args *a)
{
    if (!user_range_ok(a->a[1], sizeof(struct cosmo_vm_exit)))
        return -EFAULT;
    struct vcpu *v = vcpu_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (v == NULL)
        return -EBADF;
    struct cosmo_vm_exit x;
    if (copy_from_user(&x, a->a[1], sizeof(x))) {
        kobject_put(&v->obj);
        return -EFAULT;
    }
    int rc = vcpu_run(v, &x);
    if (rc == 0 && copy_to_user(a->a[1], &x, sizeof(x)))
        rc = -EFAULT;
    kobject_put(&v->obj);
    return rc;
}

int64_t sys_vcpu_irq(struct syscall_args *a)
{
    struct vcpu *v = vcpu_of((int)a->a[0], HANDLE_RIGHT_WRITE);
    if (v == NULL)
        return -EBADF;
    int rc = vcpu_inject(v, (unsigned)a->a[1]);
    kobject_put(&v->obj);
    return rc;
}
