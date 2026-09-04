/*
 * object.h - Kernel object header: type and reference count.
 *
 * Embed `struct kobject` in an object and recover the object with
 * container_of. kobject_get/put are atomic and usable in any context;
 * the type's release runs from the last put, outside any spinlock the
 * caller holds (callers must not hold one), and may block.
 */

#ifndef KERNEL_OBJECT_H
#define KERNEL_OBJECT_H

#include <kernel/compiler.h>

struct kobject;

struct kobject_type {
    const char *name;
    void (*release)(struct kobject *obj);
};

struct kobject {
    const struct kobject_type *type;
    uint32_t refcount;
};

void kobject_init(struct kobject *obj, const struct kobject_type *type);
void kobject_get(struct kobject *obj);
void kobject_put(struct kobject *obj);
uint32_t kobject_refcount(const struct kobject *obj);

/* --- console object: standard I/O until the VFS exists --- */

struct kobject_io_type {
    struct kobject_type base;
    int64_t (*read)(struct kobject *obj, void *buf, size_t len);
    int64_t (*write)(struct kobject *obj, const void *buf, size_t len);
};

/* The single console object; kobject_get before installing in a table. */
struct kobject *console_object(void);

#endif /* KERNEL_OBJECT_H */
