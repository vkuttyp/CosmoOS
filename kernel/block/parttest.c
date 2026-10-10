/*
 * parttest.c - Partitions as block devices on the RAM disk (part.h,
 * docs/kernel/device/testing.md, "Partitions"). Debug builds.
 *
 * One test, `blk-gpt`: a table built with gpt_build on a fresh RAM disk is
 * scanned into partition devices named by entry number; a bio is kept
 * inside its partition and lands at the partition's offset; a partition in
 * use cannot be rescanned away; each way of damaging a table (either
 * header, either entry array, overlapping entries) is refused and leaves
 * the disk with no partitions; removing the disk removes them.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/part.h>
#include <kernel/printf.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if CONFIG_DEBUG

#define SS 512u
#define DISK_SECTORS 512u   /* ramblk_create(64): 64 blocks of 4 KiB */

static const uint8_t g_disk_guid[16] = { 0x11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static void part_template(struct gpt_build_part p[2], uint64_t p1_last)
{
    memset(p, 0, 2 * sizeof(*p));
    p[0].index = 1;
    p[0].first_lba = 40;
    p[0].last_lba = p1_last;
    memcpy(p[0].type, gpt_type_esp, 16);
    for (unsigned i = 0; i < 16; i++)
        p[0].uuid[i] = (uint8_t)(0xA0 + i);
    p[1].index = 3;   /* entry 2 unused: names follow the entry, not the count */
    p[1].first_lba = 100;
    p[1].last_lba = 299;
    memcpy(p[1].type, gpt_type_cosmo_root, 16);
    for (unsigned i = 0; i < 16; i++)
        p[1].uuid[i] = (uint8_t)(0xC0 + i);
}

/* Write a table whose first partition ends at `p1_last` (99 for the good
 * table; past 99 overlaps the second). */
static int write_table(struct blkdev *bd, uint8_t *head, uint8_t *tail, uint64_t p1_last)
{
    struct gpt_build_part p[2];
    part_template(p, p1_last);
    if (!gpt_build(SS, DISK_SECTORS, g_disk_guid, p, 2, head, tail))
        return -EINVAL;
    int rc = blk_write(bd, 0, GPT_BUILD_HEAD_SECTORS(SS), head);
    if (rc == 0)
        rc = blk_write(bd, DISK_SECTORS - GPT_BUILD_TAIL_SECTORS(SS), GPT_BUILD_TAIL_SECTORS(SS), tail);
    return rc;
}

/* Flip one byte of sector `lba` in place. */
static int corrupt(struct blkdev *bd, uint64_t lba, unsigned off, uint8_t *sec)
{
    int rc = blk_read(bd, lba, 1, sec);
    if (rc == 0) {
        sec[off] ^= 0x5A;
        rc = blk_write(bd, lba, 1, sec);
    }
    return rc;
}

static bool exists(const char *name)
{
    struct blkdev *b = blk_find(name);
    if (b)
        blkdev_put(b);
    return b != NULL;
}

/* Everything but the disk's removal; the caller owns the buffers and the
 * disk, so an early return leaks nothing. */
static bool gpt_body(struct blkdev *disk, uint8_t *head, uint8_t *tail, uint8_t *sec, const char *n1,
                     const char *n2, const char *n3, const char **reason)
{
    /* A blank disk has no table, which is not an error. */
    unsigned n = 99;
    CHECK(blk_part_scan(disk, &n) == 0 && n == 0 && !exists(n1));

    /* The good table: two partitions, named by entry. */
    CHECK(write_table(disk, head, tail, 99) == 0);
    CHECK(blk_part_scan(disk, &n) == 0 && n == 2);
    CHECK(exists(n1) && !exists(n2) && exists(n3));
    struct blkdev *p1 = blk_find(n1);
    struct blkdev *p3 = blk_find(n3);
    bool ok = p1 != NULL && p3 != NULL && p1->capacity == 60 && p3->capacity == 200 && blk_is_partition(p3) &&
              !blk_is_partition(disk);
    struct blk_part_info info;
    ok = ok && blk_part_info(p3, &info) && info.index == 3 && info.start == 100 &&
         strcmp(info.disk, disk->name) == 0 && memcmp(info.type, gpt_type_cosmo_root, 16) == 0;
    struct blkdev *byid = ok ? blk_find_partuuid(info.uuid) : NULL;
    ok = ok && byid == p3;
    if (byid)
        blkdev_put(byid);

    /* Bounds (D16): the last sector of p1 is its own; one past, or a
     * request that runs past, is refused before any translation. */
    ok = ok && blk_read(p1, 59, 1, sec) == 0;
    ok = ok && blk_read(p1, 60, 1, sec) == -EINVAL;
    ok = ok && blk_read(p1, 59, 2, sec) == -EINVAL;
    ok = ok && blk_read(p3, 200, 1, sec) == -EINVAL;

    /* Translation: p3's sector 0 is the disk's sector 100, and p1's last
     * write does not reach p3. */
    if (ok) {
        memset(sec, 0x3C, SS);
        ok = blk_write(p3, 0, 1, sec) == 0 && blk_flush(p3) == 0;
        memset(sec, 0xC3, SS);
        ok = ok && blk_write(p1, 59, 1, sec) == 0 && blk_read(disk, 99, 2, sec) == 0;
        ok = ok && sec[0] == 0xC3 && sec[SS - 1] == 0xC3 && sec[SS] == 0x3C && sec[2 * SS - 1] == 0x3C;
    }

    /* In use: a partition someone holds is not rescanned away. */
    ok = ok && blk_part_scan(disk, &n) == -EBUSY && exists(n1);
    if (p1)
        blkdev_put(p1);
    if (p3)
        blkdev_put(p3);
    CHECK(ok);
    CHECK(blk_part_scan(disk, &n) == 0 && n == 2);

    /* Damage, each case on a fresh good table; every refusal leaves the
     * disk with no partitions. */
    struct { uint64_t lba; unsigned off; const char *what; } damage[] = {
        { 1, 40, "primary header" },
        { 2, 16, "primary entry array" },
        { DISK_SECTORS - 1, 40, "backup header" },
        { DISK_SECTORS - 1 - GPT_BUILD_ARRAY_SECTORS(SS), 16, "backup entry array" },
    };
    for (unsigned i = 0; i < sizeof(damage) / sizeof(damage[0]); i++) {
        CHECK(write_table(disk, head, tail, 99) == 0);
        CHECK(blk_part_scan(disk, &n) == 0 && n == 2);
        CHECK(corrupt(disk, damage[i].lba, damage[i].off, sec) == 0);
        if (blk_part_scan(disk, &n) != -EINVAL || n != 0 || exists(n1) || exists(n3)) {
            kerror("selftest: blk-gpt: a damaged %s was not refused", damage[i].what);
            *reason = "a damaged table was accepted";
            return false;
        }
    }
    /* Overlapping entries under valid checksums: refused too. */
    CHECK(write_table(disk, head, tail, 120) == 0);
    CHECK(blk_part_scan(disk, &n) == -EINVAL && n == 0 && !exists(n1));

    /* A good table again, for the removal the caller checks. */
    CHECK(write_table(disk, head, tail, 99) == 0);
    CHECK(blk_part_scan(disk, &n) == 0 && n == 2);
    return true;
}

bool selftest_blk_gpt(const char **reason)
{
    struct blkdev *disk = ramblk_create(64);
    CHECK(disk != NULL);
    char n1[BLKDEV_NAME_MAX], n2[BLKDEV_NAME_MAX], n3[BLKDEV_NAME_MAX];
    ksnprintf(n1, sizeof(n1), "%s1", disk->name);
    ksnprintf(n2, sizeof(n2), "%s2", disk->name);
    ksnprintf(n3, sizeof(n3), "%s3", disk->name);
    uint8_t *head = kmalloc((size_t)GPT_BUILD_HEAD_SECTORS(SS) * SS, KMEM_ZERO);
    uint8_t *tail = kmalloc((size_t)GPT_BUILD_TAIL_SECTORS(SS) * SS, KMEM_ZERO);
    uint8_t *sec = kmalloc(2 * SS, KMEM_ZERO);   /* DMA-able: kmalloc, not the image's BSS */
    bool ok;
    if (head == NULL || tail == NULL || sec == NULL) {
        *reason = "out of memory";
        ok = false;
    } else if (disk->sector_size != SS || disk->capacity != DISK_SECTORS) {
        *reason = "the RAM disk is not 512 sectors of 512 bytes";
        ok = false;
    } else {
        ok = gpt_body(disk, head, tail, sec, n1, n2, n3, reason);
    }
    kfree(head);
    kfree(tail);
    kfree(sec);
    /* Removing the disk removes its partitions (D17). */
    ramblk_destroy(disk);
    CHECK(ok);
    CHECK(!exists(n1) && !exists(n3));
    kinfo("selftest: blk-gpt: 2 partitions by entry number, bounds and offset held, busy rescan refused, "
          "4 damaged copies and an overlap refused, partitions gone with their disk");
    return true;
}

#else
bool selftest_blk_gpt(const char **reason)
{
    (void)reason;
    return true;
}
#endif
