/*
 * main.c - CosmoOS UEFI loader entry.
 *
 * Sequence:
 *   1. console + watchdog off
 *   2. locate the boot volume and read \cosmo\kernel.elf
 *   3. validate + load the ELF into low physical memory
 *   4. allocate page-table pool, bootinfo area, and a handoff stack
 *   5. build bootstrap page tables
 *   6. collect ACPI RSDP from the configuration tables
 *   7. GetMemoryMap + ExitBootServices (retry once if the map moved)
 *   8. translate the EFI memory map into cosmoboot entries
 *   9. arch finish (x86-64: NX/WP; AArch64: nothing), switch translation tables, jump
 *
 * Nothing may allocate after step 7. Every buffer step 8 writes into was
 * sized and allocated in step 4.
 */

#include "loader.h"
#include "cosmoboot.h"

EFI_SYSTEM_TABLE  *g_st;
EFI_BOOT_SERVICES *g_bs;
EFI_HANDLE         g_image;

/* Room for the memory map: 4 pages of entries after the header. Firmware
 * maps rarely exceed ~200 descriptors; this holds over 600 after merging. */
#define BOOTINFO_PAGES 5
#define BOOTINFO_MAX_ENTRIES \
    ((BOOTINFO_PAGES * PAGE_SIZE - sizeof(struct cosmoboot_info)) / sizeof(struct cosmoboot_mem_entry))

#define HANDOFF_STACK_PAGES 4

static bool guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return memcmp(a, b, sizeof(EFI_GUID)) == 0;
}

/* Read a whole file from the boot volume into a fresh low allocation of
 * the given EFI memory type. */
static EFI_STATUS read_boot_file(CHAR16 *path, uint32_t mem_type, bool *fallback, uint8_t **out,
                                 size_t *out_size)
{
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS st;

    st = g_bs->HandleProtocol(g_image, &loaded_image_guid, (void **)&li);
    if (EFI_ERROR(st))
        return st;
    st = g_bs->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(st))
        return st;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st))
        return st;
    st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) {
        root->Close(root);
        return st;
    }

    /* EFI_FILE_INFO has a variable-length name; 512 bytes is ample. */
    uint8_t info_buf[512];
    UINTN info_size = sizeof(info_buf);
    st = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
    if (EFI_ERROR(st))
        goto out;
    uint64_t file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
    if (file_size == 0 || file_size > (64ULL << 20)) {
        st = EFI_LOAD_ERROR;
        goto out;
    }

    EFI_PHYSICAL_ADDRESS buf;
    st = alloc_pages_low(BYTES_TO_PAGES(file_size), mem_type, &buf, fallback);
    if (EFI_ERROR(st))
        goto out;

    UINTN read = (UINTN)file_size;
    st = file->Read(file, &read, (void *)(uintptr_t)buf);
    if (!EFI_ERROR(st) && read != file_size)
        st = EFI_LOAD_ERROR;
    if (!EFI_ERROR(st)) {
        *out = (uint8_t *)(uintptr_t)buf;
        *out_size = (size_t)file_size;
    }

out:
    file->Close(file);
    root->Close(root);
    return st;
}

static uint64_t find_acpi_rsdp(void)
{
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
    uint64_t rsdp10 = 0;

    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &g_st->ConfigurationTable[i];
        if (guid_eq(&ct->VendorGuid, &acpi20))
            return (uint64_t)(uintptr_t)ct->VendorTable;
        if (guid_eq(&ct->VendorGuid, &acpi10))
            rsdp10 = (uint64_t)(uintptr_t)ct->VendorTable;
    }
    return rsdp10;
}

static uint32_t translate_type(uint32_t efi_type, uint64_t attr)
{
    if (attr & EFI_MEMORY_RUNTIME)
        return COSMOBOOT_MEM_FIRMWARE_RUNTIME;

    switch (efi_type) {
    case EfiConventionalMemory:        return COSMOBOOT_MEM_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:          return COSMOBOOT_MEM_LOADER_RECLAIMABLE;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:       return COSMOBOOT_MEM_FIRMWARE_RUNTIME;
    case EfiACPIReclaimMemory:         return COSMOBOOT_MEM_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:             return COSMOBOOT_MEM_ACPI_NVS;
    case EfiUnusableMemory:            return COSMOBOOT_MEM_BAD;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:   return COSMOBOOT_MEM_MMIO;
    case EfiPersistentMemory:          return COSMOBOOT_MEM_PERSISTENT;
    case EFI_MEMORY_TYPE_COSMO_KERNEL:     return COSMOBOOT_MEM_KERNEL;
    case EFI_MEMORY_TYPE_COSMO_BOOTINFO:   return COSMOBOOT_MEM_BOOTINFO;
    case EFI_MEMORY_TYPE_COSMO_PAGETABLES: return COSMOBOOT_MEM_BOOT_PAGETABLES;
    case EFI_MEMORY_TYPE_COSMO_ARCHIVE:     return COSMOBOOT_MEM_ARCHIVE;
    default:                           return COSMOBOOT_MEM_RESERVED;
    }
}

/* Translate the EFI map into cosmoboot entries, merging adjacent runs of
 * the same type. Returns the entry count, or 0 if the buffer is too small. */
static uint32_t translate_memory_map(const uint8_t *map, UINTN map_size, UINTN desc_size,
                                     struct cosmoboot_mem_entry *out, uint32_t max_entries)
{
    uint32_t n = 0;

    for (UINTN off = 0; off + desc_size <= map_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map + off);
        if (d->NumberOfPages == 0)
            continue;

        uint64_t base = d->PhysicalStart;
        uint64_t len = d->NumberOfPages * PAGE_SIZE;
        uint32_t type = translate_type(d->Type, d->Attribute);

        if (n > 0 && out[n - 1].type == type && out[n - 1].base + out[n - 1].length == base) {
            out[n - 1].length += len;
            continue;
        }
        if (n >= max_entries)
            return 0;
        out[n].base = base;
        out[n].length = len;
        out[n].type = type;
        out[n].reserved = 0;
        n++;
    }
    return n;
}

/*
 * Retype [base, base+len) inside the translated map, splitting entries as
 * needed. Used when the firmware refused the loader-defined EFI memory
 * types: the ranges then arrived as EfiLoaderData (reclaimable), which
 * the kernel would free. Returns false if the entry array is full.
 */
static bool mark_range(struct cosmoboot_mem_entry *e, uint32_t *n, uint32_t max, uint64_t base, uint64_t len,
                       uint32_t type)
{
    if (len == 0)
        return true;
    uint64_t end = base + len;

    for (uint32_t i = 0; i < *n; i++) {
        uint64_t elo = e[i].base;
        uint64_t ehi = e[i].base + e[i].length;
        if (end <= elo || base >= ehi || e[i].type == type)
            continue;

        uint64_t lo = base > elo ? base : elo;
        uint64_t hi = end < ehi ? end : ehi;
        uint32_t old_type = e[i].type;

        /* Pieces: [elo, lo) old, [lo, hi) new, [hi, ehi) old. */
        uint32_t extra = (lo > elo ? 1 : 0) + (hi < ehi ? 1 : 0);
        if (*n + extra > max)
            return false;

        /* Shift the tail to make room for the extra pieces after i. */
        for (uint32_t j = *n; j > i + 1; j--)
            e[j - 1 + extra] = e[j - 1];
        *n += extra;

        uint32_t k = i;
        if (lo > elo) {
            e[k].base = elo;
            e[k].length = lo - elo;
            e[k].type = old_type;
            k++;
        }
        e[k].base = lo;
        e[k].length = hi - lo;
        e[k].type = type;
        e[k].reserved = 0;
        k++;
        if (hi < ehi) {
            e[k].base = hi;
            e[k].length = ehi - hi;
            e[k].type = old_type;
            e[k].reserved = 0;
        }
        i = k - 1;
    }
    return true;
}

static const char *mem_type_name(uint32_t t)
{
    switch (t) {
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
    default:                               return "?";
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS status;
    bool type_fallback = false;

    g_st = st;
    g_bs = st->BootServices;
    g_image = image;

    console_init();
    lprintf("\n%s v%u\n", LOADER_NAME, LOADER_VERSION);

    g_bs->SetWatchdogTimer(0, 0, 0, NULL);

    /* Where is the loader itself? Its pages must stay executable. */
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *self = NULL;
    status = g_bs->HandleProtocol(g_image, &loaded_image_guid, (void **)&self);
    if (EFI_ERROR(status))
        die("cannot locate own loaded image", status);

    /* --- kernel file --- */
    uint8_t *file = NULL;
    size_t file_size = 0;
    static CHAR16 kernel_path[] = KERNEL_PATH;
    status = read_boot_file(kernel_path, EfiLoaderData, NULL, &file, &file_size);
    if (EFI_ERROR(status))
        die("cannot read \\cosmo\\kernel.elf from the boot volume", status);
    lprintf("kernel: %u bytes read\n", (unsigned)file_size);

    /* --- boot archive (optional): init and boot-time kernel modules --- */
    uint8_t *archive = NULL;
    size_t archive_size = 0;
    static CHAR16 archive_path[] = ARCHIVE_PATH;
    status = read_boot_file(archive_path, EFI_MEMORY_TYPE_COSMO_ARCHIVE, &type_fallback, &archive, &archive_size);
    if (EFI_ERROR(status)) {
        lputs("archive: \\cosmo\\boot.tar not found; the kernel will run without init or modules\n");
        archive = NULL;
        archive_size = 0;
    } else {
        lprintf("archive: %u bytes read\n", (unsigned)archive_size);
    }

    struct elf_image img;
    status = elf_load(file, file_size, &img, &type_fallback);
    if (EFI_ERROR(status))
        die("kernel ELF rejected", status);
    lprintf("kernel: virt 0x%016llx-0x%016llx -> phys 0x%llx, entry 0x%llx, %u segments\n",
            (unsigned long long)img.virt_base, (unsigned long long)img.virt_end,
            (unsigned long long)img.phys_base, (unsigned long long)img.entry, img.segment_count);

    /* --- allocations that must precede ExitBootServices --- */
    if (!cpu_prepare())
        die("this processor cannot run the kernel", EFI_UNSUPPORTED);
    /* A snapshot of the memory map for the table builder (RAM versus
     * device attributes on AArch64); the map handed to the kernel is
     * fetched again after the last allocation. */
    UINTN pre_size = 0, pre_key = 0, pre_desc = 0;
    uint32_t pre_ver = 0;
    status = g_bs->GetMemoryMap(&pre_size, NULL, &pre_key, &pre_desc, &pre_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        die("GetMemoryMap size query failed", status);
    pre_size += 8 * pre_desc;
    EFI_PHYSICAL_ADDRESS pre_phys;
    status = alloc_pages_low(BYTES_TO_PAGES(pre_size), EfiLoaderData, &pre_phys, NULL);
    if (EFI_ERROR(status))
        die("cannot allocate the memory map snapshot", status);
    status = g_bs->GetMemoryMap(&pre_size, (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)pre_phys, &pre_key, &pre_desc, &pre_ver);
    if (EFI_ERROR(status))
        die("GetMemoryMap failed", status);
    struct paging_ctx pg;
    memset(&pg, 0, sizeof(pg));
    pg.nx = true;
    pg.pool_pages = paging_pool_size(&img);
    status = alloc_pages_low(pg.pool_pages, EFI_MEMORY_TYPE_COSMO_PAGETABLES, &pg.pool_phys, &type_fallback);
    if (EFI_ERROR(status))
        die("cannot allocate page-table pool", status);

    EFI_PHYSICAL_ADDRESS info_phys;
    status = alloc_pages_low(BOOTINFO_PAGES, EFI_MEMORY_TYPE_COSMO_BOOTINFO, &info_phys, &type_fallback);
    if (EFI_ERROR(status))
        die("cannot allocate bootinfo", status);

    EFI_PHYSICAL_ADDRESS stack_phys;
    status = alloc_pages_low(HANDOFF_STACK_PAGES, EfiLoaderData, &stack_phys, NULL);
    if (EFI_ERROR(status))
        die("cannot allocate handoff stack", status);

    /* --- bootstrap page tables --- */
    status = paging_build(&pg, &img, (uint64_t)(uintptr_t)self->ImageBase, self->ImageSize,
                          (const uint8_t *)(uintptr_t)pre_phys, pre_size, pre_desc);
    if (EFI_ERROR(status))
        die("cannot build page tables", status);
    lprintf("paging: root at 0x%llx (user root 0x%llx), %u/%u pool pages used\n",
            (unsigned long long)pg.root, (unsigned long long)pg.root_user, (unsigned)pg.pool_used,
            (unsigned)pg.pool_pages);

    /* --- bootinfo header --- */
    struct cosmoboot_info *info = (struct cosmoboot_info *)(uintptr_t)info_phys;
    struct cosmoboot_mem_entry *entries = (struct cosmoboot_mem_entry *)(info + 1);

    info->magic = COSMOBOOT_MAGIC;
    info->version = COSMOBOOT_VERSION;
    info->size = sizeof(*info);
    info->arch = COSMOBOOT_ARCH_NATIVE;
    info->firmware = COSMOBOOT_FIRMWARE_UEFI;
    memcpy(info->loader_name, LOADER_NAME, sizeof(LOADER_NAME));
    info->loader_version = LOADER_VERSION;
    info->hhdm_base = BOOT_HHDM_BASE;
    info->hhdm_size = BOOT_HHDM_SIZE;
    info->kernel_phys_base = img.phys_base;
    info->kernel_virt_base = img.virt_base;
    info->kernel_size = img.virt_end - img.virt_base;
    info->boot_pagetable_root = pg.root;
    info->boot_pagetable_root_user = pg.root_user;
    info->mem_map_phys = (uint64_t)(uintptr_t)entries;
    info->mem_map_entry_size = sizeof(struct cosmoboot_mem_entry);
    info->acpi_rsdp = find_acpi_rsdp();
    info->firmware_system_table = (uint64_t)(uintptr_t)st;
    info->archive_phys = (uint64_t)(uintptr_t)archive;
    info->archive_size = archive_size;

    if (type_fallback)
        lputs("warning: firmware rejected loader memory types; kernel, bootinfo, page-table and archive "
              "ranges will be retyped from their placements\n");

    /* --- memory map + ExitBootServices --- */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    uint8_t *map = NULL;

    status = g_bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        die("GetMemoryMap size query failed", status);

    /* The allocation below changes the map; leave generous slack so the
     * post-ExitBootServices retry never needs another allocation. */
    map_size += 16 * desc_size;
    EFI_PHYSICAL_ADDRESS map_phys;
    status = alloc_pages_low(BYTES_TO_PAGES(map_size), EfiLoaderData, &map_phys, NULL);
    if (EFI_ERROR(status))
        die("cannot allocate memory map buffer", status);
    map = (uint8_t *)(uintptr_t)map_phys;
    UINTN map_capacity = map_size;

    lputs("exiting boot services\n");
    for (int attempt = 0; attempt < 2; attempt++) {
        map_size = map_capacity;
        status = g_bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status))
            die("GetMemoryMap failed", status);
        status = g_bs->ExitBootServices(g_image, map_key);
        if (!EFI_ERROR(status))
            break;
    }
    if (EFI_ERROR(status))
        die("ExitBootServices failed", status);

    /* From here on: no boot services, no firmware console. */
    console_firmware_gone();
    g_bs = NULL;

    uint32_t n = translate_memory_map(map, map_size, desc_size, entries, (uint32_t)BOOTINFO_MAX_ENTRIES);
    if (n == 0)
        die("memory map does not fit in bootinfo", EFI_BUFFER_TOO_SMALL);

    if (type_fallback) {
        /* The firmware refused loader-defined types, so these ranges are
         * currently reclaimable in the map. Retype them from the explicit
         * placements so the kernel never frees its own image. */
        uint32_t max = (uint32_t)BOOTINFO_MAX_ENTRIES;
        bool ok = mark_range(entries, &n, max, img.phys_base, img.virt_end - img.virt_base, COSMOBOOT_MEM_KERNEL) &&
                  mark_range(entries, &n, max, info_phys, BOOTINFO_PAGES * PAGE_SIZE, COSMOBOOT_MEM_BOOTINFO) &&
                  mark_range(entries, &n, max, pg.pool_phys, pg.pool_pages * PAGE_SIZE,
                             COSMOBOOT_MEM_BOOT_PAGETABLES) &&
                  mark_range(entries, &n, max, (uint64_t)(uintptr_t)archive, BYTES_TO_PAGES(archive_size) * PAGE_SIZE,
                             COSMOBOOT_MEM_ARCHIVE);
        if (!ok)
            die("memory map has no room to retype loader ranges", EFI_BUFFER_TOO_SMALL);
    }
    info->mem_map_entries = n;

    lprintf("memory map: %u entries\n", n);
    for (uint32_t i = 0; i < n; i++) {
        lprintf("  0x%016llx - 0x%016llx %s\n",
                (unsigned long long)entries[i].base,
                (unsigned long long)(entries[i].base + entries[i].length),
                mem_type_name(entries[i].type));
    }

    /* --- go --- */
    cpu_finish();

    uint64_t info_virt = BOOT_HHDM_BASE + info_phys;
    uint64_t stack_top = stack_phys + HANDOFF_STACK_PAGES * PAGE_SIZE;
    lprintf("jumping to kernel entry 0x%llx, info at 0x%llx\n",
            (unsigned long long)img.entry, (unsigned long long)info_virt);

    cpu_jump_to_kernel(&pg, stack_top, info_virt, img.entry);
}
