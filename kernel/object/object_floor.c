/*
 * object_floor.c - The C under kernel/object/object.nova (roadmap 2.0 SA2).
 *
 * The object layer's counts, release, discard and census are Nova
 * (object.nova, generated into object.c). What stays here is what Nova
 * cannot express yet: calls through the type's function pointers (an
 * imported C struct cannot declare a function-pointer member, Nova
 * type-system.md §31.1), panic's message (a variadic call), and the
 * module exports (a C macro). Each is a few lines and no decision.
 */

#include <kernel/errno.h>
#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/panic.h>

#include <uapi/cosmo/syscall.h>

/* Declared for object.nova only; not part of object.h's interface. */
void kobject_floor_release(struct kobject *obj);
uintptr_t kobject_floor_release_address(const struct kobject_type *type);
void kobject_floor_fatal(const char *before, const char *after, const struct kobject *obj);

void kobject_floor_release(struct kobject *obj)
{
    obj->type->release(obj);
}

uintptr_t kobject_floor_release_address(const struct kobject_type *type)
{
    return type == NULL ? 0 : (uintptr_t)type->release;
}

void kobject_floor_fatal(const char *before, const char *after, const struct kobject *obj)
{
    panic("%s %s object %p%s", before, obj->type != NULL ? obj->type->name : "untyped", (const void *)obj, after);
}

/* Module ABI exports (docs/kernel/module/api.md). */
EXPORT_SYMBOL(kobject_init);
EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_tryget);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_put_and_lock);
EXPORT_SYMBOL(kobject_release_final);
EXPORT_SYMBOL(kobject_refcount);

/* --- I/O dispatch through the type's operations (function pointers) --- */

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
