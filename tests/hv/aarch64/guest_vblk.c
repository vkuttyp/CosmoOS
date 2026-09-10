/*
 * guest_vblk.c - A guest that drives a virtio-mmio block device
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The virtio-mmio and virtio-blk driver dance, small and by hand: probe
 * the transport, negotiate features, set up one queue, read a sector, and
 * report what came back. It knows the transport's address from the uapi
 * constant (the real machine learns it from the device tree); everything
 * else it discovers from the device. No GIC here -- the test polls the
 * used ring rather than taking the completion interrupt, so this guest is
 * about the transport and the queue, and el2-vm-raise-spi already proved
 * the interrupt path.
 *
 *   hvc 1: x1 = MagicValue, x2 = DeviceID, x3 = capacity (sectors)
 *   hvc 2: x1 = status byte, x2..x5 = first 4 bytes the read returned
 */
#include <stdint.h>

#define VIRTIO_BASE 0x0A000000ull
#define R(off)  (*(volatile uint32_t *)(VIRTIO_BASE + (off)))

/* transport registers */
#define MAGIC 0x000
#define VERSION 0x004
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
#define INT_STATUS 0x060
#define INT_ACK 0x064
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

/* the ring, in this guest's RAM (flat binary at 0x1000; RAM is plentiful) */
#define RING 0x8000ull
#define DESC (RING + 0x000)
#define AVAIL (RING + 0x1000)
#define USED (RING + 0x2000)
#define HDR (RING + 0x3000)
#define DATA (RING + 0x3200)
#define STAT (RING + 0x3400)

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

void guest_main(void)
{
    uint32_t magic = R(MAGIC), devid = R(DEVICE_ID);
    /* the status handshake */
    R(STATUS) = 0;
    R(STATUS) = S_ACK;
    R(STATUS) = S_ACK | S_DRIVER;
    /* accept the features the device offers, in both halves */
    R(DEV_FEAT_SEL) = 0; uint32_t f0 = R(DEV_FEAT);
    R(DEV_FEAT_SEL) = 1; uint32_t f1 = R(DEV_FEAT);
    R(DRV_FEAT_SEL) = 0; R(DRV_FEAT) = f0;
    R(DRV_FEAT_SEL) = 1; R(DRV_FEAT) = f1;
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK;
    /* the capacity, from config space (8 bytes at 0x100, little-endian) */
    uint64_t cap = (uint64_t)R(CONFIG) | ((uint64_t)R(CONFIG + 4) << 32);
    /* set up queue 0 */
    R(QUEUE_SEL) = 0;
    uint32_t qmax = R(QUEUE_NUM_MAX);
    uint32_t qn = qmax < 8 ? qmax : 8;
    R(QUEUE_NUM) = qn;
    R(Q_DESC_LO) = (uint32_t)DESC; R(Q_DESC_HI) = (uint32_t)(DESC >> 32);
    R(Q_DRV_LO) = (uint32_t)AVAIL; R(Q_DRV_HI) = (uint32_t)(AVAIL >> 32);
    R(Q_DEV_LO) = (uint32_t)USED; R(Q_DEV_HI) = (uint32_t)(USED >> 32);
    R(QUEUE_READY) = 1;
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK;

    hvc3(1, magic, devid, cap);

    /* build a one-sector read of sector 1 */
    struct { uint32_t type, reserved; uint64_t sector; } *hdr = (void *)HDR;
    hdr->type = 0; hdr->reserved = 0; hdr->sector = 1;
    struct vq_desc *d = (struct vq_desc *)DESC;
    d[0].addr = HDR;  d[0].len = 16;  d[0].flags = 1;       d[0].next = 1;  /* NEXT */
    d[1].addr = DATA; d[1].len = 512; d[1].flags = 1 | 2;   d[1].next = 2;  /* NEXT|WRITE */
    d[2].addr = STAT; d[2].len = 1;   d[2].flags = 2;       d[2].next = 0;  /* WRITE */
    volatile uint16_t *avail = (volatile uint16_t *)AVAIL;   /* flags, idx, ring[] */
    avail[2] = 0;            /* ring[0] = head 0 */
    __asm__ volatile("dsb sy" ::: "memory");
    avail[1] = 1;            /* idx = 1 */
    __asm__ volatile("dsb sy" ::: "memory");
    R(QUEUE_NOTIFY) = 0;

    /* poll the used ring until the device completes it */
    volatile uint16_t *used = (volatile uint16_t *)USED;     /* flags, idx, ring[] */
    for (unsigned i = 0; i < 1000000; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (used[1] != 0)
            break;
    }
    uint8_t status = *(volatile uint8_t *)STAT;
    uint8_t *data = (uint8_t *)DATA;
    hvc5(2, status, data[0], data[1], data[2], data[3]);
    for (;;)
        hvc3(9, 0, 0, 0);
}
