/*
 * test_vblk_dev.c - The device side of a virtio-blk virtqueue, on the host
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The reusable core of the root-filesystem unit, proved with no guest and
 * no QEMU: an in-memory "guest RAM" holds a ring a driver would build, an
 * in-memory "disk" holds known bytes, and vblk_process serves a read into
 * the guest's buffer and completes it on the used ring. Then the part the
 * device exists to get right: a descriptor ring written by a hostile guest
 * -- an index out of range, a chain that loops, a buffer that points
 * outside the guest -- is refused without a read out of bounds.
 */
#include "harness.h"

#include <stdint.h>
#include <string.h>

#include "../../userland/system/vblk.h"

/* 1 MiB of "guest RAM" at gpa 0, and a small "disk" of known bytes. */
#define GRAM 0x100000u
#define DISK_SECTORS 8u
static uint8_t g_ram[GRAM];
static uint8_t g_disk[DISK_SECTORS * VBLK_SECTOR];
static unsigned g_oob_reads;   /* guest reads/writes that fell outside RAM: must stay 0 */

static int mem_rw(uint64_t gpa, void *buf, uint32_t len, int write)
{
    if (gpa > GRAM || len > GRAM - gpa) {   /* the backstop: a descriptor may point anywhere */
        g_oob_reads++;
        return -1;
    }
    if (write)
        memcpy(g_ram + gpa, buf, len);
    else
        memcpy(buf, g_ram + gpa, len);
    return 0;
}
static int rd_guest(void *c, uint64_t gpa, void *buf, uint32_t len) { (void)c; return mem_rw(gpa, buf, len, 0); }
static int wr_guest(void *c, uint64_t gpa, const void *buf, uint32_t len) { (void)c; return mem_rw(gpa, (void *)buf, len, 1); }
static int disk_rd(void *c, uint64_t off, void *buf, uint32_t len)
{
    (void)c;
    if (off + len > sizeof(g_disk))
        return -1;
    memcpy(buf, g_disk + off, len);
    return 0;
}

/* A ring built in g_ram: descriptors at 0x1000, avail at 0x2000, used at
 * 0x3000, request header at 0x4000, data buffer at 0x5000, status at
 * 0x6000. Helpers write little-endian into g_ram. */
#define DESC 0x1000u
#define AVAIL 0x2000u
#define USED 0x3000u
#define HDR 0x4000u
#define DATA 0x5000u
#define STATUS 0x6000u

static void put_desc(unsigned i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    uint8_t *d = g_ram + DESC + i * 16u;
    memcpy(d, &addr, 8); memcpy(d + 8, &len, 4); memcpy(d + 12, &flags, 2); memcpy(d + 14, &next, 2);
}
static void put16(uint64_t gpa, uint16_t v) { memcpy(g_ram + gpa, &v, 2); }

static struct vblk_io io = {
    .read_guest = rd_guest, .write_guest = wr_guest, .disk_read = disk_rd, .ctx = NULL,
    .capacity_sectors = DISK_SECTORS,
};

static struct vblk_queue fresh_queue(void)
{
    struct vblk_queue q;
    memset(&q, 0, sizeof(q));
    q.desc_gpa = DESC; q.avail_gpa = AVAIL; q.used_gpa = USED; q.size = 8; q.ready = 1;
    return q;
}

/* A standard read request: header (read), data buffer (write), status (write). */
static void build_read_req(uint64_t sector, uint32_t data_len)
{
    struct { uint32_t type, reserved; uint64_t sector; } hdr = { VIRTIO_BLK_T_IN, 0, sector };
    memcpy(g_ram + HDR, &hdr, sizeof(hdr));
    put_desc(0, HDR, sizeof(hdr), 1 /*NEXT*/, 1);
    put_desc(1, DATA, data_len, 1 | 2 /*NEXT|WRITE*/, 2);
    put_desc(2, STATUS, 1, 2 /*WRITE*/, 0);
    put16(AVAIL + 4, 0);        /* avail->ring[0] = head 0 */
    put16(AVAIL + 2, 1);        /* avail->idx = 1 */
}

static void test_read(void)
{
    memset(g_ram, 0, GRAM);
    g_oob_reads = 0;
    for (unsigned i = 0; i < sizeof(g_disk); i++)
        g_disk[i] = (uint8_t)(i * 7 + 1);    /* known pattern */
    build_read_req(2 /*sector*/, VBLK_SECTOR);
    struct vblk_queue q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == 1);       /* one request served */
    /* the guest's data buffer holds sector 2 of the disk */
    EXPECT(memcmp(g_ram + DATA, g_disk + 2 * VBLK_SECTOR, VBLK_SECTOR) == 0);
    EXPECT(g_ram[STATUS] == VIRTIO_BLK_S_OK);
    /* used->idx advanced to 1, ring[0] = { head 0, len 513 } */
    uint16_t uidx; memcpy(&uidx, g_ram + USED + 2, 2);
    EXPECT(uidx == 1);
    uint32_t id, len; memcpy(&id, g_ram + USED + 4, 4); memcpy(&len, g_ram + USED + 8, 4);
    EXPECT(id == 0 && len == VBLK_SECTOR + 1u);
    EXPECT(g_oob_reads == 0);
    /* run again with nothing new available: nothing served */
    EXPECT(vblk_process(&io, &q) == 0);
}

static void test_write_is_refused(void)
{
    memset(g_ram, 0, GRAM);
    struct { uint32_t type, reserved; uint64_t sector; } hdr = { VIRTIO_BLK_T_OUT, 0, 0 };
    memcpy(g_ram + HDR, &hdr, sizeof(hdr));
    put_desc(0, HDR, sizeof(hdr), 1, 1);
    put_desc(1, DATA, VBLK_SECTOR, 1 | 2, 2);
    put_desc(2, STATUS, 1, 2, 0);
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    struct vblk_queue q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == 1);        /* completed... */
    EXPECT(g_ram[STATUS] == VIRTIO_BLK_S_UNSUPP);  /* ...as unsupported, a read-only device */
}

static void test_hostile(void)
{
    struct vblk_queue q;

    /* (1) an available head index past the ring. */
    memset(g_ram, 0, GRAM); g_oob_reads = 0;
    put16(AVAIL + 4, 99); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == -1);
    EXPECT(g_oob_reads == 0);

    /* (2) a descriptor chain that loops (0 -> 1 -> 0 ...). */
    memset(g_ram, 0, GRAM); g_oob_reads = 0;
    { struct { uint32_t t, r; uint64_t s; } h = { VIRTIO_BLK_T_IN, 0, 0 }; memcpy(g_ram + HDR, &h, sizeof(h)); }
    put_desc(0, HDR, 16, 1, 1);
    put_desc(1, DATA, VBLK_SECTOR, 1 | 2, 0);   /* NEXT back to 0: a loop */
    put16(AVAIL + 4, 0); put16(AVAIL + 2, 1);
    q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == -1);
    EXPECT(g_oob_reads == 0);

    /* (3) a data buffer that points outside guest RAM. */
    memset(g_ram, 0, GRAM); g_oob_reads = 0;
    build_read_req(0, VBLK_SECTOR);
    put_desc(1, GRAM + 0x10000, VBLK_SECTOR, 1 | 2, 2);   /* past the end of RAM */
    q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == -1);
    EXPECT(g_oob_reads == 1);                    /* the write was attempted and the backstop caught it */

    /* (4) a read past the end of the disk is an I/O error, not a crash. */
    memset(g_ram, 0, GRAM); g_oob_reads = 0;
    build_read_req(DISK_SECTORS, VBLK_SECTOR);   /* one sector past the disk */
    q = fresh_queue();
    EXPECT(vblk_process(&io, &q) == 1);          /* completed, as an error */
    EXPECT(g_ram[STATUS] == VIRTIO_BLK_S_IOERR);

    /* (5) a queue the guest never marked ready. */
    memset(g_ram, 0, GRAM);
    build_read_req(0, VBLK_SECTOR);
    q = fresh_queue(); q.ready = 0;
    EXPECT(vblk_process(&io, &q) == -1);
    /* and an absurd queue size. */
    q = fresh_queue(); q.size = 0;
    EXPECT(vblk_process(&io, &q) == -1);
    q = fresh_queue(); q.size = VBLK_QUEUE_MAX + 1;
    EXPECT(vblk_process(&io, &q) == -1);
}

static const struct host_test tests[] = {
    { "read", test_read },
    { "write-refused", test_write_is_refused },
    { "hostile", test_hostile },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
