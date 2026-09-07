/*
 * usb_desc.c - The configuration descriptor parser (invariant U5).
 *
 * Its own file so the host fuzz target (tests/fuzz/fuzz_usb_desc.c) can
 * compile exactly this code against hostile input: the bytes come from
 * the device, and the parser must never read past what was received or
 * record more than the static limits hold.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <drivers/usb.h>

/*
 * Walk the configuration by bLength. Every descriptor must fit in what
 * was read and be at least its own header long; the walk refuses
 * anything else rather than reading past the buffer. Interfaces and
 * endpoints beyond the limits are counted in the log and dropped;
 * alternate settings other than 0 and unknown descriptor types are
 * skipped by their bLength.
 */
int usb_parse_config(struct usb_device *udev)
{
    const uint8_t *p = udev->raw_config;
    unsigned len = udev->raw_len, off = 0, dropped = 0;
    struct usb_interface *cur = NULL;
    udev->nr_intf = 0;

    if (len < sizeof(struct usb_config_descriptor) || len > USB_CONFIG_MAX)
        return -EINVAL;
    memcpy(&udev->config, p, sizeof(udev->config));
    if (udev->config.bDescriptorType != USB_DT_CONFIG || udev->config.bLength < sizeof(udev->config) ||
        udev->config.wTotalLength != len)
        return -EINVAL;

    while (off + 2 <= len) {
        uint8_t dlen = p[off], dtype = p[off + 1];
        if (dlen < 2 || off + dlen > len)
            return -EINVAL;
        if (dtype == USB_DT_INTERFACE && dlen >= sizeof(struct usb_interface_descriptor)) {
            const struct usb_interface_descriptor *d = (const void *)(p + off);
            if (d->bAlternateSetting != 0) {
                cur = NULL;   /* alternate settings are recorded nowhere; their endpoints are skipped */
            } else if (udev->nr_intf < USB_MAX_INTERFACES) {
                cur = &udev->intf[udev->nr_intf++];
                memset(cur, 0, sizeof(*cur));
                memcpy(&cur->desc, d, sizeof(cur->desc));
                cur->udev = udev;
            } else {
                cur = NULL;
                dropped++;
            }
        } else if (dtype == USB_DT_ENDPOINT && dlen >= sizeof(struct usb_endpoint_descriptor)) {
            if (cur != NULL) {
                if (cur->nr_ep < USB_MAX_ENDPOINTS)
                    memcpy(&cur->ep[cur->nr_ep++].desc, p + off, sizeof(struct usb_endpoint_descriptor));
                else
                    dropped++;
            }
        }
        off += dlen;
    }
    if (off != len)
        return -EINVAL;   /* a trailing byte that is not a descriptor */
    if (dropped)
        kwarn("usb: %s: %u interface(s)/endpoint(s) beyond the limits were dropped", udev->dev.name, dropped);
    return 0;
}
