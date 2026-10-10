/*
 * gpt.c - The GUID Partition Table, read-only (gpt.h).
 *
 * Free of kernel dependencies on purpose: the kernel, the host tests, the
 * fuzzer and the installer all link this file. Every multi-byte field is
 * read through le16/le32/le64 from the byte buffer, never through a
 * struct overlay, so neither alignment nor host byte order matters.
 */

#include <kernel/gpt.h>

const uint8_t gpt_type_esp[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
/* c9b09224-e00d-418e-846a-9f1d9a61bfd5, chosen for CosmoOS (M2). */
const uint8_t gpt_type_cosmo_root[16] = {
    0x24, 0x92, 0xb0, 0xc9, 0x0d, 0xe0, 0x8e, 0x41, 0x84, 0x6a, 0x9f, 0x1d, 0x9a, 0x61, 0xbf, 0xd5,
};

/* --- CRC-32 ----------------------------------------------------------------- */

uint32_t gpt_crc32_update(uint32_t crc, const void *data, size_t len)
{
    /* Bitwise: a table would be 1 KiB of state shared by every linker of
     * this file, and the largest input is a 32 KiB entry array read once
     * per scan. */
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned k = 0; k < 8; k++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

uint32_t gpt_crc32(const void *data, size_t len)
{
    return gpt_crc32_update(0, data, len);
}

/* --- GUID text -------------------------------------------------------------- */

/* Text position of each byte, in on-disk order: the first three groups
 * are stored little-endian, so their bytes are printed back to front. */
static const unsigned char g_guid_pos[16] = { 6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34 };

void gpt_guid_format(const uint8_t guid[16], char out[GPT_GUID_TEXT])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 36; i++)
        out[i] = '-';
    for (unsigned i = 0; i < 16; i++) {
        out[g_guid_pos[i]] = hex[guid[i] >> 4];
        out[g_guid_pos[i] + 1] = hex[guid[i] & 15];
    }
    out[36] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool gpt_guid_parse(const char *text, uint8_t guid[16])
{
    for (unsigned i = 0; i < 36; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : hexval(text[i]) < 0)
            return false;
    }
    for (unsigned i = 0; i < 16; i++)
        guid[i] = (uint8_t)(hexval(text[g_guid_pos[i]]) << 4 | hexval(text[g_guid_pos[i] + 1]));
    return true;
}

/* --- the parser ------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static bool is_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

static void copy16(uint8_t *dst, const uint8_t *src)
{
    for (unsigned i = 0; i < 16; i++)
        dst[i] = src[i];
}

static bool same16(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 16; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

/* The header fields this parser uses, decoded. */
struct hdr {
    uint64_t my_lba, alt_lba, first_usable, last_usable, entries_lba;
    uint32_t nr_entries, entry_size, entries_crc;
    uint8_t disk_guid[16];
};

#define FAIL(msg) do { *why = (msg); return GPT_BAD; } while (0)

/*
 * One header, read at `lba`, checked alone: signature, revision, size,
 * its own CRC, where it says it is, and an entry array that lies on the
 * disk outside the usable range. Agreement with the other copy is the
 * caller's check.
 */
static enum gpt_result read_header(uint32_t ss, uint64_t nsectors, uint64_t lba, gpt_read_fn read, void *ctx,
                                   uint8_t *sec, struct hdr *h, const char **why)
{
    if (read(ctx, lba, 1, sec) != 0)
        return GPT_IO;
    static const uint8_t sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (unsigned i = 0; i < 8; i++)
        if (sec[i] != sig[i])
            FAIL("header signature");
    if (le32(sec + 8) >> 16 != 1)
        FAIL("header revision");
    uint32_t hsize = le32(sec + 12);
    if (hsize < 92 || hsize > ss)
        FAIL("header size");
    uint32_t want = le32(sec + 16);
    sec[16] = sec[17] = sec[18] = sec[19] = 0;
    if (gpt_crc32(sec, hsize) != want)
        FAIL("header CRC");
    h->my_lba = le64(sec + 24);
    h->alt_lba = le64(sec + 32);
    h->first_usable = le64(sec + 40);
    h->last_usable = le64(sec + 48);
    copy16(h->disk_guid, sec + 56);
    h->entries_lba = le64(sec + 72);
    h->nr_entries = le32(sec + 80);
    h->entry_size = le32(sec + 84);
    h->entries_crc = le32(sec + 88);
    if (h->my_lba != lba)
        FAIL("header is not where it says it is");
    if (h->entry_size < 128 || (h->entry_size & (h->entry_size - 1)) != 0)
        FAIL("entry size");
    if (h->nr_entries == 0 || (uint64_t)h->nr_entries * h->entry_size > GPT_MAX_ENTRY_BYTES)
        FAIL("entry array size");
    uint64_t array_sectors = ((uint64_t)h->nr_entries * h->entry_size + ss - 1) / ss;
    if (h->first_usable > h->last_usable || h->last_usable >= nsectors - 1 || h->first_usable < 2)
        FAIL("usable range");
    if (h->entries_lba < 2 || h->entries_lba >= nsectors || nsectors - h->entries_lba < array_sectors)
        FAIL("entry array outside the disk");
    uint64_t array_end = h->entries_lba + array_sectors - 1;
    if (!(array_end < h->first_usable || h->entries_lba > h->last_usable))
        FAIL("entry array overlaps the usable range");
    if (h->entries_lba <= lba && lba <= array_end)
        FAIL("entry array overlaps its header");
    return GPT_OK;
}

static enum gpt_result read_entries(uint32_t ss, const struct hdr *h, gpt_read_fn read, void *ctx, uint8_t *buf,
                                    const char **why)
{
    uint32_t bytes = h->nr_entries * h->entry_size;
    uint32_t sectors = (bytes + ss - 1) / ss;
    if (read(ctx, h->entries_lba, sectors, buf) != 0)
        return GPT_IO;
    if (gpt_crc32(buf, bytes) != h->entries_crc)
        FAIL("entry array CRC");
    return GPT_OK;
}

enum gpt_result gpt_parse(uint32_t ss, uint64_t nsectors, gpt_read_fn read, void *ctx, void *scratch,
                          struct gpt_table *out, const char **why)
{
    static const char *unused;
    if (why == NULL)
        why = &unused;
    *why = "";
    uint8_t *sec = scratch;
    uint8_t *entries = sec + 4096;
    if (ss != 512 && ss != 4096)
        FAIL("sector size");
    /* The smallest table: MBR, header, one sector of entries, one usable
     * sector, the backup's entries and header. */
    if (nsectors < 6)
        return GPT_NONE;

    /* The protective MBR: a GPT disk says so in LBA 0 (UEFI §5.2.3). A
     * disk without one is not a GPT disk -- blank, or something else --
     * and that is not an error. */
    if (read(ctx, 0, 1, sec) != 0)
        return GPT_IO;
    bool protective = false;
    if (sec[510] == 0x55 && sec[511] == 0xAA)
        for (unsigned i = 0; i < 4; i++)
            if (sec[446 + 16 * i + 4] == 0xEE)
                protective = true;
    if (!protective)
        return GPT_NONE;

    struct hdr pri, bak;
    enum gpt_result r = read_header(ss, nsectors, 1, read, ctx, sec, &pri, why);
    if (r != GPT_OK)
        return r;
    if (pri.alt_lba != nsectors - 1)
        FAIL("backup header is not on the last sector");
    r = read_header(ss, nsectors, nsectors - 1, read, ctx, sec, &bak, why);
    if (r != GPT_OK) {
        if (r == GPT_BAD)
            *why = "backup header";   /* the copy that failed, not which check */
        return r;
    }
    if (bak.alt_lba != 1)
        FAIL("backup header does not point at the primary");
    if (!same16(pri.disk_guid, bak.disk_guid) || pri.first_usable != bak.first_usable ||
        pri.last_usable != bak.last_usable || pri.nr_entries != bak.nr_entries ||
        pri.entry_size != bak.entry_size || pri.entries_crc != bak.entries_crc)
        FAIL("primary and backup headers disagree");

    /* Both arrays must match their CRC; equal CRCs over equal sizes say
     * they hold the same entries. The backup is read first so that the
     * primary's stays in the buffer for decoding. */
    r = read_entries(ss, &bak, read, ctx, entries, why);
    if (r != GPT_OK) {
        if (r == GPT_BAD)
            *why = "backup entry array CRC";
        return r;
    }
    r = read_entries(ss, &pri, read, ctx, entries, why);
    if (r != GPT_OK)
        return r;

    copy16(out->disk_guid, pri.disk_guid);
    out->first_usable = pri.first_usable;
    out->last_usable = pri.last_usable;
    out->nr_entries = pri.nr_entries;
    out->entry_size = pri.entry_size;
    out->nparts = 0;
    for (uint32_t i = 0; i < pri.nr_entries; i++) {
        const uint8_t *e = entries + (size_t)i * pri.entry_size;
        if (is_zero(e, 16))
            continue;   /* an unused entry */
        if (out->nparts == GPT_MAX_PARTS)
            FAIL("too many partitions");
        struct gpt_part *p = &out->parts[out->nparts];
        p->index = i + 1;
        copy16(p->type, e);
        copy16(p->uuid, e + 16);
        p->first_lba = le64(e + 32);
        p->last_lba = le64(e + 40);
        p->attrs = le64(e + 48);
        if (p->first_lba > p->last_lba)
            FAIL("partition ends before it starts");
        if (p->first_lba < pri.first_usable || p->last_lba > pri.last_usable)
            FAIL("partition outside the usable range");
        if (is_zero(p->uuid, 16))
            FAIL("partition without a unique GUID");
        for (unsigned j = 0; j < out->nparts; j++) {
            const struct gpt_part *q = &out->parts[j];
            if (!(p->last_lba < q->first_lba || p->first_lba > q->last_lba))
                FAIL("partitions overlap");
            if (same16(p->uuid, q->uuid))
                FAIL("two partitions share a unique GUID");
        }
        out->nparts++;
    }
    return GPT_OK;
}

/* --- the writer ----------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void zero(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = 0;
}

uint64_t gpt_build_first_usable(uint32_t ss)
{
    return GPT_BUILD_HEAD_SECTORS(ss);
}

uint64_t gpt_build_last_usable(uint32_t ss, uint64_t nsectors)
{
    return nsectors - GPT_BUILD_TAIL_SECTORS(ss) - 1;
}

static void build_header(uint8_t *h, uint32_t ss, uint64_t my, uint64_t alt, uint64_t first, uint64_t last,
                         const uint8_t disk_guid[16], uint64_t entries_lba, uint32_t entries_crc)
{
    zero(h, ss);
    static const uint8_t sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (unsigned i = 0; i < 8; i++)
        h[i] = sig[i];
    put32(h + 8, 0x00010000u);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, first);
    put64(h + 48, last);
    copy16(h + 56, disk_guid);
    put64(h + 72, entries_lba);
    put32(h + 80, GPT_BUILD_ENTRIES);
    put32(h + 84, GPT_BUILD_ENTRY_SIZE);
    put32(h + 88, entries_crc);
    put32(h + 16, gpt_crc32(h, 92));
}

bool gpt_build(uint32_t ss, uint64_t nsectors, const uint8_t disk_guid[16], const struct gpt_build_part *parts,
               unsigned nparts, uint8_t *head, uint8_t *tail)
{
    if (ss != 512 && ss != 4096)
        return false;
    uint32_t arr = GPT_BUILD_ARRAY_SECTORS(ss);
    if (nsectors < (uint64_t)GPT_BUILD_HEAD_SECTORS(ss) + GPT_BUILD_TAIL_SECTORS(ss) + 1)
        return false;
    zero(head, (size_t)GPT_BUILD_HEAD_SECTORS(ss) * ss);
    zero(tail, (size_t)GPT_BUILD_TAIL_SECTORS(ss) * ss);

    /* The protective MBR: one partition of type 0xEE over the whole disk
     * (as much of it as 32 bits of sectors can say). */
    uint8_t *mbr = head;
    uint8_t *rec = mbr + 446;
    rec[1] = 0x00;
    rec[2] = 0x02;   /* CHS 0/0/2: unused, as UEFI writes it */
    rec[4] = 0xEE;
    rec[5] = rec[6] = rec[7] = 0xFF;
    put32(rec + 8, 1);
    put32(rec + 12, nsectors - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(nsectors - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint8_t *entries = head + 2 * (size_t)ss;
    for (unsigned i = 0; i < nparts; i++) {
        const struct gpt_build_part *p = &parts[i];
        if (p->index == 0 || p->index > GPT_BUILD_ENTRIES)
            return false;
        uint8_t *e = entries + (size_t)(p->index - 1) * GPT_BUILD_ENTRY_SIZE;
        copy16(e, p->type);
        copy16(e + 16, p->uuid);
        put64(e + 32, p->first_lba);
        put64(e + 40, p->last_lba);
    }
    uint32_t crc = gpt_crc32(entries, GPT_BUILD_ENTRIES * GPT_BUILD_ENTRY_SIZE);
    uint64_t first = gpt_build_first_usable(ss);
    uint64_t last = gpt_build_last_usable(ss, nsectors);
    build_header(head + ss, ss, 1, nsectors - 1, first, last, disk_guid, 2, crc);

    /* The backup: the same entries just before the last sector, and a
     * header on the last sector that points back at the primary. */
    for (size_t i = 0; i < (size_t)arr * ss; i++)
        tail[i] = entries[i];
    build_header(tail + (size_t)arr * ss, ss, nsectors - 1, 1, first, last, disk_guid, nsectors - 1 - arr, crc);
    return true;
}
