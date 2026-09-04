/*
 * cosmoboot.h - The CosmoOS boot protocol.
 *
 * This header is the single contract between any bootloader and the kernel.
 * It is shared verbatim by boot/ and kernel/. It must stay:
 *
 *   - architecture-neutral in layout (fixed-width integers only, explicit
 *     padding, no pointers, no enums in struct fields),
 *   - independent of any firmware type definitions (no UEFI headers),
 *   - versioned: any layout change bumps COSMOBOOT_VERSION.
 *
 * The loader fills a `struct cosmoboot_info` in memory it owns, then jumps
 * to the kernel entry with the machine in the state documented under
 * "Entry state" below. The kernel treats every field as untrusted input and
 * validates it before use.
 *
 * Entry state (all architectures):
 *   - Firmware boot services have been exited. Only firmware runtime
 *     memory (type COSMOBOOT_MEM_FIRMWARE_RUNTIME) must be preserved.
 *   - Interrupts are disabled at the CPU.
 *   - The kernel image is mapped at its ELF link addresses with W^X
 *     permissions derived from the ELF program header flags.
 *   - Physical memory [0, hhdm_size) is also mapped read/write, no-execute,
 *     at virtual address hhdm_base (the "higher-half direct map").
 *   - The bootstrap page tables live in memory of type
 *     COSMOBOOT_MEM_BOOT_PAGETABLES. The kernel must keep them until it has
 *     installed its own tables.
 *   - The kernel's ELF .bss has been zeroed.
 *   - The first argument register carries the HHDM virtual address of the
 *     info structure. A small loader-owned stack is valid but the kernel
 *     must switch to its own stack before touching reclaimable memory.
 *
 * x86-64 specifics: long mode, CR0.WP=1, EFER.NXE=1 when the CPU supports
 * NX, RDI = info, RSP = loader stack, all segment selectors are flat and
 * belong to the firmware GDT (the kernel installs its own immediately).
 * The identity map of [0, hhdm_size) is also present so the loader can
 * execute its final jump; the kernel must not rely on it.
 */

#ifndef COSMOBOOT_H
#define COSMOBOOT_H

/* Constants usable from assembly (the kernel's entry file emits the
 * protocol note). Everything below the __ASSEMBLER__ guard is C only. */

#define COSMOBOOT_VERSION 1

/* ELF note carried by the kernel so the loader can verify protocol version.
 * Name "COSMO\0", type COSMOBOOT_NOTE_TYPE, desc = uint32_t version. */
#define COSMOBOOT_NOTE_NAME "COSMO"
#define COSMOBOOT_NOTE_TYPE 1

#ifndef __ASSEMBLER__

#include <stdint.h>

/* "COSMOBT1" little-endian. */
#define COSMOBOOT_MAGIC   0x3154424F4D534F43ULL

/* Memory map entry types. Ordering is not significant; values are stable. */
#define COSMOBOOT_MEM_USABLE             1u  /* free RAM, kernel may use freely */
#define COSMOBOOT_MEM_RESERVED           2u  /* never touch */
#define COSMOBOOT_MEM_ACPI_RECLAIMABLE   3u  /* usable after ACPI tables consumed */
#define COSMOBOOT_MEM_ACPI_NVS           4u  /* must be preserved across sleep */
#define COSMOBOOT_MEM_BAD                5u  /* known-bad RAM */
#define COSMOBOOT_MEM_LOADER_RECLAIMABLE 6u  /* loader code/data, firmware boot services */
#define COSMOBOOT_MEM_KERNEL             7u  /* the loaded kernel image */
#define COSMOBOOT_MEM_BOOTINFO           8u  /* this structure and its arrays */
#define COSMOBOOT_MEM_BOOT_PAGETABLES    9u  /* bootstrap page tables */
#define COSMOBOOT_MEM_FIRMWARE_RUNTIME  10u  /* firmware runtime services code/data */
#define COSMOBOOT_MEM_MMIO              11u  /* memory-mapped I/O */
#define COSMOBOOT_MEM_PERSISTENT        12u  /* persistent memory, not general RAM */

#define COSMOBOOT_ARCH_X86_64  1u
#define COSMOBOOT_ARCH_AARCH64 2u

#define COSMOBOOT_FIRMWARE_UEFI 1u

#define COSMOBOOT_LOADER_NAME_MAX 32u

struct cosmoboot_mem_entry {
    uint64_t base;   /* physical address, page aligned */
    uint64_t length; /* bytes, page multiple */
    uint32_t type;   /* COSMOBOOT_MEM_* */
    uint32_t reserved;
};

struct cosmoboot_info {
    uint64_t magic;        /* COSMOBOOT_MAGIC */
    uint32_t version;      /* COSMOBOOT_VERSION */
    uint32_t size;         /* sizeof(struct cosmoboot_info) as written by the loader */

    uint32_t arch;         /* COSMOBOOT_ARCH_* */
    uint32_t firmware;     /* COSMOBOOT_FIRMWARE_* */

    char     loader_name[COSMOBOOT_LOADER_NAME_MAX]; /* NUL terminated */
    uint32_t loader_version;
    uint32_t reserved0;

    /* Higher-half direct map: virtual hhdm_base + p == physical p for
     * 0 <= p < hhdm_size. */
    uint64_t hhdm_base;
    uint64_t hhdm_size;

    /* Kernel image placement. virt_base is the lowest PT_LOAD vaddr. */
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    uint64_t kernel_size;

    /* Physical address of the top-level bootstrap page table (CR3 / TTBR). */
    uint64_t boot_pagetable_root;

    /* Memory map. Physical address of an array of cosmoboot_mem_entry. */
    uint64_t mem_map_phys;
    uint32_t mem_map_entries;
    uint32_t mem_map_entry_size; /* sizeof(struct cosmoboot_mem_entry) */

    /* Firmware tables. 0 when absent. Physical addresses. */
    uint64_t acpi_rsdp;
    uint64_t firmware_system_table; /* EFI_SYSTEM_TABLE for UEFI */

    /* Reserved for framebuffer, command line, and initrd in later versions.
     * Must be zero in version 1. */
    uint64_t reserved1[8];
};

#endif /* __ASSEMBLER__ */

#endif /* COSMOBOOT_H */
