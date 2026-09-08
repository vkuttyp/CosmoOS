/*
 * usb_hid.c - A USB keyboard, in the boot protocol
 * (docs/drivers/usb/design.md, "The keyboard").
 *
 * The first periodic transfer in the tree: one interrupt IN endpoint with
 * a request resubmitted from its own completion, forever. Each report is
 * eight bytes -- modifiers, a reserved byte, and six keycodes -- which is
 * what a device sends after SET_PROTOCOL(boot), so there is no report
 * descriptor parser here. A device that speaks only the report protocol
 * gets one log line and no driver; the parser waits for a device that
 * needs one (constitution section 21).
 *
 * Keys go to the console tty through tty_input, in interrupt context,
 * which is exactly what the two UART drivers do: a key from here and a
 * byte from a serial line arrive by the same door, so the shell needs to
 * know nothing about either.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/tty.h>

#include <drivers/usb.h>

#define HID_SUBCLASS_BOOT   0x01
#define HID_PROTOCOL_KEYBOARD 0x01

/* Class requests (HID 1.11 §7.2) */
#define HID_REQ_GET_REPORT  0x01
#define HID_REQ_SET_IDLE    0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_PROTOCOL_BOOT   0

#define HID_REPORT_LEN      8u
#define HID_KEYS            6u    /* keycodes in a boot report */

/* Modifier bits (byte 0 of a boot report). */
#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_CTRL    (HID_MOD_LCTRL | HID_MOD_RCTRL)
#define HID_MOD_SHIFT   (HID_MOD_LSHIFT | HID_MOD_RSHIFT)

/* Usage IDs the driver understands (HID Usage Tables §10, keyboard page).
 * A US layout: two tables indexed by usage, unshifted and shifted. Zero
 * means "no character", and those keys are dropped -- the tty has no use
 * for a function key and no way to say so. */
#define HID_USAGE_MAX 0x39u

static const char hid_plain[HID_USAGE_MAX + 1] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e', [0x09] = 'f',
    [0x0a] = 'g', [0x0b] = 'h', [0x0c] = 'i', [0x0d] = 'j', [0x0e] = 'k', [0x0f] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p', [0x14] = 'q', [0x15] = 'r',
    [0x16] = 's', [0x17] = 't', [0x18] = 'u', [0x19] = 'v', [0x1a] = 'w', [0x1b] = 'x',
    [0x1c] = 'y', [0x1d] = 'z',
    [0x1e] = '1', [0x1f] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6',
    [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x2a] = '\b', [0x2b] = '\t', [0x2c] = ' ',
    [0x2d] = '-', [0x2e] = '=', [0x2f] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const char hid_shift[HID_USAGE_MAX + 1] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E', [0x09] = 'F',
    [0x0a] = 'G', [0x0b] = 'H', [0x0c] = 'I', [0x0d] = 'J', [0x0e] = 'K', [0x0f] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R',
    [0x16] = 'S', [0x17] = 'T', [0x18] = 'U', [0x19] = 'V', [0x1a] = 'W', [0x1b] = 'X',
    [0x1c] = 'Y', [0x1d] = 'Z',
    [0x1e] = '!', [0x1f] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%', [0x23] = '^',
    [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x2a] = '\b', [0x2b] = '\t', [0x2c] = ' ',
    [0x2d] = '_', [0x2e] = '+', [0x2f] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

struct hid_kbd {
    struct usb_device *udev;
    uint8_t ep;                       /* the interrupt IN endpoint */
    unsigned ifnum;
    uint8_t *report;                  /* HID_REPORT_LEN, direct-map, the transfer's buffer */
    uint8_t previous[HID_REPORT_LEN]; /* to tell a press from a key still held */
    struct usb_request req;
    spinlock_t lock;                  /* `stopping` against the completion */
    bool stopping;
    uint64_t reports, keys, dropped, errors;
};

static int submit_report(struct hid_kbd *k);

/* One keycode's character, or 0 for a key with none. */
static char translate(uint8_t usage, uint8_t mods)
{
    if (usage > HID_USAGE_MAX)
        return 0;
    char c = (mods & HID_MOD_SHIFT) ? hid_shift[usage] : hid_plain[usage];
    if (c == 0)
        return 0;
    if (mods & HID_MOD_CTRL) {
        /* Control-letter is the control byte the tty already understands:
         * ^D ends a line, ^U clears it. Anything else with control held
         * is dropped rather than sent as its plain character. */
        if (c >= 'a' && c <= 'z')
            return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z')
            return (char)(c - 'A' + 1);
        return 0;
    }
    return c;
}

/*
 * A report is the set of keys held right now, not a stream of events, so
 * a press is a keycode in this report that was not in the last one. A key
 * held down therefore repeats nothing: auto-repeat is the tty's business
 * to want and nobody has yet.
 */
static void report_keys(struct hid_kbd *k)
{
    uint8_t mods = k->report[0];
    for (unsigned i = 0; i < HID_KEYS; i++) {
        uint8_t usage = k->report[2 + i];
        if (usage <= 3)
            continue;   /* 0 is empty; 1..3 are rollover and error codes */
        bool was_held = false;
        for (unsigned j = 0; j < HID_KEYS; j++) {
            if (k->previous[2 + j] == usage)
                was_held = true;
        }
        if (was_held)
            continue;
        char c = translate(usage, mods);
        if (c == 0) {
            k->dropped++;
            continue;
        }
        uint8_t byte = (uint8_t)c;
        tty_input(tty_console(), &byte, 1);
        k->keys++;
    }
    memcpy(k->previous, k->report, HID_REPORT_LEN);
}

/* Interrupt context: read the report, then ask for the next one. */
static void report_done(struct usb_request *r)
{
    struct hid_kbd *k = r->arg;

    if (r->status == 0) {
        if (r->actual >= HID_REPORT_LEN) {
            k->reports++;
            report_keys(k);
        } else {
            k->errors++;   /* a short report says nothing; ask again */
        }
    } else if (r->status == -EPIPE) {
        /* A halted endpoint needs thread context to clear, which this is
         * not. Count it and stop asking; the device is unusable until it
         * is unplugged and back. */
        k->errors++;
        kerror("usb-hid: %s: interrupt endpoint halted; no more keys", k->udev->dev.name);
        return;
    } else {
        /* -ENODEV or -ECANCELED: the device is gone or `remove` took the
         * request back. Either way this is the last completion. */
        return;
    }

    /* Resubmit under the lock that `remove` sets `stopping` under, so a
     * request can never be put on the ring after `remove` has cancelled:
     * either this sees the flag and stops, or it submits first and the
     * cancel takes that request back. */
    arch_irq_state_t f = spin_lock_irqsave(&k->lock);
    if (!__atomic_load_n(&k->stopping, __ATOMIC_ACQUIRE))
        (void)submit_report(k);
    spin_unlock_irqrestore(&k->lock, f);
}

static int submit_report(struct hid_kbd *k)
{
    memset(&k->req, 0, sizeof(k->req));
    k->req.udev = k->udev;
    k->req.ep = k->ep;
    k->req.buf = k->report;
    k->req.len = HID_REPORT_LEN;
    k->req.done = report_done;
    k->req.arg = k;
    int rc = usb_submit(&k->req);
    if (rc)
        k->errors++;
    return rc;
}

static int hid_probe(struct usb_device *udev, struct usb_interface *intf, const struct usb_id *id)
{
    (void)id;
    if (intf->desc.bInterfaceSubClass != HID_SUBCLASS_BOOT) {
        kinfo("usb-hid: %s: interface subclass %u speaks only the report protocol; not driven",
              udev->dev.name, intf->desc.bInterfaceSubClass);
        return -ENODEV;
    }

    uint8_t ep = 0;
    unsigned interval = 0;
    for (unsigned i = 0; i < intf->nr_ep; i++) {
        const struct usb_endpoint_descriptor *e = &intf->ep[i].desc;
        if (USB_EP_XFER(e->bmAttributes) == USB_EP_INTERRUPT && (e->bEndpointAddress & USB_EP_DIR_IN)) {
            ep = e->bEndpointAddress;
            interval = e->bInterval;
            break;
        }
    }
    if (ep == 0) {
        kerror("usb-hid: %s: no interrupt IN endpoint", udev->dev.name);
        return -ENODEV;
    }

    struct hid_kbd *k = kzalloc(sizeof(*k));
    if (k == NULL)
        return -ENOMEM;
    k->report = kzalloc(HID_REPORT_LEN);
    if (k->report == NULL) {
        kfree(k);
        return -ENOMEM;
    }
    k->udev = udev;
    k->ep = ep;
    k->ifnum = intf->desc.bInterfaceNumber;
    spinlock_init(&k->lock, "usb-hid");

    /* The boot protocol: a fixed eight-byte report, so no descriptor is
     * parsed. A device that refuses is one this driver cannot drive. */
    int rc = usb_control_msg(udev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_PROTOCOL,
                             HID_PROTOCOL_BOOT, (uint16_t)k->ifnum, NULL, 0, 0);
    if (rc < 0) {
        kerror("usb-hid: %s: SET_PROTOCOL(boot) failed (%d)", udev->dev.name, rc);
        kfree(k->report);
        kfree(k);
        return -ENODEV;
    }
    /* Report only when something changes: an idle keyboard then costs one
     * NAKed poll per interval instead of a report (fb/usb benchmarks). A
     * device that does not implement SET_IDLE is not a problem. */
    (void)usb_control_msg(udev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_IDLE, 0,
                          (uint16_t)k->ifnum, NULL, 0, 0);

    udev->drvdata = k;
    rc = submit_report(k);
    if (rc) {
        kerror("usb-hid: %s: cannot start polling (%d)", udev->dev.name, rc);
        udev->drvdata = NULL;
        kfree(k->report);
        kfree(k);
        return rc;
    }
    kinfo("usb-hid: %s is a boot keyboard on endpoint 0x%02x, interval %u", udev->dev.name, ep, interval);
    return 0;
}

static void hid_remove(struct usb_device *udev)
{
    struct hid_kbd *k = udev->drvdata;
    if (k == NULL)
        return;

    /* Stop before cancelling, under the lock the completion resubmits
     * under: after this, no completion can put the request back on the
     * ring, so the cancel below is the last word. */
    arch_irq_state_t f = spin_lock_irqsave(&k->lock);
    __atomic_store_n(&k->stopping, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&k->lock, f);
    (void)usb_cancel(&k->req, -ENODEV);

    kinfo("usb-hid: %s removed (%llu reports, %llu keys, %llu dropped, %llu errors)", udev->dev.name,
          (unsigned long long)k->reports, (unsigned long long)k->keys, (unsigned long long)k->dropped,
          (unsigned long long)k->errors);
    udev->drvdata = NULL;
    kfree(k->report);
    kfree(k);
}

static const struct usb_id hid_ids[] = {
    { 0, 0, USB_CLASS_HID, HID_SUBCLASS_BOOT, HID_PROTOCOL_KEYBOARD, USB_ID_CLASS },
    USB_ID_END,
};

static struct usb_driver hid_driver = {
    .drv = { .name = "usb-hid" },
    .ids = hid_ids,
    .probe = hid_probe,
    .remove = hid_remove,
};

static int hid_module_init(void)
{
    return usb_register_driver(&hid_driver);
}

static void hid_module_shutdown(void)
{
    usb_unregister_driver(&hid_driver);
}

COSMO_MODULE("usb_hid", "1.0", hid_module_init, hid_module_shutdown, "xhci", MODULE_CAP_DRIVER);
