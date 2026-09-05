/*
 * object.h - Kernel object header: type, reference count, owner.
 *
 * Embed `struct kobject` in an object and recover the object with
 * container_of. kobject_get/put are atomic and usable in any context;
 * the type's release runs from the last put, outside any spinlock the
 * caller holds (callers must not hold one), and may block.
 *
 * Lifetime rules (docs/kernel/quiesce/design.md, "Kernel objects"):
 *   - every type has a release; the release frees the object's memory
 *     and nothing else does. An object embedded in a caller-owned block
 *     still needs a release (it may be empty for static objects);
 *   - the creator holds the first reference; a registry that hands the
 *     object out by lookup holds its own and drops it on unregister;
 *   - a lookup returns a referenced pointer, never a borrowed one;
 *   - the owner module of the release code is recorded so the module
 *     cannot be unloaded while an object still points into it.
 */

#ifndef KERNEL_OBJECT_H
#define KERNEL_OBJECT_H

#include <kernel/compiler.h>

struct kobject;

struct kobject_type {
    const char *name;
    void (*release)(struct kobject *obj);
    unsigned flags;                       /* KOBJECT_TYPE_IO: the type is a struct kobject_io_type */
};

#define KOBJECT_TYPE_IO 1u

struct module;

struct kobject {
    const struct kobject_type *type;
    uint32_t refcount;
    struct module *owner;   /* module whose code the release lives in, or NULL for the kernel */
};

/* Reference 1 belongs to the caller. Records the owner of type->release. */
void kobject_init(struct kobject *obj, const struct kobject_type *type);
void kobject_get(struct kobject *obj);
/* Take a reference unless the count is already zero (the release is
 * running or about to). For lookups from tables the release path clears
 * under a lock the looker holds: a plain get would panic. */
bool kobject_tryget(struct kobject *obj);
void kobject_put(struct kobject *obj);
uint32_t kobject_refcount(const struct kobject *obj);

/* Types whose release trampolines to a per-object callback (device,
 * blkdev, netif) record the callback's owner here, once the callback is
 * known. `code` is the callback's address. */
void kobject_track_code(struct kobject *obj, uintptr_t code);

/* --- console object: standard I/O until the VFS exists --- */

/*
 * read/write return the byte count transferred (0 <= count <= len) or a
 * negative errno. A count above len is an object bug: the callers
 * (sys_read/sys_write) assert on it and fail the call with -EIO, they
 * never trust it to size a copy.
 */
struct cosmo_stat;
struct kobject_io_type {
    struct kobject_type base;
    int64_t (*read)(struct kobject *obj, void *buf, size_t len);
    int64_t (*write)(struct kobject *obj, const void *buf, size_t len);
    int (*stat)(struct kobject *obj, struct cosmo_stat *st);   /* optional: fstat on the object */
    /* Optional. COSMO_IO_READABLE/WRITABLE/HANGUP/ERROR bits describing what
     * would not block now; NULL means always readable and writable. Any
     * thread context; never blocks. */
    unsigned (*ready)(struct kobject *obj);
    /* Optional. Switch the object's non-blocking mode (a property of the
     * object, shared by every handle to it): `on` 0 or 1 sets it, -1 only
     * asks. Returns the previous mode (0 or 1). NULL means -EOPNOTSUPP. */
    int (*set_nonblock)(struct kobject *obj, int on);
    /* Optional. The wait queue a waiter for `events` (COSMO_IO_* bits)
     * should sleep on; it is woken whenever `ready` may have changed for
     * those bits. NULL means readiness never changes (a file). */
    struct waitqueue *(*poll_wq)(struct kobject *obj, unsigned events);
};

struct waitqueue;

/* The object's io type, or NULL when it has none (a plain kobject such as
 * a vcpu): the system-call layer refuses I/O on those with -EBADF. */
const struct kobject_io_type *kobject_io_of(const struct kobject *obj);
/* Readiness through the type: the io type's ready, or always ready; a
 * plain kobject is never ready. */
unsigned kobject_ready(struct kobject *obj);
int kobject_set_nonblock(struct kobject *obj, int on);   /* -1 asks; returns the previous mode */
struct waitqueue *kobject_poll_wq(struct kobject *obj, unsigned events);   /* NULL: never changes */

/* The single console object; kobject_get before installing in a table. */
struct kobject *console_object(void);

#endif /* KERNEL_OBJECT_H */
