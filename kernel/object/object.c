/*
 * object.c - Reference-counted kernel object header.
 */

#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/errno.h>
#include <kernel/panic.h>

#include <uapi/cosmo/syscall.h>

/* The object census (roadmap 2.0 SA2): kobjects initialised and not yet
 * released. The self-test runner checks that every test leaves it where
 * it found it, and the fault-injection sweep that a failed creation leaks
 * none (docs/kernel/object/testing.md). */
static uint64_t g_kobjects_live;

/* The census by type: each type a slot, found or claimed on first use; a
 * type beyond the table is counted in the total only. */
static const struct kobject_type *g_census_type[KOBJECT_CENSUS_TYPES];
static int64_t g_census_count[KOBJECT_CENSUS_TYPES];
/* The type's name, copied when its slot is claimed: a module's type (and
 * its name) goes away when the module is unloaded, the slot does not. */
static char g_census_name[KOBJECT_CENSUS_TYPES][KOBJECT_CENSUS_NAME];

static int census_slot(const struct kobject_type *t)
{
    for (unsigned i = 0; i < KOBJECT_CENSUS_TYPES; i++) {
        const struct kobject_type *cur = __atomic_load_n(&g_census_type[i], __ATOMIC_ACQUIRE);
        if (cur == t)
            return (int)i;
        if (cur == NULL) {
            const struct kobject_type *expected = NULL;
            if (__atomic_compare_exchange_n(&g_census_type[i], &expected, t, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                const char *name = t->name ? t->name : "?";
                unsigned n = 0;
                for (; n + 1 < KOBJECT_CENSUS_NAME && name[n]; n++)
                    g_census_name[i][n] = name[n];
                g_census_name[i][n] = '\0';
                return (int)i;
            }
            if (expected == t)
                return (int)i;
        }
    }
    return -1;
}

static void census_count(const struct kobject_type *t, int64_t delta)
{
    int k = census_slot(t);
    if (k >= 0)
        __atomic_fetch_add(&g_census_count[k], delta, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&g_kobjects_live, (uint64_t)delta, __ATOMIC_ACQ_REL);
}

void kobject_census_by_type(int64_t out[KOBJECT_CENSUS_TYPES])
{
    for (unsigned i = 0; i < KOBJECT_CENSUS_TYPES; i++)
        out[i] = __atomic_load_n(&g_census_count[i], __ATOMIC_ACQUIRE);
}

const char *kobject_census_name(unsigned i)
{
    if (i >= KOBJECT_CENSUS_TYPES || __atomic_load_n(&g_census_type[i], __ATOMIC_ACQUIRE) == NULL)
        return NULL;
    return g_census_name[i];
}

uint64_t kobject_census(void)
{
    return __atomic_load_n(&g_kobjects_live, __ATOMIC_ACQUIRE);
}

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    KASSERT(type != NULL && type->release != NULL);
    obj->type = type;
    obj->refcount = 1;
    obj->handles = 0;
    obj->watched = 0;
    obj->watchers = NULL;
    /* The owner's live-object count is taken inside module_owner_of's
     * read section, so the module cannot be freed between the lookup and
     * the increment (docs/kernel/quiesce/design.md, "Module unload"). */
    obj->owner = module_owner_of((uintptr_t)type->release);
    census_count(type, 1);
}

void kobject_discard(struct kobject *obj)
{
    if (__atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE) != 1)
        panic("kobject_discard of a %s object %p someone else holds", obj->type->name, (void *)obj);
    obj->refcount = 0;
    if (obj->owner)
        module_object_released(obj->owner);
    obj->owner = NULL;
    census_count(obj->type, -1);
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

void kobject_get_n(struct kobject *obj, uint32_t n)
{
    if (n == 0)
        return;
    uint32_t old = __atomic_fetch_add(&obj->refcount, n, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_get_n on a released %s object %p", obj->type->name, (void *)obj);
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

void kobject_release_final(struct kobject *obj)
{
    if (__atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE) != 0)
        panic("kobject_release_final on a live %s object %p", obj->type->name, (void *)obj);
    /* The release frees the object: read the owner first. The owner's
     * count is dropped after the release returned, so the module's
     * text stays mapped while its release code runs. */
    struct module *owner = obj->owner;
    const struct kobject_type *type = obj->type;
    obj->type->release(obj);
    if (owner)
        module_object_released(owner);
    census_count(type, -1);   /* the type read before the release freed the object */
}

void kobject_put(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_sub(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("kobject_put underflow on %s object %p", obj->type->name, (void *)obj);
    if (old == 1)
        kobject_release_final(obj);
}

bool kobject_put_and_lock(struct kobject *obj, spinlock_t *lock, arch_irq_state_t *state)
{
    uint32_t cur = __atomic_load_n(&obj->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur == 0)
            panic("kobject_put_and_lock underflow on %s object %p", obj->type->name, (void *)obj);
        if (cur == 1)
            break;   /* ours may be the last: decide under the lock */
        /* Not the last: drop it without the lock. A concurrent dropper
         * from the same count fails this exchange, re-reads, and finds
         * itself at 1 -- which is the whole correction. */
        if (__atomic_compare_exchange_n(&obj->refcount, &cur, cur - 1, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return false;
    }
    *state = spin_lock_irqsave(lock);
    uint32_t old = __atomic_fetch_sub(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 1)
        return true;   /* zero, and it happened under the lock: the table is ours to clean */
    /* A lookup under this lock raised the count between our read and our
     * drop; not the last after all. */
    spin_unlock_irqrestore(lock, *state);
    return false;
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
EXPORT_SYMBOL(kobject_put_and_lock);
EXPORT_SYMBOL(kobject_release_final);
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
