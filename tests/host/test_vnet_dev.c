/*
 * test_vnet_dev.c - The device side of a virtio-net device, on the host
 * (docs/kernel-services/virtualization/testing.md).
 *
 * No guest and no QEMU: an in-memory "guest RAM" holds the rings a driver
 * would build, and a small in-memory "wire" is the owner's loopback. A
 * transmitted frame is handed to the wire; a wire frame fills a posted
 * receive buffer with a zeroed header and the frame; an empty receive queue
 * leaves its buffers; and a hostile ring -- the wrong direction, a frame past
 * the maximum, a buffer outside guest RAM -- is refused with no out-of-bounds
 * access.
 */
#include "harness.h"

#include <stdint.h>
#include <string.h>

#include "../../userland/system/vnet.h"

#define GRAM 0x100000u
static uint8_t g_ram[GRAM];
static unsigned g_oob;              /* guest reads/writes outside RAM: must stay 0 */

static int mem_rw(uint64_t gpa, void *buf, uint32_t len, int write)
{
    if (gpa > GRAM || len > GRAM - gpa) { g_oob++; return -1; }
    if (write) memcpy(g_ram + gpa, buf, len);
    else memcpy(buf, g_ram + gpa, len);
    return 0;
}
static int rd_guest(void *c, uint64_t gpa, void *buf, uint32_t len) { (void)c; return mem_rw(gpa, buf, len, 0); }
static int wr_guest(void *c, uint64_t gpa, const void *buf, uint32_t len) { (void)c; return mem_rw(gpa, (void *)buf, len, 1); }

/* The wire: a small loopback FIFO. wire_tx enqueues (dropping, counted, when
 * full -- a NIC drops when there is nowhere to put a frame, it does not grow
 * without bound); wire_rx dequeues. */
#define WIRE_SLOTS 4
static struct { uint8_t buf[VNET_FRAME_MAX]; uint32_t len; } g_wire[WIRE_SLOTS];
static unsigned g_wire_head, g_wire_count, g_wire_drops;

static int wire_tx(void *c, const void *frame, uint32_t len)
{
    (void)c;
    if (len > VNET_FRAME_MAX) return -1;
    if (g_wire_count == WIRE_SLOTS) { g_wire_drops++; return 0; }   /* dropped, not an error */
    unsigned slot = (g_wire_head + g_wire_count) % WIRE_SLOTS;
    memcpy(g_wire[slot].buf, frame, len);
    g_wire[slot].len = len;
    g_wire_count++;
    return 0;
}
static int wire_rx(void *c, void *buf, uint32_t max)
{
    (void)c;
    if (g_wire_count == 0) return 0;
    uint32_t len = g_wire[g_wire_head].len;
    if (len > max) len = max;                        /* truncate to the buffer (never overrun) */
    memcpy(buf, g_wire[g_wire_head].buf, len);
    g_wire_head = (g_wire_head + 1) % WIRE_SLOTS;
    g_wire_count--;
    return (int)len;
}

#define DESC 0x1000u
#define AVAIL 0x2000u
#define USED 0x3000u
#define TXBUF 0x4000u
#define RXBUF 0x5000u

static void put_desc(unsigned i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    uint8_t *d = g_ram + DESC + i * 16u;
    memcpy(d, &addr, 8); memcpy(d + 8, &len, 4); memcpy(d + 12, &flags, 2); memcpy(d + 14, &next, 2);
}
static void put16(uint64_t gpa, uint16_t v) { memcpy(g_ram + gpa, &v, 2); }

static struct vnet_io io = {
    .read_guest = rd_guest, .write_guest = wr_guest,
    .wire_tx = wire_tx, .wire_rx = wire_rx, .ctx = NULL,
};

static struct vq_queue fresh_queue(void)
{
    struct vq_queue q;
    memset(&q, 0, sizeof(q));
    q.desc_gpa = DESC; q.avail_gpa = AVAIL; q.used_gpa = USED; q.size = 8; q.ready = 1;
    return q;
}

/* one transmit buffer: header (12) + frame, device-readable, one descriptor */
static void build_tx(uint32_t framelen)
{
    put_desc(0, TXBUF, VNET_HDR_LEN + framelen, 0 /*readable, no NEXT*/, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
}
/* one receive buffer: device-writable, one descriptor of `buflen` bytes */
static void build_rx(uint32_t buflen)
{
    put_desc(0, RXBUF, buflen, VQ_DESC_F_WRITE, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
}

/* Transmit a frame, then receive it back through the loopback wire. */
static void test_tx_rx_loopback(void)
{
    memset(g_ram, 0, GRAM);
    g_oob = 0; g_wire_head = g_wire_count = g_wire_drops = 0;
    for (unsigned i = 0; i < 100; i++)
        g_ram[TXBUF + VNET_HDR_LEN + i] = (uint8_t)(i * 9 + 2);   /* the frame */
    build_tx(100);
    struct vq_queue tx = fresh_queue();
    EXPECT(vnet_process_tx(&io, &tx) == 1);          /* one frame transmitted */
    EXPECT(g_wire_count == 1);                        /* and it is on the wire */
    uint32_t id, len; memcpy(&id, g_ram + USED + 4, 4); memcpy(&len, g_ram + USED + 8, 4);
    EXPECT(id == 0 && len == 0);                      /* transmit returns nothing to the guest */

    memset(g_ram, 0, GRAM);
    build_rx(VNET_BUF_MAX);
    struct vq_queue rx = fresh_queue();
    EXPECT(vnet_process_rx(&io, &rx) == 1);          /* the frame filled the buffer */
    EXPECT(g_wire_count == 0);
    for (unsigned i = 0; i < VNET_HDR_LEN; i++)
        EXPECT(g_ram[RXBUF + i] == 0);                /* a zeroed header */
    for (unsigned i = 0; i < 100; i++)
        EXPECT(g_ram[RXBUF + VNET_HDR_LEN + i] == (uint8_t)(i * 9 + 2));
    uint32_t rid, rlen; memcpy(&rid, g_ram + USED + 4, 4); memcpy(&rlen, g_ram + USED + 8, 4);
    EXPECT(rid == 0 && rlen == VNET_HDR_LEN + 100u);
    EXPECT(g_oob == 0);
}

/* An empty wire leaves receive buffers untouched -- a pull queue. */
static void test_rx_empty(void)
{
    memset(g_ram, 0, GRAM);
    g_wire_head = g_wire_count = 0;
    build_rx(VNET_BUF_MAX);
    struct vq_queue rx = fresh_queue();
    EXPECT(vnet_process_rx(&io, &rx) == 0);          /* nothing served */
    EXPECT(rx.last_avail == 0 && rx.used_idx == 0);  /* the buffer stays available */
}

/* The wire drops when full rather than growing without bound. */
static void test_wire_drops(void)
{
    memset(g_ram, 0, GRAM);
    g_oob = 0; g_wire_head = g_wire_count = g_wire_drops = 0;
    build_tx(64);
    /* transmit the same frame more times than the wire holds, one per call */
    for (unsigned k = 0; k < WIRE_SLOTS + 3; k++) {
        struct vq_queue tx = fresh_queue();
        EXPECT(vnet_process_tx(&io, &tx) == 1);       /* the device always completes a transmit */
    }
    EXPECT(g_wire_count == WIRE_SLOTS);               /* the wire is full, not overflowing */
    EXPECT(g_wire_drops == 3);                         /* the extra frames were dropped */
}

static void test_hostile(void)
{
    struct vq_queue q;

    /* (1) a transmit buffer marked device-writable (wrong direction). */
    memset(g_ram, 0, GRAM); g_oob = 0; g_wire_head = g_wire_count = 0;
    put_desc(0, TXBUF, VNET_HDR_LEN + 8, VQ_DESC_F_WRITE, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vnet_process_tx(&io, &q) == -1);
    EXPECT(g_wire_count == 0);

    /* (2) a transmit frame larger than the maximum. */
    memset(g_ram, 0, GRAM);
    put_desc(0, TXBUF, VNET_BUF_MAX + 1, 0, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vnet_process_tx(&io, &q) == -1);

    /* (3) a receive buffer marked device-readable (wrong direction). */
    memset(g_ram, 0, GRAM); g_wire_head = g_wire_count = 0;
    (void)wire_tx(NULL, (uint8_t[8]){ 1, 2, 3, 4, 5, 6, 7, 8 }, 8);
    put_desc(0, RXBUF, VNET_BUF_MAX, 0 /*readable: wrong*/, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vnet_process_rx(&io, &q) == -1);

    /* (4) a receive buffer too small to hold a full frame. It must be refused
       before the wire is touched, so the frame is not truncated or lost --
       neither a buffer holding only part of a frame nor an exactly
       header-sized one may dequeue it. */
    memset(g_ram, 0, GRAM); g_wire_head = g_wire_count = 0;
    (void)wire_tx(NULL, (uint8_t[8]){ 1, 2, 3, 4, 5, 6, 7, 8 }, 8);
    build_rx(VNET_HDR_LEN + 64);              /* header + part of a frame, < VNET_BUF_MAX */
    q = fresh_queue();
    EXPECT(vnet_process_rx(&io, &q) == -1);
    EXPECT(g_wire_count == 1);                /* the frame was not consumed */
    build_rx(VNET_HDR_LEN);                   /* exactly header-sized */
    q = fresh_queue();
    EXPECT(vnet_process_rx(&io, &q) == -1);
    EXPECT(g_wire_count == 1);
    build_rx(VNET_HDR_LEN - 1);               /* too small even for the header */
    q = fresh_queue();
    EXPECT(vnet_process_rx(&io, &q) == -1);
    EXPECT(g_wire_count == 1);

    /* (5) a transmit buffer that points outside guest RAM. */
    memset(g_ram, 0, GRAM); g_oob = 0; g_wire_head = g_wire_count = 0;
    put_desc(0, GRAM + 0x10000, VNET_HDR_LEN + 8, 0, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vnet_process_tx(&io, &q) == -1);
    EXPECT(g_oob == 1);

    /* (6) a receive buffer that points outside guest RAM, frame waiting. */
    memset(g_ram, 0, GRAM); g_oob = 0; g_wire_head = g_wire_count = 0;
    (void)wire_tx(NULL, (uint8_t[8]){ 9 }, 8);
    put_desc(0, GRAM + 0x10000, VNET_BUF_MAX, VQ_DESC_F_WRITE, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vnet_process_rx(&io, &q) == -1);
    EXPECT(g_oob == 1);
}

static const struct host_test tests[] = {
    { "tx-rx-loopback", test_tx_rx_loopback },
    { "rx-empty", test_rx_empty },
    { "wire-drops", test_wire_drops },
    { "hostile", test_hostile },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
