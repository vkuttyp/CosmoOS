/*
 * object.c - Reference-counted kernel object header.
 */

#include <kernel/object.h>
#include <kernel/panic.h>

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    KASSERT(type != NULL && type->release != NULL);
    obj->type = type;
    obj->refcount = 1;
}

void kobject_get(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_add(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_get on a released %s object %p", obj->type->name, (void *)obj);
}

void kobject_put(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_sub(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_put underflow on %s object %p", obj->type->name, (void *)obj);
    if (old == 1)
        obj->type->release(obj);
}

uint32_t kobject_refcount(const struct kobject *obj)
{
    return __atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE);
}
