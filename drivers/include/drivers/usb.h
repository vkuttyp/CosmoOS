/*
 * usb.h - The USB bus: devices, descriptors, transfers, class drivers,
 * and the seam to a host controller driver (docs/drivers/usb/).
 *
 * A class driver binds through `usb_bus` with a `struct usb_id` table
 * and talks to its device with usb_control_msg / usb_bulk_msg (thread
 * context) or usb_submit (any context; completion in interrupt
 * context). Every DMA on a USB device's behalf goes through the host
 * controller's device: usb_dma_dev(udev) is the only right answer to
 * "which struct device do I hand the DMA API" (design.md, "The DMA
 * rule"; invariant U1).
 */

#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

#include <kernel/completion.h>
#include <kernel/device.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/types.h>

/* --- descriptors (USB 2.0 §9.6) --------------------------------------------- */

#define USB_DT_DEVICE        1
#define USB_DT_CONFIG        2
#define USB_DT_STRING        3
#define USB_DT_INTERFACE     4
#define USB_DT_ENDPOINT      5

struct usb_device_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} __packed;

struct usb_config_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} __packed;

struct usb_interface_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} __packed;

struct usb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __packed;

#define USB_EP_DIR_IN      0x80u
#define USB_EP_NUM(addr)   ((addr) & 0x0fu)
#define USB_EP_XFER(attr)  ((attr) & 0x03u)
#define USB_EP_CONTROL     0
#define USB_EP_ISOC        1
#define USB_EP_BULK        2
#define USB_EP_INTERRUPT   3

/* Control request (USB 2.0 §9.3), as it goes on the wire. */
struct usb_setup {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} __packed;

#define USB_DIR_OUT          0x00u
#define USB_DIR_IN           0x80u
#define USB_TYPE_STANDARD    0x00u
#define USB_TYPE_CLASS       0x20u
#define USB_RECIP_DEVICE     0x00u
#define USB_RECIP_INTERFACE  0x01u
#define USB_RECIP_ENDPOINT   0x02u
#define USB_RECIP_OTHER      0x03u   /* a hub's port */

#define USB_REQ_GET_STATUS         0
#define USB_REQ_CLEAR_FEATURE      1
#define USB_REQ_SET_FEATURE        3
#define USB_REQ_SET_ADDRESS        5
#define USB_REQ_GET_DESCRIPTOR     6
#define USB_REQ_GET_CONFIGURATION  8
#define USB_REQ_SET_CONFIGURATION  9
#define USB_REQ_SET_INTERFACE      11
#define USB_FEATURE_ENDPOINT_HALT  0

#define USB_CLASS_HID          0x03
#define USB_CLASS_HUB          0x09
#define USB_CLASS_MASS_STORAGE 0x08

/* --- devices ---------------------------------------------------------------- */

enum usb_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_FULL    = 1,   /* the xHCI PORTSC/slot-context encoding */
    USB_SPEED_LOW     = 2,
    USB_SPEED_HIGH    = 3,
    USB_SPEED_SUPER   = 4,
};

#define USB_MAX_INTERFACES 4
#define USB_MAX_ENDPOINTS  8      /* per interface, EP0 not counted */
#define USB_CONFIG_MAX     512    /* the whole configuration descriptor */
#define USB_MAX_PORTS      32     /* root-hub ports a controller may have */

struct usb_device;

struct usb_endpoint {
    struct usb_endpoint_descriptor desc;
};

struct usb_interface {
    struct usb_interface_descriptor desc;
    struct usb_endpoint ep[USB_MAX_ENDPOINTS];
    unsigned nr_ep;
    struct usb_device *udev;
};

/* How deep a device may sit below the root hub. The route string holds
 * one nibble per tier and xHCI defines five (§8.9), which is also what
 * USB 3 allows. */
#define USB_MAX_DEPTH 5

struct usb_device {
    struct device dev;              /* on usb_bus, "usb<hcd>-<port>" or "...-<port>.<port>"; parent = the
                                     * controller, or the hub the device is plugged into */
    struct usb_hcd *hcd;
    unsigned port;                  /* the port on `parent`, 1-based (a root-hub port when parent is NULL) */
    /* Where the device is, which is what the controller must be told to
     * reach it (U2): the root-hub port at the top of the chain, the
     * route string of hub ports below it (four bits a tier, the first
     * tier in the lowest nibble), and how many tiers that is. All zero
     * for a device on a root port. */
    struct usb_device *parent;      /* the hub, or NULL on a root port. A reference is held. */
    unsigned root_port;
    uint32_t route;
    unsigned depth;
    enum usb_speed speed;
    unsigned slot;                  /* the HCD's slot id; 0 = none */
    struct usb_device_descriptor desc;
    struct usb_config_descriptor config;
    struct usb_interface intf[USB_MAX_INTERFACES];
    unsigned nr_intf;
    uint8_t raw_config[USB_CONFIG_MAX];
    unsigned raw_len;
    void *hcd_priv;                 /* the HCD's per-device state (slot contexts, rings) */
    bool gone;                      /* disconnected: every new submit is -ENODEV */
    void *drvdata;                  /* the bound class driver's */
};

static inline struct usb_device *to_usb_device(struct device *dev)
{
    return container_of(dev, struct usb_device, dev);
}

/* The device to hand the DMA API for anything this device transfers:
 * the controller, which is the requester (U1). Never udev->dev. */
struct device *usb_dma_dev(const struct usb_device *udev);

static inline void usb_device_get(struct usb_device *udev) { device_get(&udev->dev); }
static inline void usb_device_put(struct usb_device *udev) { device_put(&udev->dev); }

/* --- transfers -------------------------------------------------------------- */

/* A segment of a scatter-gather request: direct-map, physically contiguous. */
struct usb_sg {
    void *buf;
    uint32_t len;
};

struct usb_request {
    struct usb_device *udev;
    uint8_t ep;                     /* bEndpointAddress; 0 for a control transfer on EP0 */
    struct usb_setup setup;         /* control transfers only */
    void *buf;                      /* direct-map memory (kmalloc, dma_alloc); may be NULL when len is 0 */
    uint32_t len;
    const struct usb_sg *sgs;       /* bulk only: the segments instead of buf/len when nr_sgs > 0 */
    unsigned nr_sgs;
    uint64_t debug_dma;             /* tests only: nonzero = use this bus address for one segment of
                                     * `len` bytes, unmapped and unchecked (the IOMMU fault test) */
    bool debug_no_doorbell;         /* tests only: put the TRBs on the ring but never tell the controller,
                                     * so the request stays in flight until cancelled (a device that never answers) */
    uint32_t actual;                /* bytes moved, set at completion */
    int status;                     /* 0, -EPIPE (stall: the endpoint is halted), -EOVERFLOW (babble),
                                     * -EIO, -ETIMEDOUT / -ECANCELED (usb_cancel), -ENODEV (gone) */
    void (*done)(struct usb_request *r);   /* interrupt context; must not block */
    void *arg;
    void *hcd_priv;                 /* the HCD's, while in flight */
    struct list_node link;          /* the HCD's, while in flight */
};

/* Asynchronous: the HCD owns `r` until r->done runs. Any context. Fails
 * without running done: -ENODEV (device gone or controller dead),
 * -EINVAL (no such endpoint, a buffer dma_map refuses, too many
 * segments), -ENOSPC (the endpoint's ring is full). */
int usb_submit(struct usb_request *r);

/* Take a request back: the endpoint is stopped and its ring emptied;
 * every request on it completes (-ECANCELED, `r` itself with `status`)
 * before this returns. Thread context. 0, or -ENOENT if `r` had already
 * completed -- and in that case its `done` has finished too, so either
 * answer means the caller may free what the request pointed at. */
int usb_cancel(struct usb_request *r, int status);

/* Synchronous shapes, thread context, bounded by `timeout_ns` (0: the
 * default, USB_TIMEOUT_NS). usb_control_msg returns the bytes moved or a
 * negative errno; usb_bulk_msg returns 0 with *actual set, or -errno. */
#define USB_TIMEOUT_NS (1000ull * 1000 * 1000)
int usb_control_msg(struct usb_device *udev, uint8_t request_type, uint8_t request, uint16_t value,
                    uint16_t index, void *buf, uint16_t len, uint64_t timeout_ns);
int usb_bulk_msg(struct usb_device *udev, uint8_t ep, void *buf, uint32_t len, uint32_t *actual,
                 uint64_t timeout_ns);

/* After -EPIPE: reset the endpoint in the controller, then in the device
 * (CLEAR_FEATURE ENDPOINT_HALT). Thread context. */
int usb_clear_halt(struct usb_device *udev, uint8_t ep);


/* --- class drivers ---------------------------------------------------------- */

#define USB_ID_VENDOR (1u << 0)   /* match idVendor/idProduct */
#define USB_ID_CLASS  (1u << 1)   /* match an interface's class/subclass/protocol */
struct usb_id {
    uint16_t vendor, product;
    uint8_t class, subclass, protocol;
    unsigned flags;
};
#define USB_ID_END { 0, 0, 0, 0, 0, 0xffffffffu }

struct usb_driver {
    struct device_driver drv;
    const struct usb_id *ids;       /* terminated by USB_ID_END */
    /* `intf` is the interface the id matched (the device's first, for a
     * vendor match). The model's lock is held. */
    int (*probe)(struct usb_device *udev, struct usb_interface *intf, const struct usb_id *id);
    void (*remove)(struct usb_device *udev);
};

/* Register/unregister; probes attached devices. Sleeps. */
int usb_register_driver(struct usb_driver *udrv);
void usb_unregister_driver(struct usb_driver *udrv);

extern struct bus_type usb_bus;

/* --- the host controller seam (within the xhci module) ----------------------- */

struct usb_hcd;

struct usb_hcd_ops {
    /* A slot and an address for a device just reported on `udev->port`
     * at `udev->speed`; EP0 usable afterwards. Sets udev->slot. */
    int (*enable_device)(struct usb_hcd *hcd, struct usb_device *udev);
    /* EP0's max packet size turned out to be `mps`. */
    int (*update_ep0)(struct usb_hcd *hcd, struct usb_device *udev, unsigned mps);
    /* Rings and contexts for every endpoint of the parsed configuration. */
    int (*configure)(struct usb_hcd *hcd, struct usb_device *udev);
    /* Stop every endpoint, complete its requests -ENODEV, free the slot. */
    void (*disable_device)(struct usb_hcd *hcd, struct usb_device *udev);
    int (*submit)(struct usb_hcd *hcd, struct usb_request *r);
    int (*cancel)(struct usb_hcd *hcd, struct usb_request *r, int status);
    /* The controller's half of usb_clear_halt. */
    int (*reset_endpoint)(struct usb_hcd *hcd, struct usb_device *udev, uint8_t ep);
    /* Tests only: run the port worker's own path for `port` as if the
     * controller had reported a disconnect (false: the device is taken
     * down while the hardware stays attached) or a connect (true: the
     * port is reset and the device enumerated again). Thread context. */
    int (*debug_port)(struct usb_hcd *hcd, unsigned port, bool connected);
};

struct usb_hcd {
    struct device *dev;             /* the controller: the device that does DMA */
    const struct usb_hcd_ops *ops;
    unsigned index;                 /* "xhci0": names the devices "usb0-N" */
    unsigned nr_ports;
    struct usb_device *port_dev[USB_MAX_PORTS + 1];   /* by 1-based port */
    struct mutex lock;              /* enumeration and disconnect, one at a time */
    bool dead;                      /* a command never completed: every request is -EIO */
    uint64_t enumerated, released;  /* devices enumerated; usb_device releases run (tests read them) */
    void *priv;
};

int usb_hcd_register(struct usb_hcd *hcd);
void usb_hcd_unregister(struct usb_hcd *hcd);   /* disconnects every port first */

/* From the HCD's port worker, thread context: enumerate the device on
 * `port` (already enabled), or take down the one that was there. */
int usb_port_connected(struct usb_hcd *hcd, unsigned port, enum usb_speed speed);
void usb_port_disconnected(struct usb_hcd *hcd, unsigned port);

/* The same, one tier down, for a hub driver (drivers/usb/usb_hub.c):
 * `hub` is the hub's own device and `port` one of its ports, already
 * powered, reset and enabled. Thread context. On success *out holds a
 * reference the caller must give back with usb_hub_port_disconnected.
 * -ELOOP when the device would sit deeper than USB_MAX_DEPTH. */
int usb_hub_port_connected(struct usb_device *hub, unsigned port, enum usb_speed speed,
                           struct usb_device **out);
void usb_hub_port_disconnected(struct usb_device *child);

/* The HCD reports a request finished (any context). */
void usb_request_complete(struct usb_request *r, int status, uint32_t actual);

/* Maximum packet size of EP0 before the device has said (by speed). */
unsigned usb_ep0_mps(enum usb_speed speed);

/* Registers usb_bus; the xhci module's init calls it once. */
void usb_core_init(void);

/* Parse udev->raw_config (raw_len bytes) into config, intf[] and their
 * endpoints (usb_desc.c; fuzzed on the host). -EINVAL for anything that
 * does not walk cleanly to the end. */
int usb_parse_config(struct usb_device *udev);

#endif /* DRIVERS_USB_H */
