/*
 * guest_tap.c - A guest that transmits an ARP request over virtio-net
 * (docs/kernel-services/virtualization/testing.md).
 *
 * el2-tap-host bridges this guest's transmit queue to a real tap in the
 * host stack. The guest brings the device up, sets up the transmit queue,
 * and transmits a broadcast ARP request for the host's tap address. The
 * test then checks the host stack answered on the tap -- so the guest's
 * frame crossed virtio-net, the bridge and into the real stack. The reply
 * reaching the guest's receive queue is el2-virtq-net's and the tap
 * selftest's; this proves the outbound path end to end.
 *
 *   hvc 1: x1 = MagicValue, x2 = DeviceID
 *   hvc 2: x1 = 1 once the ARP request has been transmitted
 */
#include <stdint.h>

#define VIRTIO_BASE 0x0A000200ull
#define R(off)  (*(volatile uint32_t *)(VIRTIO_BASE + (off)))

#define MAGIC 0x000
#define DEVICE_ID 0x008
#define DEV_FEAT 0x010
#define DEV_FEAT_SEL 0x014
#define DRV_FEAT 0x020
#define DRV_FEAT_SEL 0x024
#define QUEUE_SEL 0x030
#define QUEUE_NUM_MAX 0x034
#define QUEUE_NUM 0x038
#define QUEUE_READY 0x044
#define QUEUE_NOTIFY 0x050
#define STATUS 0x070
#define Q_DESC_LO 0x080
#define Q_DESC_HI 0x084
#define Q_DRV_LO 0x090
#define Q_DRV_HI 0x094
#define Q_DEV_LO 0x0a0
#define Q_DEV_HI 0x0a4

#define S_ACK 1
#define S_DRIVER 2
#define S_FEATURES_OK 8
#define S_DRIVER_OK 4

#define HDR_LEN 12
#define TXDESC  0xA000ull
#define TXAVAIL 0xA800ull
#define TXUSED  0xB000ull
#define TXBUF   0xB800ull

struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };

static uint64_t hvc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0") = n, x1 __asm__("x1") = a, x2 __asm__("x2") = b, x3 __asm__("x3") = c;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

void guest_main(void)
{
    uint32_t magic = R(MAGIC), devid = R(DEVICE_ID);
    R(STATUS) = 0;
    R(STATUS) = S_ACK;
    R(STATUS) = S_ACK | S_DRIVER;
    R(DEV_FEAT_SEL) = 0; uint32_t f0 = R(DEV_FEAT);
    R(DEV_FEAT_SEL) = 1; uint32_t f1 = R(DEV_FEAT);
    R(DRV_FEAT_SEL) = 0; R(DRV_FEAT) = f0;
    R(DRV_FEAT_SEL) = 1; R(DRV_FEAT) = f1;
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK;
    R(QUEUE_SEL) = 1;                        /* transmit queue */
    uint32_t qmax = R(QUEUE_NUM_MAX);
    R(QUEUE_NUM) = qmax < 8 ? qmax : 8;
    R(Q_DESC_LO) = (uint32_t)TXDESC; R(Q_DESC_HI) = (uint32_t)(TXDESC >> 32);
    R(Q_DRV_LO) = (uint32_t)TXAVAIL; R(Q_DRV_HI) = (uint32_t)(TXAVAIL >> 32);
    R(Q_DEV_LO) = (uint32_t)TXUSED;  R(Q_DEV_HI) = (uint32_t)(TXUSED >> 32);
    R(QUEUE_READY) = 1;
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK;

    hvc3(1, magic, devid, 0);

    /* [zeroed virtio_net_hdr][Ethernet ARP request for 10.0.4.1 from
     * 52:54:00:00:00:01 / 10.0.4.15] */
    uint8_t *b = (uint8_t *)TXBUF;
    for (unsigned i = 0; i < HDR_LEN + 42; i++)
        b[i] = 0;
    uint8_t *e = b + HDR_LEN;                /* the Ethernet frame */
    for (unsigned i = 0; i < 6; i++) e[i] = 0xff;              /* dst broadcast */
    e[6] = 0x52; e[7] = 0x54; e[11] = 0x01;                    /* src 52:54:00:00:00:01 */
    e[12] = 0x08; e[13] = 0x06;                                /* ethertype ARP */
    e[15] = 1;                                                 /* htype Ethernet */
    e[16] = 0x08;                                              /* ptype IPv4 */
    e[18] = 6; e[19] = 4;                                      /* hlen, plen */
    e[21] = 1;                                                 /* op request */
    e[22] = 0x52; e[23] = 0x54; e[27] = 0x01;                  /* sha = src MAC */
    e[28] = 10; e[29] = 0; e[30] = 4; e[31] = 15;              /* spa 10.0.4.15 */
    e[38] = 10; e[39] = 0; e[40] = 4; e[41] = 1;               /* tpa 10.0.4.1 */

    struct vq_desc *d = (struct vq_desc *)TXDESC;
    d[0].addr = TXBUF; d[0].len = HDR_LEN + 42; d[0].flags = 0; d[0].next = 0;
    volatile uint16_t *a = (volatile uint16_t *)TXAVAIL;
    a[2] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    a[1] = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    R(QUEUE_NOTIFY) = 1;

    hvc3(2, 1, 0, 0);
    for (;;)
        hvc3(9, 0, 0, 0);
}
