/*
 * object.c - Reference-counted kernel object header.
 */

#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/panic.h>

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    KASSERT(type != NULL && type->release != NULL);
    obj->type = type;
    obj->refcount = 1;
    /* The owner's live-object count is taken inside module_owner_of's
     * read section, so the module cannot be freed between the lookup and
     * the increment (docs/kernel/quiesce/design.md, "Module unload"). */
    obj->owner = module_owner_of((uintptr_t)type->release);
}

void kobject_track_code(struct kobject *obj, uintptr_t code)
{
    struct module *m = module_owner_of(code);
    if (obj->owner)
        module_object_released(obj->owner);
    obj->owner = m;
}

void kobject_get(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_add(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_get on a released %s object %p", obj->type->name, (void *)obj);
}

bool kobject_tryget(struct kobject *obj)
{
    uint32_t cur = __atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur == 0)
            return false;
        if (__atomic_compare_exchange_n(&obj->refcount, &cur, cur + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}

void kobject_put(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_sub(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_put underflow on %s object %p", obj->type->name, (void *)obj);
    if (old == 1) {
        /* The release frees the object: read the owner first. The owner's
         * count is dropped after the release returned, so the module's
         * text stays mapped while its release code runs. */
        struct module *owner = obj->owner;
        obj->type->release(obj);
        if (owner)
            module_object_released(owner);
    }
}

uint32_t kobject_refcount(const struct kobject *obj)
{
    return __atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE);
}

/* Module ABI exports (docs/kernel/module/api.md). */
EXPORT_SYMBOL(kobject_init);
EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_tryget);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_refcount);
