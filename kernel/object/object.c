/*
 * object.c - Reference-counted kernel object header.
 */

#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/errno.h>
#include <kernel/panic.h>

#include <uapi/cosmo/syscall.h>

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

const struct kobject_io_type *kobject_io_of(const struct kobject *obj)
{
    if (obj == NULL || !(obj->type->flags & KOBJECT_TYPE_IO))
        return NULL;
    return (const struct kobject_io_type *)obj->type;
}

unsigned kobject_ready(struct kobject *obj)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL)
        return 0;
    if (io->ready == NULL)
        return COSMO_IO_READABLE | COSMO_IO_WRITABLE;
    return io->ready(obj);
}

struct waitqueue *kobject_poll_wq(struct kobject *obj, unsigned events)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->poll_wq == NULL)
        return NULL;
    return io->poll_wq(obj, events);
}

int kobject_set_nonblock(struct kobject *obj, int on)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io == NULL || io->set_nonblock == NULL)
        return -EOPNOTSUPP;
    return io->set_nonblock(obj, on);
}
