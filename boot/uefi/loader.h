/*
 * loader.h - Internal declarations shared by the CosmoOS UEFI loader.
 *
 * Everything here runs before ExitBootServices unless explicitly noted.
 * The loader is single-threaded and never re-entered; there is no
 * synchronization anywhere in it.
 */

#ifndef COSMO_BOOT_LOADER_H
#define COSMO_BOOT_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "efi.h"

#define LOADER_NAME    "cosmoboot-uefi"
#define LOADER_VERSION 1u

/* Paths on the EFI system partition. */
#define KERNEL_PATH L"\\cosmo\\kernel.elf"
#define ARCHIVE_PATH L"\\cosmo\\boot.tar"
#define CMDLINE_PATH L"\\cosmo\\cmdline"
#define MANIFEST_PATH L"\\cosmo\\manifest"

/* Everything the loader allocates stays below this so it is covered by the
 * bootstrap identity map and HHDM. */
#define LOADER_ALLOC_LIMIT (4ULL << 30)
#define BOOT_HHDM_BASE     0xFFFF800000000000ULL
#define BOOT_HHDM_SIZE     (4ULL << 30)

#define PAGE_SIZE 4096ULL
#define PAGE_2M   (2ULL << 20)

#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + ((uint64_t)(a) - 1), (a))
#define BYTES_TO_PAGES(b) (ALIGN_UP((b), PAGE_SIZE) / PAGE_SIZE)

extern EFI_SYSTEM_TABLE  *g_st;
extern EFI_BOOT_SERVICES *g_bs;
extern EFI_HANDLE         g_image;

/* console.c
 * Output goes to the firmware text console (while boot services are up)
 * and to the COM1 serial port (always). Safe to call after
 * ExitBootServices only after console_firmware_gone() has been called. */
void console_init(void);
void console_firmware_gone(void);
void lputs(const char *s);
void lprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* acpi.c: walk the firmware's ACPI tables (info->acpi_rsdp), record the
 * SPCR serial console in info->console_*, and print what the firmware
 * offers. Boot services must still be up (the firmware console prints). */
struct cosmoboot_info;
void acpi_scan(struct cosmoboot_info *info);
/* After ExitBootServices: what the MADT, MCFG, GTDT and FADT say. */
void acpi_report(void);

/* firmware.c: a seed from EFI_RNG_PROTOCOL and the time from GetTime into
 * info->fw_* (protocol v10). Boot services must still be up; call it
 * after the last allocation, just before ExitBootServices. */
void firmware_gather(struct cosmoboot_info *info);

/* display.c: set the display mode the EDID prefers, when the firmware
 * offers it with a linear framebuffer; called before the framebuffer is
 * read for the boot info. */
void display_choose_mode(void);
/* The Graphics Output Protocol both of those use, and its handle; NULL if none. */
EFI_GRAPHICS_OUTPUT_PROTOCOL *display_gop(EFI_HANDLE *handle);
/* After ExitBootServices, on the serial port: what display_choose_mode saw and did. */
void display_report(void);
/* After ExitBootServices, on the serial port: what firmware_gather found. */
void firmware_report(const struct cosmoboot_info *info);

/* Print a message and stop. Returns to firmware while boot services are
 * available, halts the CPU forever otherwise. */
void die(const char *what, EFI_STATUS status) __attribute__((noreturn));

/* string.c - freestanding replacements, may be emitted by the compiler. */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* memory.c
 * Allocate `pages` physical pages below LOADER_ALLOC_LIMIT with the given
 * EFI memory type. Falls back to EfiLoaderData if the firmware rejects a
 * loader-defined type; `*fallback_used` is set in that case. */
EFI_STATUS alloc_pages_low(UINTN pages, uint32_t type, EFI_PHYSICAL_ADDRESS *out, bool *fallback_used);

/* elf.c */
#define ELF_MAX_SEGMENTS 8

struct elf_segment {
    uint64_t vaddr;  /* page aligned start */
    uint64_t size;   /* page multiple */
    uint32_t flags;  /* PF_R / PF_W / PF_X */
    uint32_t reserved;
};

struct elf_image {
    uint64_t entry;
    uint64_t virt_base;      /* lowest PT_LOAD vaddr, page aligned */
    uint64_t virt_end;       /* highest PT_LOAD end, page aligned */
    uint64_t phys_base;      /* where virt_base landed in RAM */
    uint32_t note_version;   /* from .note.cosmoboot, 0 if absent */
    uint32_t segment_count;
    struct elf_segment segments[ELF_MAX_SEGMENTS];
};

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

/* Validate an x86-64 ELF64 executable and copy its PT_LOAD segments into
 * freshly allocated physical memory. `img` describes the result. */
EFI_STATUS elf_load(const uint8_t *file, size_t size, struct elf_image *img, bool *fallback_used);

/* verify.c - verified boot (docs/boot/design.md, "Verified boot").
 * verify_manifest() checks \cosmo\manifest (NULL when it could not be
 * read) and fills key_id; verify_file() checks one file, by its
 * COSMO_MANIFEST_* index, once it is in memory (NULL when it could not be
 * read). Both die() on any mismatch, naming the file. verify_complete()
 * is true when every file was checked. In a VERIFY_BOOT=0 loader they
 * check nothing and verify_complete() is false. */
void verify_manifest(const uint8_t *file, size_t size, uint8_t key_id[8]);
void verify_file(unsigned which, const uint8_t *data, size_t size);
bool verify_complete(void);
const char *verify_path(unsigned which);

/* paging.c */
#include "arch/arch.h"

#endif /* COSMO_BOOT_LOADER_H */
