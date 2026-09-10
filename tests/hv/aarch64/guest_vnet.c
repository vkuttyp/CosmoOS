/*
 * guest_vnet.c - A guest that drives a virtio-mmio network device
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Probe the transport, negotiate features, set up the receive (0) and
 * transmit (1) queues, post a receive buffer, transmit a known frame, and
 * -- the owner's wire being a loopback -- receive it back. It knows the
 * transport's address from the uapi constant (the real machine learns it
 * from the device tree). No GIC here: the test polls the used ring rather
 * than taking the completion interrupt.
 *
 *   hvc 1: x1 = MagicValue, x2 = DeviceID, x3 = MAC low 4 bytes
 *   hvc 2: x1 = received used length, x2..x4 = first 3 frame bytes back
 */
#include <stdint.h>

#define VIRTIO_BASE 0x0A000200ull   /* COSMO_HVM_VIRTIO1_BASE: the net window */
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
#define CONFIG 0x100

#define S_ACK 1
#define S_DRIVER 2
#define S_FEATURES_OK 8
#define S_DRIVER_OK 4

#define HDR_LEN 12
#define FRAMELEN 64

/* two rings in this guest's RAM (flat binary at 0x1000) */
#define RXDESC  0x8000ull
#define RXAVAIL 0x8800ull
#define RXUSED  0x9000ull
#define RXBUF   0x9800ull
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
static uint64_t hvc5(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    register uint64_t x0 __asm__("x0") = n, x1 __asm__("x1") = a, x2 __asm__("x2") = b,
                      x3 __asm__("x3") = c, x4 __asm__("x4") = d, x5 __asm__("x5") = e;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

static void setup_queue(uint32_t sel, uint64_t desc, uint64_t drv, uint64_t dev)
{
    R(QUEUE_SEL) = sel;
    uint32_t qmax = R(QUEUE_NUM_MAX);
    R(QUEUE_NUM) = qmax < 8 ? qmax : 8;
    R(Q_DESC_LO) = (uint32_t)desc; R(Q_DESC_HI) = (uint32_t)(desc >> 32);
    R(Q_DRV_LO) = (uint32_t)drv;  R(Q_DRV_HI) = (uint32_t)(drv >> 32);
    R(Q_DEV_LO) = (uint32_t)dev;  R(Q_DEV_HI) = (uint32_t)(dev >> 32);
    R(QUEUE_READY) = 1;
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
    uint32_t mac_lo = R(CONFIG);
    setup_queue(0, RXDESC, RXAVAIL, RXUSED);   /* receive */
    setup_queue(1, TXDESC, TXAVAIL, TXUSED);   /* transmit */
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK;

    hvc3(1, magic, devid, mac_lo);

    /* post one receive buffer (device-writable) */
    struct vq_desc *rd = (struct vq_desc *)RXDESC;
    rd[0].addr = RXBUF; rd[0].len = HDR_LEN + 1600; rd[0].flags = 2 /*WRITE*/; rd[0].next = 0;
    volatile uint16_t *ra = (volatile uint16_t *)RXAVAIL;
    ra[2] = 0;                              /* ring[0] = head 0 */
    __asm__ volatile("dsb sy" ::: "memory");
    ra[1] = 1;                              /* idx */
    __asm__ volatile("dsb sy" ::: "memory");

    /* build and transmit a known frame: [zeroed hdr][frame] */
    uint8_t *tx = (uint8_t *)TXBUF;
    for (unsigned i = 0; i < HDR_LEN; i++) tx[i] = 0;
    for (unsigned i = 0; i < FRAMELEN; i++) tx[HDR_LEN + i] = (uint8_t)(i + 0x30);
    struct vq_desc *td = (struct vq_desc *)TXDESC;
    td[0].addr = TXBUF; td[0].len = HDR_LEN + FRAMELEN; td[0].flags = 0 /*readable*/; td[0].next = 0;
    volatile uint16_t *ta = (volatile uint16_t *)TXAVAIL;
    ta[2] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    ta[1] = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    R(QUEUE_NOTIFY) = 1;                    /* notify transmit; the device serves both queues */

    /* poll the receive used ring for the looped-back frame */
    volatile uint16_t *ru = (volatile uint16_t *)RXUSED;
    for (unsigned i = 0; i < 1000000; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (ru[1] != 0)
            break;
    }
    uint32_t used_len = *(volatile uint32_t *)(RXUSED + 4 + 4);   /* used->ring[0].len */
    uint8_t *rx = (uint8_t *)(RXBUF + HDR_LEN);                   /* past the received header */
    hvc5(2, used_len, rx[0], rx[1], rx[2], rx[3]);
    for (;;)
        hvc3(9, 0, 0, 0);
}
