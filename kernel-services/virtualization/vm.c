/*
 * vm.c - struct vm: lifetime, the kobject, the owner's view
 * (docs/kernel-services/virtualization/design.md, "Ownership and lifetime").
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include "hv_internal.h"

static void vm_release(struct kobject *obj);

static int64_t vm_obj_read(struct kobject *obj, void *buf, size_t len)
{
    struct vm *vm = container_of(obj, struct vm, obj);
    return (int64_t)vm_console_read(vm, buf, len);
}

static int64_t vm_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    (void)obj;
    (void)buf;
    (void)len;
    return -ENOTSUP;
}

static int vm_obj_stat(struct kobject *obj, struct cosmo_stat *st)
{
    struct vm *vm = container_of(obj, struct vm, obj);
    memset(st, 0, sizeof(*st));
    st->ino = vm->id;
    st->type = COSMO_DT_CHR;
    st->mode = 0600;
    st->nlink = 1;
    st->uid = vm->owner_uid;
    st->size = vm_console_pending(vm);
    return 0;
}

static const struct kobject_io_type vm_type = {
    .base = { .name = "vm", .release = vm_release },
    .read = vm_obj_read,
    .write = vm_obj_write,
    .stat = vm_obj_stat,
};

struct vm *vm_from_kobject(struct kobject *obj)
{
    return obj != NULL && obj->type == &vm_type.base ? container_of(obj, struct vm, obj) : NULL;
}

int vm_create(uint32_t owner_uid, struct vm **out)
{
    if (!hv_caps()->present)
        return -ENOTSUP;
    struct vm *vm = kzalloc(sizeof(*vm));
    if (vm == NULL)
        return -ENOMEM;
    kobject_init(&vm->obj, &vm_type.base);
    mutex_init(&vm->lock, "vm");
    list_init(&vm->regions);
    list_init(&vm->devices);
    list_init(&vm->link);
    spinlock_init(&vm->console.lock, "vm-console");
    vm->owner_uid = owner_uid;
    int rc = arch_hv_vm_create(&vm->arch);
    if (rc) {
        kfree(vm);
        return rc;
    }
    vmdev_init(vm);
    rc = hv_register_vm(vm);
    if (rc) {
        arch_hv_vm_destroy(vm->arch);
        kfree(vm);
        return rc;
    }
    kdebug("hv: vm%u created (uid %u)", vm->id, owner_uid);
    *out = vm;
    return 0;
}

/* Last reference: no handle and no vCPU refers to this VM any more. */
static void vm_release(struct kobject *obj)
{
    struct vm *vm = container_of(obj, struct vm, obj);
    hv_unregister_vm(vm);
    guestmem_release(vm);
    arch_hv_vm_destroy(vm->arch);
    kdebug("hv: vm%u released", vm->id);
    kfree(vm);
}
