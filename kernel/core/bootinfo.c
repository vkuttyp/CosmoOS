/*
 * bootinfo.c - Validation and access for loader-provided boot data.
 *
 * Trust boundary: the loader is part of the trusted computing base but
 * bugs happen, and on real hardware the structure could be corrupted by a
 * firmware quirk. Every field that drives a memory access is checked
 * before use; failure is a panic with the offending value.
 */

#include <kernel/bootinfo.h>
#include <kernel/compiler.h>
#include <kernel/log.h>
#include <kernel/panic.h>

static const struct cosmoboot_info *g_info;
static const struct cosmoboot_mem_entry *g_map;
static uint32_t g_map_count;
static struct bootinfo_framebuffer g_fb;
static bool g_have_fb;
/* v7: copied out of loader memory at bootinfo_init, so nothing later
 * depends on how long the kernel keeps the bootinfo pages. */
static char g_cmdline[COSMOBOOT_CMDLINE_MAX + 1];
static uint8_t g_boot_partuuid[16];
static bool g_have_boot_partuuid;

void bootinfo_init(const struct cosmoboot_info *info)
{
    if (info == NULL)
        panic("bootinfo: loader passed a NULL info pointer");
    if (info->magic != COSMOBOOT_MAGIC)
        panic("bootinfo: bad magic 0x%016llx", (unsigned long long)info->magic);
    if (info->version != COSMOBOOT_VERSION)
        panic("bootinfo: protocol version %u, kernel expects %u", info->version, COSMOBOOT_VERSION);
    if (info->size < sizeof(*info))
        panic("bootinfo: structure size %u smaller than expected %zu", info->size, sizeof(*info));
    if (info->hhdm_base == 0 || info->hhdm_size == 0)
        panic("bootinfo: no higher-half direct map");
    if (info->mem_map_entry_size != sizeof(struct cosmoboot_mem_entry))
        panic("bootinfo: memory map entry size %u, expected %zu",
              info->mem_map_entry_size, sizeof(struct cosmoboot_mem_entry));
    if (info->mem_map_entries == 0)
        panic("bootinfo: empty memory map");

    uint64_t map_bytes = (uint64_t)info->mem_map_entries * sizeof(struct cosmoboot_mem_entry);
    if (info->mem_map_phys >= info->hhdm_size || map_bytes > info->hhdm_size - info->mem_map_phys)
        panic("bootinfo: memory map at phys 0x%llx (%llu bytes) lies outside the direct map",
              (unsigned long long)info->mem_map_phys, (unsigned long long)map_bytes);

    /* v7: the command line. Absent is both fields zero; present is a
     * NUL-terminated text of at most COSMOBOOT_CMDLINE_MAX bytes inside
     * the direct map. Anything else is a loader bug, not a choice. */
    if (info->cmdline_size > COSMOBOOT_CMDLINE_MAX)
        panic("bootinfo: command line of %u bytes (at most %u)", info->cmdline_size, COSMOBOOT_CMDLINE_MAX);
    if (info->cmdline_phys == 0 && info->cmdline_size != 0)
        panic("bootinfo: command line of %u bytes at phys 0", info->cmdline_size);
    if (info->cmdline_phys != 0) {
        if (info->cmdline_phys >= info->hhdm_size || info->cmdline_size + 1ull > info->hhdm_size - info->cmdline_phys)
            panic("bootinfo: command line at phys 0x%llx lies outside the direct map",
                  (unsigned long long)info->cmdline_phys);
        const char *text = (const char *)(uintptr_t)(info->hhdm_base + info->cmdline_phys);
        if (text[info->cmdline_size] != '\0')
            panic("bootinfo: command line is not NUL terminated at its size (%u)", info->cmdline_size);
        for (uint32_t i = 0; i < info->cmdline_size; i++)
            g_cmdline[i] = text[i];
        g_cmdline[info->cmdline_size] = '\0';
    }
    if (info->boot_flags & ~COSMOBOOT_BOOT_PARTUUID)
        panic("bootinfo: unknown boot_flags 0x%x", info->boot_flags);
    if (info->boot_flags & COSMOBOOT_BOOT_PARTUUID) {
        for (unsigned i = 0; i < 16; i++)
            g_boot_partuuid[i] = info->boot_partuuid[i];
        g_have_boot_partuuid = true;
    }

    g_info = info;
    g_map = (const struct cosmoboot_mem_entry *)(uintptr_t)(info->hhdm_base + info->mem_map_phys);
    g_map_count = info->mem_map_entries;

    /* A framebuffer the kernel may not write to is not a reason to stop
     * booting: the serial console still works, and the reason is worth
     * more on the screen of the next machine than a panic here. */
    const char *why = "";
    g_have_fb = bootinfo_fb_validate(info, &g_fb, &why);
    if (!g_have_fb && info->fb_phys != 0)
        klog(KLOG_WARN, "bootinfo: framebuffer ignored: %s", why);

    for (uint32_t i = 0; i < g_map_count; i++) {
        const struct cosmoboot_mem_entry *e = &g_map[i];
        if (e->length == 0)
            panic("bootinfo: memory map entry %u has zero length", i);
        if (!IS_ALIGNED(e->base, 4096) || !IS_ALIGNED(e->length, 4096))
            panic("bootinfo: memory map entry %u not page aligned (0x%llx + 0x%llx)",
                  i, (unsigned long long)e->base, (unsigned long long)e->length);
        if (e->base + e->length < e->base)
            panic("bootinfo: memory map entry %u overflows", i);
    }
}

const char *bootinfo_cmdline(void)
{
    return g_cmdline;
}

bool bootinfo_boot_partuuid(uint8_t out[16])
{
    if (!g_have_boot_partuuid)
        return false;
    for (unsigned i = 0; i < 16; i++)
        out[i] = g_boot_partuuid[i];
    return true;
}

const struct bootinfo_framebuffer *bootinfo_framebuffer(void)
{
    return g_have_fb ? &g_fb : NULL;
}

const struct cosmoboot_info *bootinfo_get(void)
{
    KASSERT(g_info != NULL);
    return g_info;
}

const struct cosmoboot_mem_entry *bootinfo_mem_map(uint32_t *count)
{
    KASSERT(g_info != NULL);
    if (count)
        *count = g_map_count;
    return g_map;
}

uint64_t bootinfo_usable_bytes(void)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < g_map_count; i++) {
        if (g_map[i].type == COSMOBOOT_MEM_USABLE)
            total += g_map[i].length;
    }
    return total;
}

bool bootinfo_mem_type_is_ram(uint32_t type)
{
    switch (type) {
    case COSMOBOOT_MEM_USABLE:
    case COSMOBOOT_MEM_ACPI_RECLAIMABLE:
    case COSMOBOOT_MEM_ACPI_NVS:
    case COSMOBOOT_MEM_LOADER_RECLAIMABLE:
    case COSMOBOOT_MEM_KERNEL:
    case COSMOBOOT_MEM_BOOTINFO:
    case COSMOBOOT_MEM_BOOT_PAGETABLES:
    case COSMOBOOT_MEM_FIRMWARE_RUNTIME:
    case COSMOBOOT_MEM_ARCHIVE:
        return true;
    default:
        return false;
    }
}

bool bootinfo_phys_is_ram(uint64_t pa)
{
    for (uint32_t i = 0; i < g_map_count; i++) {
        if (pa >= g_map[i].base && pa < g_map[i].base + g_map[i].length)
            return bootinfo_mem_type_is_ram(g_map[i].type);
    }
    return false;
}

uint64_t bootinfo_phys_limit(void)
{
    uint64_t limit = 0;
    for (uint32_t i = 0; i < g_map_count; i++) {
        if (!bootinfo_mem_type_is_ram(g_map[i].type))
            continue;
        uint64_t end = g_map[i].base + g_map[i].length;
        if (end > limit)
            limit = end;
    }
    return limit;
}

void *bootinfo_phys_to_virt(uint64_t phys)
{
    KASSERT(g_info != NULL);
    if (phys >= g_info->hhdm_size)
        panic("bootinfo: phys 0x%llx outside direct map (size 0x%llx)",
              (unsigned long long)phys, (unsigned long long)g_info->hhdm_size);
    return (void *)(uintptr_t)(g_info->hhdm_base + phys);
}

const char *bootinfo_mem_type_name(uint32_t type)
{
    switch (type) {
    case COSMOBOOT_MEM_USABLE:             return "usable";
    case COSMOBOOT_MEM_RESERVED:           return "reserved";
    case COSMOBOOT_MEM_ACPI_RECLAIMABLE:   return "acpi-reclaim";
    case COSMOBOOT_MEM_ACPI_NVS:           return "acpi-nvs";
    case COSMOBOOT_MEM_BAD:                return "bad";
    case COSMOBOOT_MEM_LOADER_RECLAIMABLE: return "loader";
    case COSMOBOOT_MEM_KERNEL:             return "kernel";
    case COSMOBOOT_MEM_BOOTINFO:           return "bootinfo";
    case COSMOBOOT_MEM_BOOT_PAGETABLES:    return "pagetables";
    case COSMOBOOT_MEM_FIRMWARE_RUNTIME:   return "fw-runtime";
    case COSMOBOOT_MEM_MMIO:               return "mmio";
    case COSMOBOOT_MEM_PERSISTENT:         return "persistent";
    case COSMOBOOT_MEM_ARCHIVE:            return "archive";
    default:                               return "unknown";
    }
}
