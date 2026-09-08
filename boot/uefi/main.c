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

/* Decode one channel of a PixelBitMask format: the position of the
 * lowest set bit and the number of contiguous bits above it. A mask with
 * holes in it is not a channel, and is refused by returning 0 bits. */
static void mask_to_field(uint32_t mask, uint8_t *shift, uint8_t *bits)
{
    *shift = 0;
    *bits = 0;
    if (mask == 0)
        return;
    uint32_t sh = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        sh++;
    }
    uint32_t n = 0;
    while (mask & 1u) {
        mask >>= 1;
        n++;
    }
    if (mask != 0)
        return;   /* not contiguous */
    *shift = (uint8_t)sh;
    *bits = (uint8_t)n;
}

/*
 * Record the framebuffer the firmware has already configured. The mode is
 * taken as found: SetMode is never called, because the firmware's choice
 * is known to work and choosing modes is a display driver's job.
 *
 * Leaves every field zero when there is no Graphics Output Protocol, when
 * the protocol offers no linear framebuffer (PixelBltOnly), or when the
 * mode it describes does not fit the memory it claims. A machine with no
 * framebuffer is not an error; it is a machine with a serial console.
 */
static void find_framebuffer(struct cosmoboot_info *info)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    EFI_STATUS st = g_bs->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(st) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        lputs("framebuffer: no graphics output protocol; serial console only\n");
        return;
    }

    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
    uint8_t rs = 0, rb = 0, gs = 0, gb = 0, bs = 0, bb = 0;
    uint32_t bpp = 32;

    switch (mi->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        rs = 0; gs = 8; bs = 16;
        rb = gb = bb = 8;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        bs = 0; gs = 8; rs = 16;
        rb = gb = bb = 8;
        break;
    case PixelBitMask: {
        mask_to_field(mi->PixelInformation.RedMask, &rs, &rb);
        mask_to_field(mi->PixelInformation.GreenMask, &gs, &gb);
        mask_to_field(mi->PixelInformation.BlueMask, &bs, &bb);
        if (rb == 0 || gb == 0 || bb == 0) {
            lputs("framebuffer: pixel bit mask has no usable channels; ignored\n");
            return;
        }
        uint32_t all = mi->PixelInformation.RedMask | mi->PixelInformation.GreenMask |
                       mi->PixelInformation.BlueMask | mi->PixelInformation.ReservedMask;
        uint32_t top = 0;
        for (uint32_t i = 0; i < 32; i++) {
            if (all & (1u << i))
                top = i + 1;
        }
        bpp = ALIGN_UP(top, 8);
        break;
    }
    default:
        lprintf("framebuffer: pixel format %u has no linear buffer; ignored\n",
                (unsigned)mi->PixelFormat);
        return;
    }

    uint64_t base = gop->Mode->FrameBufferBase;
    uint64_t size = gop->Mode->FrameBufferSize;
    uint64_t width = mi->HorizontalResolution;
    uint64_t height = mi->VerticalResolution;
    uint64_t per_line = mi->PixelsPerScanLine ? mi->PixelsPerScanLine : width;
    uint64_t pitch = per_line * (bpp / 8);

    if (base == 0 || size == 0 || width == 0 || height == 0 || per_line < width ||
        pitch * height > size) {
        lprintf("framebuffer: %llux%llu, %u bpp does not fit %llu bytes at 0x%llx; ignored\n",
                (unsigned long long)width, (unsigned long long)height, (unsigned)bpp,
                (unsigned long long)size, (unsigned long long)base);
        return;
    }

    info->fb_phys = base;
    info->fb_size = size;
    info->fb_width = (uint32_t)width;
    info->fb_height = (uint32_t)height;
    info->fb_pitch = (uint32_t)pitch;
    info->fb_bpp = bpp;
    info->fb_red_shift = rs;
    info->fb_red_bits = rb;
    info->fb_green_shift = gs;
    info->fb_green_bits = gb;
    info->fb_blue_shift = bs;
    info->fb_blue_bits = bb;

    lprintf("framebuffer: %ux%u, %u bpp, pitch %u, at 0x%llx (%llu KiB)\n",
            info->fb_width, info->fb_height, info->fb_bpp, info->fb_pitch,
            (unsigned long long)base, (unsigned long long)(size >> 10));
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
    case EFI_MEMORY_TYPE_COSMO_EL2:        return COSMOBOOT_MEM_EL2_STUB;
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
    pg.pool_pages = paging_pool_size(&img, pre_desc > 0 ? pre_size / pre_desc : 0);
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

    /* Every field the loader does not set must read zero: "absent" is
     * how optional data (the archive, the EL2 stub, the framebuffer) is
     * spelled, and firmware does not promise a fresh page is clean. */
    memset(info, 0, sizeof(*info));

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
    info->el2_stub_phys = cpu_el2_stub();
    info->mem_map_phys = (uint64_t)(uintptr_t)entries;
    info->mem_map_entry_size = sizeof(struct cosmoboot_mem_entry);
    info->acpi_rsdp = find_acpi_rsdp();
    info->firmware_system_table = (uint64_t)(uintptr_t)st;
    info->archive_phys = (uint64_t)(uintptr_t)archive;
    info->archive_size = archive_size;
    find_framebuffer(info);

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

    /* The EL2 stub is allocated in cpu_prepare, before this flag exists. */
    if (cpu_el2_type_fallback())
        type_fallback = true;
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
                             COSMOBOOT_MEM_ARCHIVE) &&
                  (cpu_el2_stub() == 0 ||
                   mark_range(entries, &n, max, cpu_el2_stub(), PAGE_SIZE, COSMOBOOT_MEM_EL2_STUB));
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
