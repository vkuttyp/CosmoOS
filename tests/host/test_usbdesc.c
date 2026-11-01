/*
 * test_usbdesc.c - Host test of the USB configuration descriptor decoder
 * (drivers/usb/usb_desc; roadmap 2.0 SA5, the virtq/USB/ACPI family): a
 * recorded digest over seeded configurations -- interfaces with alternate
 * settings and past the limits, endpoints past theirs and outside any
 * interface, unknown and companion descriptors skipped by length, and
 * lengths that are short, run past the end, leave a trailing byte or
 * disagree with the header. Every result is a line: the return, the drop
 * count and everything the decoder recorded; the lines' FNV-1a digest is
 * recorded below. The C gave it (2026-11-01), and the CeeSafe that
 * replaced it gives the same. USBDESC_DUMP=1 prints the lines;
 * USBDESC_BENCH=1 times the decoder. ASan and UBSan.
 */

#include "harness.h"

#include <kernel/errno.h>

#include <drivers/usb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROUNDS 20000
#define SEED 0xC05F5FA2026B0005ull
/* The digest of the C decoder (usb_desc.c, 2026-11-01). */
#define USBDESC_DIGEST 0x7758cfbcf4bd5d6dull

static uint64_t g_rng;
static uint64_t g_digest;
static bool g_dump;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void emit(const char *line, int n)
{
    EXPECT(n > 0 && n < 512);
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

/* One descriptor of `len` bytes at `*off`, its fields random past the two
 * the walk reads; false when it would not fit. */
static bool put(uint8_t *b, unsigned *off, unsigned len, uint8_t type)
{
    if (*off + (len ? len : 1) > USB_CONFIG_MAX)
        return false;
    b[*off] = (uint8_t)len;
    if (len >= 2)
        b[*off + 1] = type;
    for (unsigned i = 2; i < len; i++)
        b[*off + i] = (uint8_t)next();
    *off += len ? len : 1;
    return true;
}

/* A configuration: its header, then interfaces (alternate setting 0 most
 * of the time), endpoints and other descriptors, a few lengths wrong. */
static void gen_config(struct usb_device *u)
{
    memset(u, 0xa5, sizeof(*u));   /* what the decoder does not record keeps this pattern */
    uint8_t *b = u->raw_config;
    memset(b, 0, USB_CONFIG_MAX);
    unsigned off = 0;
    put(b, &off, next() % 8 ? 9 : (unsigned)(next() % 12), next() % 8 ? USB_DT_CONFIG : (uint8_t)next());
    unsigned n = (unsigned)(next() % 24);
    for (unsigned d = 0; d < n; d++) {
        unsigned r = (unsigned)(next() % 16);
        bool ok;
        if (r < 4) {
            unsigned at = off, len = next() % 8 ? 9 : (unsigned)(next() % 12);
            ok = put(b, &off, len, USB_DT_INTERFACE);
            if (ok && len >= 4)   /* alternate setting 0, or now and then another */
                b[at + 3] = next() % 4 == 0 ? (uint8_t)(1 + next() % 3) : 0;
        } else if (r < 11) {
            ok = put(b, &off, next() % 8 ? 7 : (unsigned)(next() % 10), USB_DT_ENDPOINT);
        } else if (r < 13) {
            ok = put(b, &off, 6, 0x30);   /* a SuperSpeed endpoint companion */
        } else if (r < 15) {
            ok = put(b, &off, (unsigned)(next() % 12), (uint8_t)next());
        } else {
            ok = put(b, &off, (unsigned)(next() % 256), (uint8_t)next());   /* may run past the end */
        }
        if (!ok)
            break;
    }
    if (next() % 16 == 0 && off < USB_CONFIG_MAX)
        b[off++] = (uint8_t)next();   /* a trailing byte */
    unsigned len = off;
    switch (next() % 16) {
    case 0: len = (unsigned)(next() % (USB_CONFIG_MAX + 8)); break;
    case 1: len = off + 1; break;
    case 2: len = off ? off - 1 : 0; break;
    default: break;
    }
    u->raw_len = len;
    uint16_t total = next() % 8 ? (uint16_t)len : (uint16_t)next();
    memcpy(b + 2, &total, 2);
}

static void decode(struct usb_device *u)
{
    char line[512];
    unsigned dropped = 0xa5a5a5a5u;
    int rc = usb_parse_config(u, &dropped);
    const struct usb_config_descriptor *c = &u->config;
    int n = snprintf(line, sizeof(line), "c%d %u %02x%02x%04x%02x%02x%02x%02x%02x %u", rc, rc == 0 ? dropped : 0,
                     c->bLength, c->bDescriptorType, c->wTotalLength, c->bNumInterfaces, c->bConfigurationValue,
                     c->iConfiguration, c->bmAttributes, c->bMaxPower, u->nr_intf);
    emit(line, n);
    for (unsigned i = 0; i < u->nr_intf && i < USB_MAX_INTERFACES; i++) {
        const struct usb_interface *f = &u->intf[i];
        const struct usb_interface_descriptor *d = &f->desc;
        n = snprintf(line, sizeof(line), " i%02x%02x%02x%02x%02x%02x%02x%02x%02x %u %d", d->bLength,
                     d->bDescriptorType, d->bInterfaceNumber, d->bAlternateSetting, d->bNumEndpoints,
                     d->bInterfaceClass, d->bInterfaceSubClass, d->bInterfaceProtocol, d->iInterface, f->nr_ep,
                     f->udev == u);
        emit(line, n);
        for (unsigned k = 0; k < USB_MAX_ENDPOINTS; k++) {   /* the unused ones too: zeroed with the interface */
            const struct usb_endpoint_descriptor *e = &f->ep[k].desc;
            n = snprintf(line, sizeof(line), "  e%02x%02x%02x%02x%04x%02x", e->bLength, e->bDescriptorType,
                         e->bEndpointAddress, e->bmAttributes, e->wMaxPacketSize, e->bInterval);
            emit(line, n);
        }
    }
}

static void test_digest(void)
{
    static struct usb_device u;
    g_dump = getenv("USBDESC_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++) {
        gen_config(&u);
        decode(&u);
    }
    printf("  usb_desc digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == USBDESC_DIGEST);
}

/* The configuration QEMU's mass-storage device presents: one interface,
 * two bulk endpoints, their SuperSpeed companions skipped by length. */
static void test_storage(void)
{
    static struct usb_device u;
    static const uint8_t cfg[] = {
        9, USB_DT_CONFIG, 44, 0, 1, 1, 0, 0x80, 50,
        9, USB_DT_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DT_ENDPOINT, 0x81, 0x02, 0x00, 0x04, 0,
        6, 0x30, 0, 0, 0, 0,
        7, USB_DT_ENDPOINT, 0x02, 0x02, 0x00, 0x04, 0,
        6, 0x30, 0, 0, 0, 0,
    };
    memset(&u, 0, sizeof(u));
    memcpy(u.raw_config, cfg, sizeof(cfg));
    u.raw_len = sizeof(cfg);
    unsigned dropped = 1;
    EXPECT(usb_parse_config(&u, &dropped) == 0 && dropped == 0);
    EXPECT(u.nr_intf == 1 && u.intf[0].nr_ep == 2 && u.intf[0].udev == &u);
    EXPECT(u.intf[0].desc.bInterfaceClass == 0x08 && u.intf[0].ep[0].desc.bEndpointAddress == 0x81);
    EXPECT(u.intf[0].ep[1].desc.wMaxPacketSize == 0x400 && u.config.bConfigurationValue == 1);
    u.raw_len = sizeof(cfg) - 1;   /* the header's total no longer agrees */
    EXPECT(usb_parse_config(&u, &dropped) == -EINVAL);
}

static volatile uint64_t g_sink;

/* USBDESC_BENCH=1: the decoder alone on 2,000 configurations made beforehand (best of 15). */
static int bench(void)
{
    enum { N = 2000 };
    struct usb_device *us = malloc(sizeof(*us) * N);
    if (us == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned i = 0; i < N; i++)
        gen_config(&us[i]);
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        struct timespec t0, t1;
        uint64_t acc = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < N; i++) {
            unsigned dropped = 0;
            acc += (uint64_t)usb_parse_config(&us[i], &dropped) + us[i].nr_intf + dropped;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("usb_desc bench: %.2f ns per configuration (best of 15 runs of %u)\n", best / N, N);
    free(us);
    return 0;
}

static const struct host_test tests[] = {
    { "usb_desc digest", test_digest },
    { "usb_desc storage", test_storage },
};

int main(void)
{
    if (getenv("USBDESC_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
