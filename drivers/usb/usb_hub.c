/*
 * usb_hub.c - A USB hub: the second tier of the bus
 * (docs/drivers/usb/design.md, "Hubs").
 *
 * A hub is a device that has devices behind it, which is the first thing
 * on this bus whose parent is not the controller. What that costs the
 * core is a parent pointer, a depth and a route string on struct
 * usb_device (U2); what it costs the controller driver is two fields of
 * the slot context. Nothing else in the tree changed shape.
 *
 * The status-change endpoint reports which ports changed and nothing
 * about what changed, so every report wakes a worker thread which asks
 * each named port for its status. That split is not an optimisation: the
 * completion runs in interrupt context and every step of a port change
 * -- GET_STATUS, the reset, CLEAR_FEATURE, the enumeration that follows
 * -- is a control transfer that sleeps.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

#include <drivers/usb.h>

/* Hub class requests and descriptors (USB 2.0 §11.24). */
#define HUB_DT_HUB            0x29
#define HUB_MAX_PORTS         15u    /* a route string has four bits a tier */

#define HUB_PORT_CONNECTION   0
#define HUB_PORT_ENABLE       1
#define HUB_PORT_RESET        4
#define HUB_PORT_POWER        8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_ENABLE     17
#define HUB_C_PORT_SUSPEND    18
#define HUB_C_PORT_OVER_CURRENT 19
#define HUB_C_PORT_RESET      20

/* wPortStatus bits (§11.24.2.7.1) */
#define PORT_STAT_CONNECTION  (1u << 0)
#define PORT_STAT_ENABLE      (1u << 1)
#define PORT_STAT_OVERCURRENT (1u << 3)
#define PORT_STAT_RESET       (1u << 4)
#define PORT_STAT_POWER       (1u << 8)
#define PORT_STAT_LOW_SPEED   (1u << 9)
#define PORT_STAT_HIGH_SPEED  (1u << 10)

#define HUB_RESET_TRIES       50u    /* 50 x 10 ms: far past the 20 ms a reset takes */
#define HUB_DEBOUNCE_MS       100u   /* §7.1.7.3: let a connection settle before resetting */

struct hub_desc {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;      /* in 2 ms units */
    uint8_t bHubContrCurrent;
    uint8_t rest[8];             /* removable and power-control masks, one bit a port */
} __packed;

struct usb_hub {
    struct usb_device *udev;
    unsigned nr_ports;
    uint8_t ep;                       /* the status-change endpoint */
    uint8_t *status_buf;              /* the change bitmap the endpoint reports */
    unsigned status_len;
    uint8_t *port_buf;                /* four bytes for a port's status: transfers need direct-map memory */
    struct usb_request req;
    struct usb_device *child[HUB_MAX_PORTS + 1];   /* by 1-based port */

    struct thread *worker;
    struct waitqueue work;
    spinlock_t lock;                  /* changed, stopping */
    uint32_t changed;                 /* ports the last report named, 1-based bits */
    bool stopping;
    uint64_t reports, arrivals, departures, errors;
};

static int submit_status(struct usb_hub *h);

/* --- port requests: all of them sleep, so all of them are the worker's --- */

static int port_feature(struct usb_hub *h, unsigned port, uint16_t feature, bool set)
{
    return usb_control_msg(h->udev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                           set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE, feature, (uint16_t)port,
                           NULL, 0, 0);
}

/*
 * A transfer's buffer has to be memory the controller can reach through
 * the direct map (usb.h): a kernel stack is in the arena and is not, so
 * the four bytes of a port status live in the hub's own allocation. This
 * is the same rule the storage driver follows for its wrappers.
 */
static int port_status(struct usb_hub *h, unsigned port, uint16_t *status, uint16_t *change)
{
    uint8_t *buf = h->port_buf;
    memset(buf, 0, 4);
    int rc = usb_control_msg(h->udev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0,
                             (uint16_t)port, buf, 4, 0);
    if (rc < 0)
        return rc;
    if (rc < 4)
        return -EIO;
    *status = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    *change = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    return 0;
}

static enum usb_speed speed_of(uint16_t status)
{
    if (status & PORT_STAT_HIGH_SPEED)
        return USB_SPEED_HIGH;
    if (status & PORT_STAT_LOW_SPEED)
        return USB_SPEED_LOW;
    return USB_SPEED_FULL;
}

/* Reset a port and wait for the hub to say it is enabled. */
static int port_reset(struct usb_hub *h, unsigned port, uint16_t *status_out)
{
    int rc = port_feature(h, port, HUB_PORT_RESET, true);
    if (rc < 0)
        return rc;
    for (unsigned i = 0; i < HUB_RESET_TRIES; i++) {
        thread_sleep_ms(10);
        uint16_t status = 0, change = 0;
        rc = port_status(h, port, &status, &change);
        if (rc)
            return rc;
        if (change & (1u << (HUB_C_PORT_RESET - 16)))
            (void)port_feature(h, port, HUB_C_PORT_RESET, false);
        if (!(status & PORT_STAT_RESET) && (status & PORT_STAT_ENABLE)) {
            *status_out = status;
            return 0;
        }
        if (!(status & PORT_STAT_CONNECTION))
            return -ENODEV;   /* pulled out again during the reset */
    }
    return -ETIMEDOUT;
}

static void child_gone(struct usb_hub *h, unsigned port)
{
    struct usb_device *child = h->child[port];
    if (child == NULL)
        return;
    h->child[port] = NULL;
    usb_hub_port_disconnected(child);
    h->departures++;
}

/*
 * One port's change, in thread context: what is there now decides what
 * happens, not what the change bits say happened, because two changes
 * can land between reports.
 */
static void port_changed(struct usb_hub *h, unsigned port)
{
    uint16_t status = 0, change = 0;
    int rc = port_status(h, port, &status, &change);
    if (rc != 0) {
        kwarn("usb-hub: %s: port %u status: %d", h->udev->dev.name, port, rc);
        h->errors++;
        return;
    }
    kdebug("usb-hub: %s: port %u status 0x%04x change 0x%04x", h->udev->dev.name, port, status, change);
    /* Acknowledge everything the hub is holding, or it reports the same
     * change forever. */
    for (unsigned bit = HUB_C_PORT_CONNECTION; bit <= HUB_C_PORT_RESET; bit++) {
        if (change & (1u << (bit - 16)))
            (void)port_feature(h, port, (uint16_t)bit, false);
    }
    if (status & PORT_STAT_OVERCURRENT)
        kwarn("usb-hub: %s: port %u reports over-current", h->udev->dev.name, port);

    bool connected = (status & PORT_STAT_CONNECTION) != 0;
    if (!connected) {
        child_gone(h, port);
        return;
    }
    if (h->child[port] != NULL)
        return;   /* still the device we already know about */

    thread_sleep_ms(HUB_DEBOUNCE_MS);
    uint16_t after = 0;
    rc = port_reset(h, port, &after);
    if (rc) {
        kwarn("usb-hub: %s: port %u did not come up after a reset (%d)", h->udev->dev.name, port, rc);
        h->errors++;
        return;
    }
    struct usb_device *child = NULL;
    rc = usb_hub_port_connected(h->udev, port, speed_of(after), &child);
    if (rc) {
        kwarn("usb-hub: %s: port %u: enumeration failed (%d)", h->udev->dev.name, port, rc);
        h->errors++;
        return;
    }
    h->child[port] = child;
    h->arrivals++;
}

/* The hub's state, and its reference to its own device, belong to the
 * worker once `remove` has told it to stop: see hub_remove. */
static void hub_free(struct usb_hub *h)
{
    struct usb_device *udev = h->udev;
    kinfo("usb-hub: %s removed (%llu reports, %llu arrival(s), %llu departure(s), %llu error(s))",
          udev->dev.name, (unsigned long long)h->reports, (unsigned long long)h->arrivals,
          (unsigned long long)h->departures, (unsigned long long)h->errors);
    kfree(h->status_buf);
    kfree(h->port_buf);
    kfree(h);
    usb_device_put(udev);
}

static void hub_worker(void *arg)
{
    struct usb_hub *h = arg;

    /* The first pass is every port: devices plugged in before the hub
     * was driven are changes nobody reported. */
    uint32_t pending = 0;
    for (unsigned p = 1; p <= h->nr_ports; p++)
        pending |= 1u << p;

    for (;;) {
        arch_irq_state_t f = spin_lock_irqsave(&h->lock);
        pending |= h->changed;
        h->changed = 0;
        bool stopping = __atomic_load_n(&h->stopping, __ATOMIC_ACQUIRE);
        spin_unlock_irqrestore(&h->lock, f);
        if (stopping)
            break;
        if (pending == 0) {
            wait_event(&h->work, __atomic_load_n(&h->changed, __ATOMIC_ACQUIRE) != 0 ||
                                     __atomic_load_n(&h->stopping, __ATOMIC_ACQUIRE));
            continue;
        }
        for (unsigned p = 1; p <= h->nr_ports; p++) {
            if (!(pending & (1u << p)))
                continue;
            /* A port change takes a debounce and a reset; a hub told to
             * stop in the middle of a sweep does not finish it. */
            if (__atomic_load_n(&h->stopping, __ATOMIC_ACQUIRE))
                break;
            pending &= ~(1u << p);
            port_changed(h, p);
        }
    }

    /* The children were taken down by the core before the hub itself
     * (U9); this drops what the driver still held of them, and then the
     * hub's own state, which nobody else is waiting for. */
    for (unsigned p = 1; p <= h->nr_ports; p++)
        child_gone(h, p);
    hub_free(h);
    thread_exit(0);
}

/* Interrupt context: record which ports changed and wake the worker. */
static void status_done(struct usb_request *r)
{
    struct usb_hub *h = r->arg;

    if (r->status != 0) {
        /* -ENODEV or -ECANCELED: the hub is gone or `remove` took the
         * request back. -EPIPE needs thread context to clear, which this
         * is not, so it ends the polling too. */
        if (r->status == -EPIPE) {
            h->errors++;
            kerror("usb-hub: %s: status endpoint halted; no more port changes", h->udev->dev.name);
        }
        return;
    }
    h->reports++;
    uint32_t changed = 0;
    for (unsigned p = 1; p <= h->nr_ports; p++) {
        unsigned byte = p / 8, bit = p % 8;
        if (byte < r->actual && (h->status_buf[byte] & (1u << bit)))
            changed |= 1u << p;
    }

    /* Resubmit under the lock that `remove` sets `stopping` under, so a
     * request can never reach the ring after the cancel (usb_hid.c has
     * the same arrangement for the same reason). */
    arch_irq_state_t f = spin_lock_irqsave(&h->lock);
    h->changed |= changed;
    if (!__atomic_load_n(&h->stopping, __ATOMIC_ACQUIRE))
        (void)submit_status(h);
    spin_unlock_irqrestore(&h->lock, f);
    if (changed != 0)
        waitqueue_wake_all(&h->work);
}

static int submit_status(struct usb_hub *h)
{
    memset(&h->req, 0, sizeof(h->req));
    h->req.udev = h->udev;
    h->req.ep = h->ep;
    h->req.buf = h->status_buf;
    h->req.len = h->status_len;
    h->req.done = status_done;
    h->req.arg = h;
    int rc = usb_submit(&h->req);
    if (rc)
        h->errors++;
    return rc;
}

static int hub_probe(struct usb_device *udev, struct usb_interface *intf, const struct usb_id *id)
{
    (void)id;
    if (udev->depth + 1 > USB_MAX_DEPTH) {
        kinfo("usb-hub: %s is as deep as the bus goes; its ports are not driven", udev->dev.name);
        return -ENODEV;
    }
    uint8_t ep = 0;
    for (unsigned i = 0; i < intf->nr_ep; i++) {
        const struct usb_endpoint_descriptor *e = &intf->ep[i].desc;
        if (USB_EP_XFER(e->bmAttributes) == USB_EP_INTERRUPT && (e->bEndpointAddress & USB_EP_DIR_IN)) {
            ep = e->bEndpointAddress;
            break;
        }
    }
    if (ep == 0) {
        kerror("usb-hub: %s: no status-change endpoint", udev->dev.name);
        return -ENODEV;
    }

    struct hub_desc *hd = kzalloc(sizeof(*hd));
    if (hd == NULL)
        return -ENOMEM;
    int rc = usb_control_msg(udev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(HUB_DT_HUB << 8), 0, hd, sizeof(*hd), 0);
    if (rc < (int)sizeof(struct hub_desc) - 8) {
        kerror("usb-hub: %s: no hub descriptor (%d)", udev->dev.name, rc);
        kfree(hd);
        return -ENODEV;
    }
    unsigned nr_ports = hd->bNbrPorts;
    unsigned power_ms = (unsigned)hd->bPwrOn2PwrGood * 2;
    if (nr_ports == 0 || nr_ports > HUB_MAX_PORTS) {
        kinfo("usb-hub: %s has %u ports, which this driver does not describe; not driven", udev->dev.name,
              nr_ports);
        kfree(hd);
        return -ENODEV;
    }
    kfree(hd);

    struct usb_hub *h = kzalloc(sizeof(*h));
    if (h == NULL)
        return -ENOMEM;
    h->status_len = (nr_ports + 8) / 8;   /* one bit a port, plus the hub's own bit 0 */
    h->status_buf = kzalloc(h->status_len);
    h->port_buf = kzalloc(4);
    if (h->status_buf == NULL || h->port_buf == NULL) {
        kfree(h->status_buf);
        kfree(h->port_buf);
        kfree(h);
        return -ENOMEM;
    }
    h->udev = udev;
    device_get(&udev->dev);   /* the worker's: the hub outlives `remove` */
    h->nr_ports = nr_ports;
    h->ep = ep;
    spinlock_init(&h->lock, "usb-hub");
    waitqueue_init(&h->work, "usb-hub");
    udev->drvdata = h;

    /* Power every port and let the hub settle before anything is asked
     * of the devices behind it. */
    for (unsigned p = 1; p <= nr_ports; p++)
        (void)port_feature(h, p, HUB_PORT_POWER, true);
    thread_sleep_ms(power_ms + 10);

    h->worker = thread_create(hub_worker, h, "usb-hub", SCHED_PRIO_DEFAULT);
    if (h->worker == NULL) {
        udev->drvdata = NULL;
        kfree(h->status_buf);
        kfree(h->port_buf);
        kfree(h);
        usb_device_put(udev);
        return -ENOMEM;
    }
    rc = submit_status(h);
    if (rc) {
        kerror("usb-hub: %s: cannot watch for port changes (%d)", udev->dev.name, rc);
        arch_irq_state_t f = spin_lock_irqsave(&h->lock);
        __atomic_store_n(&h->stopping, true, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&h->lock, f);
        udev->drvdata = NULL;
        waitqueue_wake_all(&h->work);   /* the worker frees h and drops the reference */
        return rc;
    }
    kinfo("usb-hub: %s has %u port(s), %u ms to power, status on endpoint 0x%02x", udev->dev.name, nr_ports,
          power_ms, ep);
    return 0;
}

/*
 * A driver's `remove` runs with the device model's lock held, and this
 * driver's worker registers and unregisters devices -- so `remove` must
 * not wait for it. It tells the worker to stop and takes the request
 * back; the worker then owns everything, drops what the hub held and
 * frees it, with nobody blocked meanwhile. The hub's own device stays
 * alive for that because the worker holds a reference to it.
 *
 * What would go wrong with a join: pulling out a hub that is enumerating
 * would deadlock, because the worker would be inside `device_register`
 * waiting for the lock this call is holding (U9).
 */
static void hub_remove(struct usb_device *udev)
{
    struct usb_hub *h = udev->drvdata;
    if (h == NULL)
        return;

    /* Stop under the lock the completion resubmits under, so no request
     * reaches the ring after the cancel. */
    arch_irq_state_t f = spin_lock_irqsave(&h->lock);
    __atomic_store_n(&h->stopping, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&h->lock, f);
    (void)usb_cancel(&h->req, -ENODEV);
    udev->drvdata = NULL;
    waitqueue_wake_all(&h->work);
}

/* Every hub is class 09, subclass 0; protocol 0 is a full-speed hub, 1
 * and 2 are the single- and multiple-TT high-speed ones, and 3 is a
 * SuperSpeed hub. All four are driven the same way from here. */
static const struct usb_id hub_ids[] = {
    { 0, 0, USB_CLASS_HUB, 0, 0, USB_ID_CLASS },
    { 0, 0, USB_CLASS_HUB, 0, 1, USB_ID_CLASS },
    { 0, 0, USB_CLASS_HUB, 0, 2, USB_ID_CLASS },
    { 0, 0, USB_CLASS_HUB, 0, 3, USB_ID_CLASS },
    USB_ID_END,
};

static struct usb_driver hub_driver = {
    .drv = { .name = "usb-hub" },
    .ids = hub_ids,
    .probe = hub_probe,
    .remove = hub_remove,
};

static int hub_module_init(void)
{
    return usb_register_driver(&hub_driver);
}

static void hub_module_shutdown(void)
{
    usb_unregister_driver(&hub_driver);
}

COSMO_MODULE("usb_hub", "1.0", hub_module_init, hub_module_shutdown, "xhci", MODULE_CAP_DRIVER);
