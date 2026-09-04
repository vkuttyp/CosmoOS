/*
 * bootarchive.c - ustar boot archive parsing.
 *
 * Format as written by scripts/mkbootarchive.py: 512-byte headers,
 * octal ASCII sizes, data padded to 512, two zero blocks at the end,
 * regular files only, no prefix field. Every header is validated
 * against the archive bounds before anything is read through it.
 */

#include <kernel/bootarchive.h>
#include <kernel/bootinfo.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#define TAR_BLOCK 512u

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

STATIC_ASSERT(sizeof(struct tar_header) == TAR_BLOCK, "tar header is one block");

static struct bootarchive_entry g_entries[BOOTARCHIVE_MAX_ENTRIES];
static unsigned g_count;
static bool g_initialized;

/* Octal field: digits, optionally NUL/space terminated. false on junk. */
static bool parse_octal(const char *field, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && field[i] == ' ')
        i++;
    if (i == len || field[i] == '\0')
        return false;
    for (; i < len && field[i] != '\0' && field[i] != ' '; i++) {
        if (field[i] < '0' || field[i] > '7')
            return false;
        if (v > (UINT64_MAX >> 3))
            return false;
        v = (v << 3) | (uint64_t)(field[i] - '0');
    }
    *out = v;
    return true;
}

static bool block_is_zero(const uint8_t *b)
{
    for (unsigned i = 0; i < TAR_BLOCK; i++) {
        if (b[i] != 0)
            return false;
    }
    return true;
}

static bool checksum_ok(const struct tar_header *h)
{
    uint64_t want;
    if (!parse_octal(h->chksum, sizeof(h->chksum), &want))
        return false;
    const uint8_t *b = (const uint8_t *)h;
    uint64_t sum = 0;
    for (unsigned i = 0; i < TAR_BLOCK; i++) {
        bool in_chksum = i >= offsetof(struct tar_header, chksum) &&
                         i < offsetof(struct tar_header, chksum) + sizeof(h->chksum);
        sum += in_chksum ? ' ' : b[i];
    }
    return sum == want;
}

void bootarchive_init(void)
{
    KASSERT(!g_initialized);
    const struct cosmoboot_info *info = bootinfo_get();
    g_initialized = true;

    if (info->archive_size == 0) {
        kwarn("bootarchive: no boot archive");
        return;
    }
    if (info->archive_size % TAR_BLOCK != 0)
        panic("bootarchive: size %llu is not a multiple of %u", (unsigned long long)info->archive_size,
              TAR_BLOCK);

    const uint8_t *base = bootinfo_phys_to_virt(info->archive_phys);
    size_t size = (size_t)info->archive_size;
    size_t off = 0;

    while (off + TAR_BLOCK <= size) {
        const uint8_t *block = base + off;
        if (block_is_zero(block))
            break;
        const struct tar_header *h = (const struct tar_header *)block;

        if (!checksum_ok(h))
            panic("bootarchive: bad header checksum at offset %zu", off);
        if (memcmp(h->magic, "ustar", 5) != 0)
            panic("bootarchive: not a ustar header at offset %zu", off);
        if (memchr(h->name, '\0', sizeof(h->name)) == NULL)
            panic("bootarchive: unterminated name at offset %zu", off);
        if (h->prefix[0] != '\0')
            panic("bootarchive: prefix field unsupported (entry %s)", h->name);
        uint64_t fsize;
        if (!parse_octal(h->size, sizeof(h->size), &fsize))
            panic("bootarchive: bad size field (entry %s)", h->name);
        if (fsize > size - off - TAR_BLOCK)
            panic("bootarchive: entry %s runs past the archive end", h->name);
        if (h->typeflag != '0' && h->typeflag != '\0')
            panic("bootarchive: entry %s is not a regular file (type %c)", h->name, h->typeflag);
        if (h->name[0] == '\0' || h->name[0] == '/' || strstr(h->name, "..") != NULL)
            panic("bootarchive: entry name '%s' rejected", h->name);
        if (g_count == BOOTARCHIVE_MAX_ENTRIES)
            panic("bootarchive: more than %u entries", BOOTARCHIVE_MAX_ENTRIES);
        for (unsigned i = 0; i < g_count; i++) {
            if (strcmp(g_entries[i].name, h->name) == 0)
                panic("bootarchive: duplicate entry %s", h->name);
        }

        struct bootarchive_entry *e = &g_entries[g_count++];
        strlcpy(e->name, h->name, sizeof(e->name));
        e->data = block + TAR_BLOCK;
        e->size = (size_t)fsize;
        kdebug("bootarchive: %-24s %zu bytes", e->name, e->size);

        off += TAR_BLOCK + ALIGN_UP((size_t)fsize, TAR_BLOCK);
    }

    kinfo("bootarchive: %u entries in %llu KiB", g_count, (unsigned long long)(info->archive_size >> 10));
}

bool bootarchive_find(const char *name, const void **data, size_t *size)
{
    KASSERT(g_initialized);
    for (unsigned i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            if (data)
                *data = g_entries[i].data;
            if (size)
                *size = g_entries[i].size;
            return true;
        }
    }
    return false;
}

unsigned bootarchive_count(void)
{
    KASSERT(g_initialized);
    return g_count;
}

const struct bootarchive_entry *bootarchive_entry(unsigned index)
{
    KASSERT(g_initialized);
    return index < g_count ? &g_entries[index] : NULL;
}
