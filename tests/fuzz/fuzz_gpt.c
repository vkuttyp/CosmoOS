/*
 * fuzz_gpt.c - A disk's partition table, from arbitrary bytes
 * (kernel/block/gpt.c).
 *
 * The input is the disk: the first four bytes choose its size in sectors
 * (bounded by the input), the rest are its contents, 512-byte sectors,
 * zero past the end of the input. Whatever the parser accepts must be a
 * table the kernel can register safely: every partition inside the
 * usable range, inside the disk, none overlapping another, none sharing a
 * unique GUID, at most GPT_MAX_PARTS. The seeds are tables gpt_build
 * makes, so mutation starts from ones that pass every checksum.
 */

#include "fuzz.h"

#include <kernel/gpt.h>

#include <string.h>

#define SS 512u
#define MAX_SECTORS 256u   /* 128 KiB disks: big enough for a whole table */

struct fdisk {
    const uint8_t *data;
    size_t size;
    uint64_t n;
};

static int fread_(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    struct fdisk *d = ctx;
    if (lba >= d->n || d->n - lba < count)
        return -1;
    uint8_t *out = buf;
    for (uint64_t i = 0; i < (uint64_t)count * SS; i++) {
        uint64_t off = lba * SS + i;
        out[i] = off < d->size ? d->data[off] : 0;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4)
        return 0;
    uint32_t want = (uint32_t)data[0] | (uint32_t)data[1] << 8;
    struct fdisk d = { data + 4, size - 4, 6 + want % (MAX_SECTORS - 5) };
    static uint8_t scratch[GPT_SCRATCH_BYTES];
    static struct gpt_table t;
    const char *why = NULL;
    enum gpt_result r = gpt_parse(SS, d.n, fread_, &d, scratch, &t, &why);
    if (r == GPT_BAD)
        FUZZ_ASSERT(why != NULL && why[0] != '\0');
    if (r != GPT_OK)
        return 0;
    FUZZ_ASSERT(t.nparts <= GPT_MAX_PARTS);
    FUZZ_ASSERT(t.first_usable <= t.last_usable && t.last_usable < d.n);
    for (unsigned i = 0; i < t.nparts; i++) {
        const struct gpt_part *p = &t.parts[i];
        FUZZ_ASSERT(p->first_lba <= p->last_lba);
        FUZZ_ASSERT(p->first_lba >= t.first_usable && p->last_lba <= t.last_usable);
        FUZZ_ASSERT(p->index >= 1 && p->index <= t.nr_entries);
        for (unsigned j = 0; j < i; j++) {
            FUZZ_ASSERT(p->last_lba < t.parts[j].first_lba || p->first_lba > t.parts[j].last_lba);
            FUZZ_ASSERT(memcmp(p->uuid, t.parts[j].uuid, 16) != 0);
            FUZZ_ASSERT(p->index > t.parts[j].index);
        }
    }
    return 0;
}

size_t fuzz_max_len(void)
{
    return 4 + MAX_SECTORS * SS;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    /* Seeds: a good table on disks of a few sizes, with 1..3 partitions. */
    static const uint64_t sizes[] = { 80, 128, 200 };
    if (i >= 3 * 3)
        return 0;
    uint64_t n = sizes[i % 3];
    unsigned np = 1 + i / 3;
    size_t len = 4 + (size_t)n * SS;
    if (cap < len)
        return 0;
    memset(buf, 0, len);
    uint32_t want = (uint32_t)(n - 6);
    buf[0] = (uint8_t)want;
    buf[1] = (uint8_t)(want >> 8);
    static const uint8_t guid[16] = { 7 };
    struct gpt_build_part p[3];
    memset(p, 0, sizeof(p));
    uint64_t f = gpt_build_first_usable(SS);
    for (unsigned k = 0; k < np; k++) {
        p[k].index = 1 + 2 * k;
        p[k].first_lba = f + 4 * k;
        p[k].last_lba = f + 4 * k + 3;
        memcpy(p[k].type, k ? gpt_type_cosmo_root : gpt_type_esp, 16);
        memset(p[k].uuid, (int)(0x10 + k), 16);
    }
    uint8_t *disk = buf + 4;
    if (!gpt_build(SS, n, guid, p, np, disk, disk + (n - GPT_BUILD_TAIL_SECTORS(SS)) * SS))
        return 0;
    return len;
}
