/*
 * usb.c - The USB core: the bus, enumeration, transfers, class drivers
 * (docs/drivers/usb/design.md, invariants U1-U7).
 *
 * Bus-independent: nothing here touches a controller register. The
 * controller driver reports ports and moves TRBs; this file turns a
 * port with a device on it into a struct usb_device on the "usb" bus
 * that class drivers can bind, and takes it down again in the order
 * the design document gives.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <drivers/usb.h>

/* --- the bus ------------------------------------------------------------------ */

static const struct usb_id *usb_match_ids(struct usb_device *udev, const struct usb_id *ids,
                                          struct usb_interface **intf_out)
{
    for (; ids->flags != 0xffffffffu; ids++) {
        if (ids->flags & USB_ID_VENDOR) {
            if (ids->vendor != udev->desc.idVendor || ids->product != udev->desc.idProduct)
                continue;
            if (!(ids->flags & USB_ID_CLASS)) {
                *intf_out = udev->nr_intf ? &udev->intf[0] : NULL;
                return ids;
            }
        }
        if (ids->flags & USB_ID_CLASS) {
            for (unsigned i = 0; i < udev->nr_intf; i++) {
                const struct usb_interface_descriptor *d = &udev->intf[i].desc;
                if (d->bInterfaceClass == ids->class && d->bInterfaceSubClass == ids->subclass &&
                    d->bInterfaceProtocol == ids->protocol) {
                    *intf_out = &udev->intf[i];
                    return ids;
                }
            }
        }
    }
    return NULL;
}

static bool usb_match(struct device *dev, struct device_driver *drv)
{
    struct usb_interface *intf;
    return usb_match_ids(to_usb_device(dev), drv->match_data, &intf) != NULL;
}

struct bus_type usb_bus = {
    .name = "usb",
    .match = usb_match,
};

static int usb_probe_thunk(struct device *dev)
{
    struct usb_driver *udrv = container_of(dev->driver, struct usb_driver, drv);
    struct usb_device *udev = to_usb_device(dev);
    struct usb_interface *intf = NULL;
    const struct usb_id *id = usb_match_ids(udev, udrv->ids, &intf);
    if (id == NULL)
        return -ENODEV;   /* matched a moment ago; cannot happen, but never probe on a guess */
    return udrv->probe(udev, intf, id);
}

static void usb_remove_thunk(struct device *dev)
{
    struct usb_driver *udrv = container_of(dev->driver, struct usb_driver, drv);
    if (udrv->remove)
        udrv->remove(to_usb_device(dev));
}

int usb_register_driver(struct usb_driver *udrv)
{
    if (udrv->ids == NULL || udrv->probe == NULL)
        return -EINVAL;
    udrv->drv.bus = &usb_bus;
    udrv->drv.match_data = udrv->ids;
    udrv->drv.probe = usb_probe_thunk;
    udrv->drv.remove = usb_remove_thunk;
    return driver_register(&udrv->drv);
}

void usb_unregister_driver(struct usb_driver *udrv)
{
    driver_unregister(&udrv->drv);
}

/* --- devices ------------------------------------------------------------------ */

struct device *usb_dma_dev(const struct usb_device *udev)
{
    return udev->hcd->dev;
}

unsigned usb_ep0_mps(enum usb_speed speed)
{
    switch (speed) {
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 8;   /* low, full: full may say more in its descriptor */
    }
}

static void usb_device_release(struct device *dev)
{
    struct usb_device *udev = to_usb_device(dev);
    if (udev->hcd)
        __atomic_fetch_add(&udev->hcd->released, 1u, __ATOMIC_RELAXED);
    kfree(udev);   /* hcd_priv was freed by disable_device, which runs before the last put */
}

/* --- transfers ---------------------------------------------------------------- */

int usb_submit(struct usb_request *r)
{
    struct usb_device *udev = r->udev;
    if (udev == NULL || udev->hcd == NULL)
        return -EINVAL;
    if (__atomic_load_n(&udev->gone, __ATOMIC_ACQUIRE) || udev->hcd->dead)
        return -ENODEV;
    r->actual = 0;
    r->status = -EINPROGRESS;
    return udev->hcd->ops->submit(udev->hcd, r);
}

int usb_cancel(struct usb_request *r, int status)
{
    return r->udev->hcd->ops->cancel(r->udev->hcd, r, status);
}

void usb_request_complete(struct usb_request *r, int status, uint32_t actual)
{
    r->actual = actual;
    r->status = status;
    if (r->done)
        r->done(r);
}

struct usb_sync {
    struct completion done;
};

static void usb_sync_done(struct usb_request *r)
{
    struct usb_sync *s = r->arg;
    complete(&s->done);
}

/*
 * Submit and wait, bounded. No timed wait exists on a completion, so
 * this polls it at a fine grain; on the deadline the request is
 * cancelled, which completes it (with `-ETIMEDOUT`) before returning
 * unless the controller completed it first -- either way the completion
 * is signalled and the stack frame it lives in is safe to leave.
 */
static int usb_sync_msg(struct usb_request *r, uint64_t timeout_ns)
{
    struct usb_sync s;
    completion_init(&s.done, "usb-sync");
    r->done = usb_sync_done;
    r->arg = &s;
    int rc = usb_submit(r);
    if (rc)
        return rc;
    uint64_t deadline = clock_now_ns() + (timeout_ns ? timeout_ns : USB_TIMEOUT_NS);
    while (!completion_done(&s.done) && clock_now_ns() < deadline)
        thread_sleep_ns(250000);
    if (!completion_done(&s.done)) {
        if (usb_cancel(r, -ETIMEDOUT) == 0)
            kwarn("usb: %s: %s transfer on ep 0x%02x timed out", r->udev->dev.name,
                  r->ep == 0 ? "control" : "bulk", r->ep);
    }
    wait_for_completion(&s.done);
    return r->status;
}

int usb_control_msg(struct usb_device *udev, uint8_t request_type, uint8_t request, uint16_t value,
                    uint16_t index, void *buf, uint16_t len, uint64_t timeout_ns)
{
    struct usb_request r;
    memset(&r, 0, sizeof(r));
    r.udev = udev;
    r.ep = 0;
    r.setup.bmRequestType = request_type;
    r.setup.bRequest = request;
    r.setup.wValue = value;
    r.setup.wIndex = index;
    r.setup.wLength = len;
    r.buf = buf;
    r.len = len;
    int rc = usb_sync_msg(&r, timeout_ns);
    return rc ? rc : (int)r.actual;
}

int usb_bulk_msg(struct usb_device *udev, uint8_t ep, void *buf, uint32_t len, uint32_t *actual,
                 uint64_t timeout_ns)
{
    struct usb_request r;
    memset(&r, 0, sizeof(r));
    r.udev = udev;
    r.ep = ep;
    r.buf = buf;
    r.len = len;
    int rc = usb_sync_msg(&r, timeout_ns);
    if (actual)
        *actual = r.actual;
    return rc;
}

int usb_clear_halt(struct usb_device *udev, uint8_t ep)
{
    /* The controller's view first, then the device's (xHCI §4.6.8). */
    int rc = udev->hcd->ops->reset_endpoint(udev->hcd, udev, ep);
    if (rc)
        return rc;
    rc = usb_control_msg(udev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                         USB_FEATURE_ENDPOINT_HALT, ep, NULL, 0, 0);
    return rc < 0 ? rc : 0;
}

/* --- descriptors -------------------------------------------------------------- */

static int usb_get_descriptor(struct usb_device *udev, uint8_t type, uint8_t index, void *buf, uint16_t len)
{
    return usb_control_msg(udev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)((type << 8) | index), 0, buf, len, 0);
}

/* --- enumeration -------------------------------------------------------------- */

static const char *speed_name(enum usb_speed s)
{
    switch (s) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "unknown";
    }
}

static int usb_enumerate(struct usb_device *udev)
{
    struct usb_hcd *hcd = udev->hcd;
    int rc = hcd->ops->enable_device(hcd, udev);
    if (rc) {
        kerror("usb: %s: address device failed (%d)", udev->dev.name, rc);
        return rc;
    }

    /* The first 8 bytes carry bMaxPacketSize0; a full-speed device may
     * say more than the 8 the context assumed, and nothing longer is read
     * until the context agrees with the device. */
    uint8_t *tmp = kmalloc(USB_CONFIG_MAX, 0);
    if (tmp == NULL)
        return -ENOMEM;
    rc = usb_get_descriptor(udev, USB_DT_DEVICE, 0, tmp, 8);
    if (rc < 8) {
        kerror("usb: %s: device descriptor (first 8 bytes): %d", udev->dev.name, rc);
        rc = rc < 0 ? rc : -EIO;
        goto out;
    }
    /* USB 3: bMaxPacketSize0 is an exponent (9 for 512); USB 2: the size. */
    unsigned mps0 = udev->speed == USB_SPEED_SUPER ? (tmp[7] < 16 ? 1u << tmp[7] : 0) : tmp[7];
    if (mps0 != usb_ep0_mps(udev->speed)) {
        if (udev->speed != USB_SPEED_FULL || (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64)) {
            kerror("usb: %s: bMaxPacketSize0 %u is not valid at %s speed", udev->dev.name, mps0,
                   speed_name(udev->speed));
            rc = -EINVAL;
            goto out;
        }
        rc = hcd->ops->update_ep0(hcd, udev, mps0);
        if (rc) {
            kerror("usb: %s: EP0 max packet %u: %d", udev->dev.name, mps0, rc);
            goto out;
        }
    }
    rc = usb_get_descriptor(udev, USB_DT_DEVICE, 0, tmp, sizeof(udev->desc));
    if (rc < (int)sizeof(udev->desc)) {
        kerror("usb: %s: device descriptor: %d", udev->dev.name, rc);
        rc = rc < 0 ? rc : -EIO;
        goto out;
    }
    memcpy(&udev->desc, tmp, sizeof(udev->desc));
    if (udev->desc.bDescriptorType != USB_DT_DEVICE || udev->desc.bNumConfigurations == 0) {
        kerror("usb: %s: not a device descriptor (type %u, %u configurations)", udev->dev.name,
               udev->desc.bDescriptorType, udev->desc.bNumConfigurations);
        rc = -EINVAL;
        goto out;
    }

    rc = usb_get_descriptor(udev, USB_DT_CONFIG, 0, tmp, sizeof(struct usb_config_descriptor));
    if (rc < (int)sizeof(struct usb_config_descriptor)) {
        kerror("usb: %s: configuration descriptor header: %d", udev->dev.name, rc);
        rc = rc < 0 ? rc : -EIO;
        goto out;
    }
    unsigned total = ((const struct usb_config_descriptor *)tmp)->wTotalLength;
    if (total < sizeof(struct usb_config_descriptor) || total > USB_CONFIG_MAX) {
        kerror("usb: %s: configuration of %u bytes refused (limit %u)", udev->dev.name, total, USB_CONFIG_MAX);
        rc = -EINVAL;
        goto out;
    }
    rc = usb_get_descriptor(udev, USB_DT_CONFIG, 0, udev->raw_config, (uint16_t)total);
    if (rc < (int)total) {
        kerror("usb: %s: configuration descriptor (%u bytes): %d", udev->dev.name, total, rc);
        rc = rc < 0 ? rc : -EIO;
        goto out;
    }
    udev->raw_len = total;
    rc = usb_parse_config(udev);
    if (rc) {
        kerror("usb: %s: malformed configuration descriptor", udev->dev.name);
        goto out;
    }

    rc = hcd->ops->configure(hcd, udev);
    if (rc) {
        kerror("usb: %s: configure endpoints: %d", udev->dev.name, rc);
        goto out;
    }
    rc = usb_control_msg(udev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                         udev->config.bConfigurationValue, 0, NULL, 0, 0);
    if (rc < 0) {
        kerror("usb: %s: SET_CONFIGURATION %u: %d", udev->dev.name, udev->config.bConfigurationValue, rc);
        goto out;
    }
    rc = 0;
out:
    kfree(tmp);
    return rc;
}

/*
 * Enumerate a device that is already enabled on its port, wherever that
 * port is: `parent` is the hub it hangs from, or NULL for a root-hub
 * port. On success the device is registered and holds a reference for
 * its caller. The root-port caller holds hcd->lock; a hub's worker does
 * not (see usb_hub_port_connected).
 */
static int usb_device_arrived(struct usb_hcd *hcd, struct usb_device *parent, unsigned port,
                              enum usb_speed speed, struct usb_device **out)
{
    struct usb_device *udev = kzalloc(sizeof(*udev));
    if (udev == NULL)
        return -ENOMEM;

    char name[DEVICE_NAME_MAX];
    if (parent == NULL)
        ksnprintf(name, sizeof(name), "usb%u-%u", hcd->index, port);
    else
        ksnprintf(name, sizeof(name), "%s.%u", parent->dev.name, port);
    device_setup(&udev->dev, &usb_bus, parent ? &parent->dev : hcd->dev, name);
    udev->dev.release = usb_device_release;
    udev->dev.dma_mask = hcd->dev->dma_mask;   /* informational: DMA goes through the controller (U1) */
    udev->hcd = hcd;
    udev->port = port;
    udev->speed = speed;
    if (parent == NULL) {
        udev->root_port = port;
    } else {
        /* One nibble a tier, the tier nearest the root hub lowest
         * (xHCI §8.9). A port above 15 cannot be described, and a hub
         * with more than 15 ports does not exist. */
        udev->parent = parent;
        udev->root_port = parent->root_port;
        udev->depth = parent->depth + 1;
        udev->route = parent->route | ((port > 15 ? 15u : port) << (4 * parent->depth));
        device_get(&parent->dev);   /* the child keeps its hub alive */
    }

    int rc = usb_enumerate(udev);
    if (rc) {
        /* Addressed or not, the slot goes; the port stays as it is until
         * its next change, and the log has said which step failed. */
        hcd->ops->disable_device(hcd, udev);
        udev->parent = NULL;
        device_put(&udev->dev);
        if (parent != NULL)
            device_put(&parent->dev);   /* the reference taken above */
        return rc;
    }
    const struct usb_interface_descriptor *i0 = udev->nr_intf ? &udev->intf[0].desc : NULL;
    kinfo("usb: %s: %04x:%04x at %s speed, %u interface(s), class %02x/%02x/%02x", udev->dev.name,
          udev->desc.idVendor, udev->desc.idProduct, speed_name(speed), udev->nr_intf,
          i0 ? i0->bInterfaceClass : udev->desc.bDeviceClass, i0 ? i0->bInterfaceSubClass : udev->desc.bDeviceSubClass,
          i0 ? i0->bInterfaceProtocol : udev->desc.bDeviceProtocol);
    hcd->enumerated++;
    rc = device_register(&udev->dev);   /* probes class drivers; a probe failure is DEV_FAILED, not ours */
    if (rc) {
        kerror("usb: %s: device_register: %d", udev->dev.name, rc);
        hcd->ops->disable_device(hcd, udev);
        udev->parent = NULL;
        device_put(&udev->dev);
        if (parent != NULL)
            device_put(&parent->dev);
        return rc;
    }
    *out = udev;
    return 0;
}

/* Devices whose parent is `parent`, with a reference on each. */
struct child_walk {
    const struct usb_device *parent;
    struct usb_device *kids[USB_MAX_PORTS];
    unsigned n;
};

static int collect_child(struct device *dev, void *arg)
{
    struct child_walk *w = arg;
    struct usb_device *udev = to_usb_device(dev);
    if (udev->parent == w->parent && w->n < USB_MAX_PORTS) {
        device_get(dev);
        w->kids[w->n++] = udev;
    }
    return 0;
}

static void usb_device_left(struct usb_hcd *hcd, struct usb_device *udev);

/*
 * Everything behind `parent` goes before `parent` does. The core does
 * this rather than the hub driver because a driver's `remove` runs with
 * the device model's lock held and so cannot wait for a thread that
 * registers or unregisters devices (U9); the walk here happens before
 * that lock is taken.
 */
static void remove_children(struct usb_hcd *hcd, struct usb_device *parent)
{
    for (;;) {
        struct child_walk w = { parent, { NULL }, 0 };
        device_for_each(&usb_bus, collect_child, &w);   /* the model's lock, and released again */
        if (w.n == 0)
            return;
        for (unsigned i = 0; i < w.n; i++) {
            usb_device_left(hcd, w.kids[i]);
            device_put(&w.kids[i]->dev);
        }
        if (w.n < USB_MAX_PORTS)
            return;   /* the walk saw them all */
    }
}

/*
 * A device gone from its port: everything behind it first, then refuse
 * new submits, run the class driver's remove, and take the slot and
 * everything on it, completing what was in flight with -ENODEV. The
 * memory goes when the last holder lets go (design.md, "Disconnect").
 *
 * Idempotent, and it has to be: a hub's child can be taken down here as
 * part of the hub's removal and again by the hub's worker when it drops
 * what it held. The caller keeps its own reference and puts it after.
 */
static void usb_device_left(struct usb_hcd *hcd, struct usb_device *udev)
{
    if (__atomic_exchange_n(&udev->gone, true, __ATOMIC_ACQ_REL))
        return;
    remove_children(hcd, udev);
    device_unregister(&udev->dev);
    hcd->ops->disable_device(hcd, udev);
    kinfo("usb: %s: disconnected", udev->dev.name);
    struct usb_device *parent = udev->parent;
    udev->parent = NULL;
    if (parent != NULL)
        device_put(&parent->dev);
}

int usb_port_connected(struct usb_hcd *hcd, unsigned port, enum usb_speed speed)
{
    if (port == 0 || port > hcd->nr_ports)
        return -EINVAL;
    mutex_lock(&hcd->lock);
    if (hcd->port_dev[port] != NULL) {
        mutex_unlock(&hcd->lock);
        return -EBUSY;   /* the worker reports a disconnect before a new connect */
    }
    struct usb_device *udev = NULL;
    int rc = usb_device_arrived(hcd, NULL, port, speed, &udev);
    if (rc == 0)
        hcd->port_dev[port] = udev;
    mutex_unlock(&hcd->lock);
    return rc;
}

/*
 * A device on a hub's port. Like its disconnect counterpart below, this
 * takes no controller lock: a hub's ports are the work of that hub's one
 * worker thread, and the hub driver's `remove` -- which the core runs
 * with the lock held -- joins that thread. Reaching for the lock here
 * would deadlock a hub unplugged while it was enumerating, which is
 * exactly what pulling a dock out does.
 *
 * What is left serialising this against a root port's enumeration on the
 * same controller is the controller's own command lock, which is where
 * that serialisation belongs: the core's mutex exists for the port
 * bookkeeping (`port_dev[]`), and a hub's children have none.
 */
int usb_hub_port_connected(struct usb_device *hub, unsigned port, enum usb_speed speed,
                           struct usb_device **out)
{
    if (hub == NULL || out == NULL || port == 0)
        return -EINVAL;
    if (hub->depth + 1 > USB_MAX_DEPTH)
        return -ELOOP;   /* deeper than a route string can say */
    if (__atomic_load_n(&hub->gone, __ATOMIC_ACQUIRE))
        return -ENODEV;
    return usb_device_arrived(hub->hcd, hub, port, speed, out);
}

/*
 * A device behind a hub is gone. Unlike the root-hub pair, this takes no
 * controller lock, and must not: it is called from the hub's worker,
 * which the hub driver's `remove` joins -- and `remove` itself runs from
 * inside `device_unregister` on the disconnect path, which holds that
 * lock already. Taking it here would deadlock the first time a hub was
 * unplugged or the controller unregistered.
 *
 * What the lock gives the root-hub pair is one enumeration or disconnect
 * at a time per controller. A hub's ports have that anyway: every arrival
 * and departure below a hub is the work of that hub's single worker
 * thread, and `remove` stops the worker before anything else happens.
 */
void usb_hub_port_disconnected(struct usb_device *child)
{
    if (child == NULL)
        return;
    usb_device_left(child->hcd, child);
    device_put(&child->dev);   /* the hub driver's reference */
}

void usb_port_disconnected(struct usb_hcd *hcd, unsigned port)
{
    if (port == 0 || port > hcd->nr_ports)
        return;
    mutex_lock(&hcd->lock);
    struct usb_device *udev = hcd->port_dev[port];
    if (udev == NULL) {
        mutex_unlock(&hcd->lock);
        return;
    }
    hcd->port_dev[port] = NULL;
    usb_device_left(hcd, udev);
    mutex_unlock(&hcd->lock);
    device_put(&udev->dev);   /* the port's reference */
}

/* --- controllers -------------------------------------------------------------- */

static unsigned g_next_hcd;

int usb_hcd_register(struct usb_hcd *hcd)
{
    if (hcd->dev == NULL || hcd->ops == NULL || hcd->nr_ports == 0 || hcd->nr_ports > USB_MAX_PORTS)
        return -EINVAL;
    mutex_init(&hcd->lock, "usb-hcd");
    hcd->index = __atomic_fetch_add(&g_next_hcd, 1u, __ATOMIC_RELAXED);
    memset(hcd->port_dev, 0, sizeof(hcd->port_dev));
    hcd->dead = false;
    hcd->enumerated = hcd->released = 0;
    return 0;
}

void usb_hcd_unregister(struct usb_hcd *hcd)
{
    /* Children first: the model does not cascade, and the hardware
     * order needs the slots gone before the controller is (design.md,
     * "What the device model needed"). */
    for (unsigned p = 1; p <= hcd->nr_ports; p++)
        usb_port_disconnected(hcd, p);
}

/* --- module glue ----------------------------------------------------------------- */

void usb_core_init(void)
{
    bus_register(&usb_bus);
}

EXPORT_SYMBOL(usb_bus);
EXPORT_SYMBOL(usb_register_driver);
EXPORT_SYMBOL(usb_unregister_driver);
EXPORT_SYMBOL(usb_dma_dev);
EXPORT_SYMBOL(usb_submit);
EXPORT_SYMBOL(usb_cancel);
EXPORT_SYMBOL(usb_control_msg);
EXPORT_SYMBOL(usb_bulk_msg);
EXPORT_SYMBOL(usb_clear_halt);
EXPORT_SYMBOL(usb_hub_port_connected);
EXPORT_SYMBOL(usb_hub_port_disconnected);
