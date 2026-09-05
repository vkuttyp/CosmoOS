/*
 * arch/arch.h - What the loader's generic code asks of an architecture
 * (docs/boot/, docs/kernel/arch/aarch64/design.md "The loader").
 * Implemented under boot/uefi/arch/<arch>/{cpu.c,paging.c,serial.c}.
 */

#ifndef COSMO_BOOT_ARCH_H
#define COSMO_BOOT_ARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "efi.h"

struct elf_image;

#if defined(ARCH_AARCH64)
#define LOADER_ELF_MACHINE       183u
#define LOADER_ELF_MACHINE_NAME  "AArch64"
#define COSMOBOOT_ARCH_NATIVE    COSMOBOOT_ARCH_AARCH64
#else
#define LOADER_ELF_MACHINE       62u
#define LOADER_ELF_MACHINE_NAME  "x86-64"
#define COSMOBOOT_ARCH_NATIVE    COSMOBOOT_ARCH_X86_64
#endif

/* Bootstrap translation tables: one pool of zeroed pages, one or two roots. */
struct paging_ctx {
    uint64_t pool_phys;   /* first page of the page-table pool */
    UINTN    pool_pages;
    UINTN    pool_used;
    uint64_t root;        /* x86-64: the PML4 (CR3); AArch64: the TTBR1 table (higher half) */
    uint64_t root_user;   /* AArch64: the TTBR0 identity table the loader runs on; x86-64: 0 */
    bool     nx;          /* x86-64: NX available and to be set */
};

UINTN paging_pool_size(const struct elf_image *img);
/* `mmap` is a snapshot of the EFI memory map (descriptors of `desc_size`
 * bytes): AArch64 uses it to give RAM and MMIO different attributes. */
EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base,
                        uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size);

/* Before anything else: refuse a CPU the kernel cannot run on
 * (x86-64 without NX; AArch64 entered at EL2 or below EL1). */
bool cpu_prepare(void);
/* After ExitBootServices, before the jump (x86-64: enable NX and WP). */
void cpu_finish(void);
void cpu_halt(void) __attribute__((noreturn));
void cpu_jump_to_kernel(const struct paging_ctx *pg, uint64_t stack_top, uint64_t info, uint64_t entry)
    __attribute__((noreturn));

/* The early serial console (before and after the firmware console). */
void arch_serial_init(void);
bool arch_serial_present(void);
void arch_serial_putc(char c);

#endif /* COSMO_BOOT_ARCH_H */
