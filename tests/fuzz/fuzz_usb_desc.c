/*
 * fuzz_usb_desc.c - The USB configuration descriptor parser
 * (drivers/usb/usb_desc.c) against bytes a hostile device could send
 * (docs/verification/design.md; docs/drivers/usb/invariants.md U5).
 *
 * The input is the configuration descriptor as read from the device, up
 * to USB_CONFIG_MAX bytes. Assertions: the parser never reads outside
 * the buffer (ASan; the buffer is exactly the input's size, so one byte
 * past it is poisoned), never records more than the static limits, and
 * when it accepts the input every recorded descriptor lies within it and
 * is at least its own header long.
 */

#include "fuzz.h"

#include <kernel/errno.h>

#include <drivers/usb.h>

#include <stdlib.h>
#include <string.h>

int usb_parse_config(struct usb_device *udev);

size_t fuzz_max_len(void)
{
    return USB_CONFIG_MAX + 8;   /* a little past the limit: the parser must refuse, not read */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct usb_device *udev = calloc(1, sizeof(*udev));
    FUZZ_ASSERT(udev != NULL);
    strcpy(udev->dev.name, "fuzz0-1");
    size_t n = size > USB_CONFIG_MAX ? USB_CONFIG_MAX : size;
    memcpy(udev->raw_config, data, n);
    udev->raw_len = (unsigned)size;   /* may exceed the buffer: the parser must refuse it first */

    int rc = usb_parse_config(udev);
    FUZZ_ASSERT(udev->nr_intf <= USB_MAX_INTERFACES);
    if (rc == 0) {
        FUZZ_ASSERT(size >= sizeof(struct usb_config_descriptor) && size <= USB_CONFIG_MAX);
        FUZZ_ASSERT(udev->config.bDescriptorType == USB_DT_CONFIG);
        FUZZ_ASSERT(udev->config.wTotalLength == size);
        for (unsigned i = 0; i < udev->nr_intf; i++) {
            const struct usb_interface *intf = &udev->intf[i];
            FUZZ_ASSERT(intf->desc.bDescriptorType == USB_DT_INTERFACE);
            FUZZ_ASSERT(intf->desc.bLength >= sizeof(struct usb_interface_descriptor));
            FUZZ_ASSERT(intf->desc.bAlternateSetting == 0);
            FUZZ_ASSERT(intf->nr_ep <= USB_MAX_ENDPOINTS);
            FUZZ_ASSERT(intf->udev == udev);
            for (unsigned k = 0; k < intf->nr_ep; k++) {
                FUZZ_ASSERT(intf->ep[k].desc.bDescriptorType == USB_DT_ENDPOINT);
                FUZZ_ASSERT(intf->ep[k].desc.bLength >= sizeof(struct usb_endpoint_descriptor));
            }
        }
    } else {
        FUZZ_ASSERT(rc == -EINVAL);
    }
    free(udev);
    return 0;
}

/* Seeds: the mass-storage device QEMU presents (with its SuperSpeed
 * endpoint companions, which the parser skips by length), then edge
 * cases the mutator would take a while to find. */
static size_t seed_storage(uint8_t *b, size_t cap, bool companions)
{
    uint8_t cfg[] = {
        9, USB_DT_CONFIG, 0, 0, 1, 1, 0, 0x80, 50,
        9, USB_DT_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x04, 0,
        6, 0x30, 0, 0, 0, 0,
        7, USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x04, 0,
        6, 0x30, 0, 0, 0, 0,
    };
    size_t n = sizeof(cfg);
    if (!companions) {
        /* Drop the two companion descriptors. */
        memmove(cfg + 25, cfg + 31, 7);
        n = 32;
    }
    cfg[2] = (uint8_t)n;
    cfg[3] = (uint8_t)(n >> 8);
    if (cap < n)
        return 0;
    memcpy(b, cfg, n);
    return n;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    switch (i) {
    case 0: return seed_storage(buf, cap, true);
    case 1: return seed_storage(buf, cap, false);
    case 2: {   /* a descriptor whose bLength runs past the end */
        size_t n = seed_storage(buf, cap, false);
        if (n)
            buf[n - 7] = 0x40;
        return n;
    }
    case 3: {   /* wTotalLength longer than what was read */
        size_t n = seed_storage(buf, cap, false);
        if (n)
            buf[2] = 0xff;
        return n;
    }
    case 4: {   /* a zero bLength: would loop forever if walked */
        size_t n = seed_storage(buf, cap, false);
        if (n)
            buf[9] = 0;
        return n;
    }
    case 5: {   /* more interfaces than the limit holds */
        if (cap < 9 + 9 * 6)
            return 0;
        size_t n = seed_storage(buf, cap, false);
        n = 9;
        for (unsigned k = 0; k < 6; k++) {
            uint8_t intf[9] = { 9, USB_DT_INTERFACE, (uint8_t)k, 0, 0, 0xff, 0, 0, 0 };
            memcpy(buf + n, intf, 9);
            n += 9;
        }
        buf[2] = (uint8_t)n;
        buf[3] = (uint8_t)(n >> 8);
        return n;
    }
    default:
        return 0;
    }
}
