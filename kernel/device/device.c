/*
 * device.c - Bus/device/driver registry and probing.
 */

#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vmm.h>

/* The model lock is recursive on purpose: a bus driver's probe (running
 * under the lock) registers the child devices it discovers, and a
 * driver's remove unregisters them. Nesting is tracked per owning
 * thread; only the outermost unlock releases the mutex. */
static struct mutex g_device_mutex;
static struct thread *g_lock_owner;
static unsigned g_lock_depth;
static LIST_HEAD(g_buses);
static bool g_initialized;

static void model_lock(void)
{
    struct thread *self = thread_current();
    if (g_lock_owner == self) {
        g_lock_depth++;
        return;
    }
    mutex_lock(&g_device_mutex);
    g_lock_owner = self;
    g_lock_depth = 1;
}

static void model_unlock(void)
{
    KASSERT(g_lock_owner == thread_current() && g_lock_depth > 0);
    if (--g_lock_depth == 0) {
        g_lock_owner = NULL;
        mutex_unlock(&g_device_mutex);
    }
}

static void device_release(struct kobject *obj)
{
    struct device *dev = container_of(obj, struct device, obj);
    KASSERT(list_empty(&dev->bus_link));   /* unregistered before the last put */
    dev->release(dev);
}

void device_release_static(struct device *dev)
{
    (void)dev;
}

static const struct kobject_type device_type = {
    .name = "device",
    .release = device_release,
};

void device_init(void)
{
    KASSERT(!g_initialized);
    mutex_init(&g_device_mutex, "devices");
    g_initialized = true;
}

void bus_register(struct bus_type *bus)
{
    KASSERT(g_initialized && bus->name != NULL && bus->match != NULL);
    model_lock();
    struct bus_type *b;
    list_for_each_entry(b, &g_buses, link) {
        if (b == bus || strcmp(b->name, bus->name) == 0)
            panic("device: bus %s registered twice", bus->name);
    }
    list_init(&bus->devices);
    list_init(&bus->drivers);
    list_init(&bus->link);
    bus->nr_devices = 0;
    list_push_back(&g_buses, &bus->link);
    model_unlock();
    kdebug("device: bus %s registered", bus->name);
}

struct bus_type *bus_find(const char *name)
{
    model_lock();
    struct bus_type *b, *found = NULL;
    list_for_each_entry(b, &g_buses, link) {
        if (strcmp(b->name, name) == 0) {
            found = b;
            break;
        }
    }
    model_unlock();
    return found;
}

void device_setup(struct device *dev, struct bus_type *bus, struct device *parent, const char *name)
{
    memset(dev, 0, sizeof(*dev));
    kobject_init(&dev->obj, &device_type);
    strlcpy(dev->name, name, sizeof(dev->name));
    dev->bus = bus;
    dev->parent = parent;
    dev->dma_mask = 0xFFFFFFFFULL;
    dev->state = DEV_UNBOUND;
    list_init(&dev->bus_link);
}

int device_add_resource(struct device *dev, enum resource_type type, uint64_t start, uint64_t size,
                        unsigned flags)
{
    if (dev->nr_res == DEVICE_MAX_RESOURCES)
        return -ENOSPC;
    struct resource *r = &dev->res[dev->nr_res++];
    r->type = type;
    r->start = start;
    r->size = size;
    r->flags = flags;
    return 0;
}

const struct resource *device_resource(const struct device *dev, enum resource_type type, unsigned index)
{
    for (unsigned i = 0; i < dev->nr_res; i++) {
        if (dev->res[i].type != type)
            continue;
        if (index == 0)
            return &dev->res[i];
        index--;
    }
    return NULL;
}

/* Bind dev to drv if it matches. Returns true when the driver claimed
 * the device (even if probe failed). Lock held. */
static bool try_bind(struct device *dev, struct device_driver *drv)
{
    if (!drv->bus->match(dev, drv))
        return false;
    /* The driver is named before probe so a bus thunk can find its typed
     * driver through dev->driver instead of re-matching (several drivers
     * may match one device). */
    dev->driver = drv;
    int rc = drv->probe(dev);
    if (rc) {
        dev->driver = NULL;
        dev->state = DEV_FAILED;
        dev->probe_error = rc;
        dev->drvdata = NULL;
        kerror("device: %s: driver %s probe failed (%d)", dev->name, drv->name, rc);
        return true;
    }
    dev->state = DEV_BOUND;
    drv->bound++;
    kinfo("device: %s bound to %s", dev->name, drv->name);
    return true;
}

static void unbind(struct device *dev)
{
    struct device_driver *drv = dev->driver;
    if (drv == NULL)
        return;
    if (drv->remove)
        drv->remove(dev);
    KASSERT(drv->bound > 0);
    drv->bound--;
    dev->driver = NULL;
    dev->drvdata = NULL;
    dev->state = DEV_UNBOUND;
    kinfo("device: %s unbound from %s", dev->name, drv->name);
}

int device_register(struct device *dev)
{
    KASSERT(g_initialized && dev->bus != NULL);
    if (dev->release == NULL) {
        kerror("device: %s registered without a release", dev->name);
        return -EINVAL;
    }
    model_lock();
    struct device *d;
    list_for_each_entry(d, &dev->bus->devices, bus_link) {
        if (strcmp(d->name, dev->name) == 0) {
            model_unlock();
            return -EEXIST;   /* no owner count taken: the caller's failure path frees the storage directly */
        }
    }
    /* Accepted: record the release's owner module only now, so a failed
     * registration leaves nothing to balance. */
    kobject_track_code(&dev->obj, (uintptr_t)dev->release);
    list_push_back(&dev->bus->devices, &dev->bus_link);
    dev->bus->nr_devices++;
    kobject_get(&dev->obj);   /* the bus's reference */

    struct device_driver *drv;
    list_for_each_entry(drv, &dev->bus->drivers, bus_link) {
        if (try_bind(dev, drv))
            break;
    }
    model_unlock();
    return 0;
}

void device_unregister(struct device *dev)
{
    model_lock();
    unbind(dev);
    list_remove(&dev->bus_link);
    list_init(&dev->bus_link);
    dev->bus->nr_devices--;
    model_unlock();
    kobject_put(&dev->obj);
}

int driver_register(struct device_driver *drv)
{
    KASSERT(g_initialized && drv->bus != NULL && drv->probe != NULL && drv->name != NULL);
    model_lock();
    struct device_driver *d;
    list_for_each_entry(d, &drv->bus->drivers, bus_link) {
        if (d == drv) {
            model_unlock();
            return -EEXIST;
        }
    }
    list_init(&drv->bus_link);
    drv->bound = 0;
    list_push_back(&drv->bus->drivers, &drv->bus_link);

    struct device *dev;
    list_for_each_entry(dev, &drv->bus->devices, bus_link) {
        if (dev->state == DEV_UNBOUND)
            try_bind(dev, drv);
    }
    model_unlock();
    kdebug("device: driver %s registered on bus %s (%u bound)", drv->name, drv->bus->name, drv->bound);
    return 0;
}

void driver_unregister(struct device_driver *drv)
{
    model_lock();
    struct device *dev;
    list_for_each_entry_reverse(dev, &drv->bus->devices, bus_link) {
        if (dev->driver == drv)
            unbind(dev);
        else if (dev->state == DEV_FAILED)
            dev->state = DEV_UNBOUND;   /* give another driver a chance */
    }
    KASSERT(drv->bound == 0);
    list_remove(&drv->bus_link);
    list_init(&drv->bus_link);
    model_unlock();
}

vaddr_t device_map_mmio(struct device *dev, const struct resource *res)
{
    (void)dev;
    if (res == NULL || res->type != RES_MMIO || res->size == 0)
        return 0;
    paddr_t base = res->start & ~(paddr_t)(PAGE_SIZE - 1);
    size_t len = (size_t)ALIGN_UP(res->start + res->size, PAGE_SIZE) - (size_t)base;
    vaddr_t va = vm_map_phys(base, len, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        return 0;
    return va + (vaddr_t)(res->start - base);
}

void device_unmap_mmio(vaddr_t va)
{
    vm_unmap_phys(va & ~(vaddr_t)(PAGE_SIZE - 1));
}

struct device *device_find(struct bus_type *bus, const char *name)
{
    model_lock();
    struct device *dev, *found = NULL;
    list_for_each_entry(dev, &bus->devices, bus_link) {
        if (strcmp(dev->name, name) == 0) {
            found = dev;
            kobject_get(&dev->obj);
            break;
        }
    }
    model_unlock();
    return found;
}

static int for_each_on_bus(struct bus_type *bus, int (*fn)(struct device *dev, void *arg), void *arg)
{
    struct device *dev;
    list_for_each_entry(dev, &bus->devices, bus_link) {
        int rc = fn(dev, arg);
        if (rc)
            return rc;
    }
    return 0;
}

int device_for_each(struct bus_type *bus, int (*fn)(struct device *dev, void *arg), void *arg)
{
    int rc = 0;
    model_lock();
    if (bus) {
        rc = for_each_on_bus(bus, fn, arg);
    } else {
        struct bus_type *b;
        list_for_each_entry(b, &g_buses, link) {
            rc = for_each_on_bus(b, fn, arg);
            if (rc)
                break;
        }
    }
    model_unlock();
    return rc;
}

unsigned device_count(struct bus_type *bus)
{
    model_lock();
    unsigned n = 0;
    if (bus) {
        n = bus->nr_devices;
    } else {
        struct bus_type *b;
        list_for_each_entry(b, &g_buses, link)
            n += b->nr_devices;
    }
    model_unlock();
    return n;
}

static const char *state_name(enum device_state s)
{
    switch (s) {
    case DEV_UNBOUND: return "unbound";
    case DEV_BOUND:   return "bound";
    default:          return "failed";
    }
}

void device_dump(void)
{
    model_lock();
    struct bus_type *b;
    list_for_each_entry(b, &g_buses, link) {
        kprintf("bus %s: %u device(s)\n", b->name, b->nr_devices);
        struct device *dev;
        list_for_each_entry(dev, &b->devices, bus_link) {
            kprintf("  %-16s %-8s %s\n", dev->name, state_name(dev->state),
                    dev->driver ? dev->driver->name : "-");
            for (unsigned i = 0; i < dev->nr_res; i++) {
                const struct resource *r = &dev->res[i];
                kprintf("    %s 0x%llx+0x%llx\n", r->type == RES_MMIO ? "mmio" : r->type == RES_IO ? "io" : "irq",
                        (unsigned long long)r->start, (unsigned long long)r->size);
            }
        }
    }
    model_unlock();
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(bus_register);
EXPORT_SYMBOL(bus_find);
EXPORT_SYMBOL(device_setup);
EXPORT_SYMBOL(device_release_static);
EXPORT_SYMBOL(device_add_resource);
EXPORT_SYMBOL(device_resource);
EXPORT_SYMBOL(device_register);
EXPORT_SYMBOL(device_unregister);
EXPORT_SYMBOL(driver_register);
EXPORT_SYMBOL(driver_unregister);
EXPORT_SYMBOL(device_map_mmio);
EXPORT_SYMBOL(device_unmap_mmio);
EXPORT_SYMBOL(device_find);
EXPORT_SYMBOL(device_for_each);
EXPORT_SYMBOL(device_count);
