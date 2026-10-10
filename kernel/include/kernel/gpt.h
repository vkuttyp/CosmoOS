/*
 * gpt.h - The GUID Partition Table (UEFI 2.10 §5.3).
 *
 * A parser with no kernel dependencies: it reads sectors through a
 * callback into a buffer the caller owns, so the same file is linked into
 * the kernel (kernel/block/part.c), the host tests, the fuzzer and the
 * installer, which checks the table it has just written with it
 * (docs/kernel/device/design.md, "Partitions").
 *
 * The kernel only reads tables. gpt_build, the writer, is here so that
 * the installer and the tests produce what the parser accepts from one
 * definition of the layout; the kernel calls it only in its self-test,
 * on a RAM disk.
 *
 * A table is accepted only whole: the protective MBR, the primary header
 * and its entry array, and the backup header and its entry array must all
 * be valid and agree. There is no recovery from one copy; that is a
 * choice for a later unit (inventory), and refusing is the safe half of it.
 */

#ifndef KERNEL_GPT_H
#define KERNEL_GPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most partitions a table may use; a table with more is refused. */
#define GPT_MAX_PARTS 32u
/* The largest entry array accepted (128 entries of 128 bytes is 16 KiB). */
#define GPT_MAX_ENTRY_BYTES (32u * 1024u)
/* Scratch the parser needs: one sector (up to 4 KiB) plus the largest
 * entry array, rounded to whole 4 KiB sectors. */
#define GPT_SCRATCH_BYTES (4096u + GPT_MAX_ENTRY_BYTES)

enum gpt_result {
    GPT_OK = 0,     /* a valid table */
    GPT_NONE = 1,   /* no protective MBR: not a GPT disk at all */
    GPT_BAD = 2,    /* a GPT disk whose table is refused; `why` says why */
    GPT_IO = 3,     /* the read callback failed */
};

struct gpt_part {
    uint32_t index;          /* 1-based position in the entry array: the partition's number */
    uint64_t first_lba;      /* inclusive */
    uint64_t last_lba;       /* inclusive */
    uint8_t type[16];        /* type GUID, on-disk byte order */
    uint8_t uuid[16];        /* unique GUID (PARTUUID), on-disk byte order */
    uint64_t attrs;
};

struct gpt_table {
    uint8_t disk_guid[16];
    uint64_t first_usable, last_usable;
    uint32_t nr_entries, entry_size;
    unsigned nparts;
    struct gpt_part parts[GPT_MAX_PARTS];   /* used entries, in index order */
};

/* Read `count` sectors from `lba` into `buf`; 0 or nonzero on failure. */
typedef int (*gpt_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/*
 * Parse the table of a disk of `nsectors` sectors of `sector_size` bytes
 * (512 or 4096, else GPT_BAD). `scratch` is GPT_SCRATCH_BYTES of the
 * caller's memory (the kernel passes DMA-able memory: the reads land in
 * it). On GPT_BAD, *why names the first check that failed.
 */
enum gpt_result gpt_parse(uint32_t sector_size, uint64_t nsectors, gpt_read_fn read, void *ctx, void *scratch,
                          struct gpt_table *out, const char **why);

/* --- the writer ------------------------------------------------------------ */

/* The layout gpt_build writes: 128 entries of 128 bytes (16 KiB). */
#define GPT_BUILD_ENTRIES 128u
#define GPT_BUILD_ENTRY_SIZE 128u
/* Sectors of the entry array, and of the two regions gpt_build fills:
 * the head (protective MBR, primary header, entries: LBA 0 on) and the
 * tail (backup entries, backup header: the last sectors of the disk). */
#define GPT_BUILD_ARRAY_SECTORS(ss) ((GPT_BUILD_ENTRIES * GPT_BUILD_ENTRY_SIZE) / (ss))
#define GPT_BUILD_HEAD_SECTORS(ss) (2u + GPT_BUILD_ARRAY_SECTORS(ss))
#define GPT_BUILD_TAIL_SECTORS(ss) (GPT_BUILD_ARRAY_SECTORS(ss) + 1u)

struct gpt_build_part {
    uint32_t index;          /* 1 .. GPT_BUILD_ENTRIES: the entry it occupies */
    uint64_t first_lba, last_lba;
    uint8_t type[16], uuid[16];
};

/* The usable range a built table declares on a disk of `nsectors`. */
uint64_t gpt_build_first_usable(uint32_t ss);
uint64_t gpt_build_last_usable(uint32_t ss, uint64_t nsectors);

/*
 * Lay out a table for a disk of `nsectors` sectors of `ss` bytes (512 or
 * 4096) into `head` (GPT_BUILD_HEAD_SECTORS(ss) * ss bytes, written at
 * LBA 0) and `tail` (GPT_BUILD_TAIL_SECTORS(ss) * ss bytes, written at
 * LBA nsectors - GPT_BUILD_TAIL_SECTORS(ss)). Nothing about the parts is
 * checked: the tests build bad tables with it on purpose, and the
 * installer checks what it built by parsing it. False only for a sector
 * size, disk size or index the layout cannot express.
 */
bool gpt_build(uint32_t ss, uint64_t nsectors, const uint8_t disk_guid[16], const struct gpt_build_part *parts,
               unsigned nparts, uint8_t *head, uint8_t *tail);

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320, init and final xor ~0), the
 * checksum GPT uses. gpt_crc32("123456789") == 0xCBF43926. */
uint32_t gpt_crc32(const void *data, size_t len);
uint32_t gpt_crc32_update(uint32_t crc, const void *data, size_t len);

/* GUID text "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 characters, any
 * case on input, lower case on output) in the mixed-endian GPT byte order:
 * the first three groups little-endian, the last two as written. */
#define GPT_GUID_TEXT 37u   /* with the NUL */
void gpt_guid_format(const uint8_t guid[16], char out[GPT_GUID_TEXT]);
/* Exactly 36 characters at `text` (the next character is not looked at).
 * False if any is not where a hex digit or a dash belongs. */
bool gpt_guid_parse(const char *text, uint8_t guid[16]);

/* Well-known partition type GUIDs, on-disk byte order. */
extern const uint8_t gpt_type_esp[16];          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const uint8_t gpt_type_cosmo_root[16];   /* the CosmoOS root (docs/kernel/device/design.md) */

#endif /* KERNEL_GPT_H */
