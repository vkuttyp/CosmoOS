/*
 * device.h - The device model: buses, devices, drivers, resources.
 *
 * A bus enumerates devices and knows how to match them to drivers; a
 * driver binds to a device through probe() and leaves through remove().
 * Registration of either side triggers probing of the other. One mutex
 * (g_device_lock) serialises the model; probe and remove run with it
 * held. Lock order: g_modules_lock -> g_device_lock -> kernel_space.lock.
 * See docs/kernel/device/.
 */

#ifndef KERNEL_DEVICE_H
#define KERNEL_DEVICE_H

#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/types.h>

#include <arch/mmu.h>

#define DEVICE_NAME_MAX      32
#define DEVICE_MAX_RESOURCES 8

enum resource_type {
    RES_MMIO,
    RES_IO,
    RES_IRQ,
};

struct resource {
    enum resource_type type;
    uint64_t start;
    uint64_t size;
    unsigned flags;        /* bus specific (PCI: BAR index, 64-bit, prefetch) */
};

enum device_state {
    DEV_UNBOUND,
    DEV_BOUND,
    DEV_FAILED,
};

struct device;
struct device_driver;

struct bus_type {
    const char *name;
    bool (*match)(struct device *dev, struct device_driver *drv);
    struct list_node devices;   /* struct device.bus_link */
    struct list_node drivers;   /* struct device_driver.bus_link */
    struct list_node link;      /* all buses */
    unsigned nr_devices;
};

struct device {
    struct kobject obj;
    char name[DEVICE_NAME_MAX];
    struct bus_type *bus;
    struct device *parent;
    struct device_driver *driver;   /* set for the duration of probe and while bound */
    void *drvdata;
    struct resource res[DEVICE_MAX_RESOURCES];
    unsigned nr_res;
    uint64_t dma_mask;          /* highest bus address the device can use */
    enum device_state state;
    int probe_error;
    struct list_node bus_link;
    /* Mandatory: runs when the last reference drops, after unregister;
     * frees the memory the device is embedded in. device_release_static
     * for objects that are never freed. Set before device_register. */
    void (*release)(struct device *dev);
};

/* For devices in static storage (tests, immortal bus roots). */
void device_release_static(struct device *dev);

struct device_driver {
    const char *name;
    struct bus_type *bus;
    const void *match_data;     /* bus-specific id table */
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    struct list_node bus_link;
    unsigned bound;
};

void device_init(void);

/* Buses are static and immortal; registering twice is a bug. */
void bus_register(struct bus_type *bus);
struct bus_type *bus_find(const char *name);

/* Prepare a device owned by `bus`: kobject, name, default DMA mask. The
 * caller allocated it (embedded in a bus-specific structure). */
void device_setup(struct device *dev, struct bus_type *bus, struct device *parent, const char *name);
int device_add_resource(struct device *dev, enum resource_type type, uint64_t start, uint64_t size,
                        unsigned flags);
const struct resource *device_resource(const struct device *dev, enum resource_type type, unsigned index);

/* Register: appends to the bus (taking the bus's reference) and probes
 * registered drivers. Sleeps. -EINVAL without a release, -EEXIST if a
 * device of that name is already on the bus. A probe failure is not a
 * registration failure (state DEV_FAILED). */
int device_register(struct device *dev);

/* Unbind (remove() if bound), drop from the bus and drop the bus's
 * reference. The creator's reference remains: device_put it when done
 * with the object; the release runs when the last holder is gone. Sleeps. */
void device_unregister(struct device *dev);

/* Register a driver and probe the bus's unbound devices. Sleeps.
 * Returns 0 even if no device matched. */
int driver_register(struct device_driver *drv);

/* remove() every bound device, then forget the driver. Sleeps. */
void driver_unregister(struct device_driver *drv);

/* Map an MMIO resource uncached. 0 on failure. The mapping lives until
 * device_unmap_mmio. Sleeps (VMM). */
vaddr_t device_map_mmio(struct device *dev, const struct resource *res);
void device_unmap_mmio(vaddr_t va);

/* Referenced pointer or NULL. Sleeps. */
struct device *device_find(struct bus_type *bus, const char *name);
static inline void device_get(struct device *dev) { kobject_get(&dev->obj); }
static inline void device_put(struct device *dev) { kobject_put(&dev->obj); }

/* Iterate a bus (or every bus when NULL) under the lock; return nonzero
 * from fn to stop. Sleeps. fn must not register or unregister. */
int device_for_each(struct bus_type *bus, int (*fn)(struct device *dev, void *arg), void *arg);

unsigned device_count(struct bus_type *bus);
void device_dump(void);

#endif /* KERNEL_DEVICE_H */
