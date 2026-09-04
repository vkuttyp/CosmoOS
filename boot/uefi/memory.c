/*
 * memory.c - Physical page allocation for the loader.
 *
 * Every allocation is pinned below LOADER_ALLOC_LIMIT so the bootstrap
 * page tables (which map exactly that range) cover it. Loader-defined EFI
 * memory types let the kernel see kernel/bootinfo/page-table ranges by
 * type; a firmware that rejects them gets EfiLoaderData instead and the
 * kernel relies on the explicit ranges in cosmoboot_info.
 */

#include "loader.h"

EFI_STATUS alloc_pages_low(UINTN pages, uint32_t type, EFI_PHYSICAL_ADDRESS *out, bool *fallback_used)
{
    EFI_PHYSICAL_ADDRESS addr = LOADER_ALLOC_LIMIT - 1;
    EFI_STATUS st;

    if (pages == 0 || out == NULL)
        return EFI_INVALID_PARAMETER;

    st = g_bs->AllocatePages(AllocateMaxAddress, (EFI_MEMORY_TYPE)type, pages, &addr);
    if (st == EFI_INVALID_PARAMETER && type >= 0x80000000u) {
        addr = LOADER_ALLOC_LIMIT - 1;
        st = g_bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
        if (!EFI_ERROR(st) && fallback_used)
            *fallback_used = true;
    }
    if (EFI_ERROR(st))
        return st;

    memset((void *)(uintptr_t)addr, 0, pages * PAGE_SIZE);
    *out = addr;
    return EFI_SUCCESS;
}
