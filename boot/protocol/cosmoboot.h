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
 *
 * AArch64 specifics: EL1, MMU on, TTBR1 = boot_pagetable_root (the
 * higher half: image and direct map), TTBR0 = boot_pagetable_root_user
 * (the loader's identity map), MAIR/TCR programmed as the kernel expects
 * (4 KiB granule, 48-bit), interrupts masked, x0 = info, sp = the loader
 * stack (identity mapped). The direct map gives RAM normal write-back
 * attributes and every other range device attributes.
 */

#ifndef COSMOBOOT_H
#define COSMOBOOT_H

/* Constants usable from assembly (the kernel's entry file emits the
 * protocol note). Everything below the __ASSEMBLER__ guard is C only. */

/* Version history:
 *   1  memory map, HHDM, kernel placement, page-table root, ACPI RSDP
 *   2  + one boot module (module_phys/module_size) and COSMOBOOT_MEM_MODULE
 *   3  the module becomes a ustar boot archive (archive_phys/archive_size,
 *      COSMOBOOT_MEM_ARCHIVE) holding init and the boot-time kernel modules */
#define COSMOBOOT_VERSION 5

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
#define COSMOBOOT_MEM_ARCHIVE           13u  /* boot archive (init, modules), v3 */
#define COSMOBOOT_MEM_EL2_STUB          14u  /* AArch64: the resident EL2 stub, v5 */

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

    /* Version 3: the boot archive, a ustar archive holding the initial
     * user executable ("init") and boot-time kernel modules
     * ("modules/<name>.ko"), in memory of type COSMOBOOT_MEM_ARCHIVE.
     * Both zero when no archive was found. (Version 2 carried a single
     * raw ELF here as module_phys/module_size.) */
    uint64_t archive_phys;
    uint64_t archive_size;

    /* Reserved for framebuffer and command line in later versions.
     * Must be zero in version 3. */
    /* v4: a second bootstrap root for architectures with split roots
     * (AArch64: the TTBR0 identity table the loader still runs on; the
     * kernel adopts boot_pagetable_root as TTBR1). x86-64 writes 0. */
    uint64_t boot_pagetable_root_user;
    /* v5: AArch64 only. When firmware handed the loader control at EL2,
     * the loader left a stub owning EL2 (a vector table plus a handler,
     * VBAR_EL2 pointing at it, the EL2 MMU off) and dropped to EL1; this
     * is that page's physical address, in memory of type
     * COSMOBOOT_MEM_EL2_STUB. Zero when the machine has no EL2 or the
     * loader could not reserve the page: EL1 then has no way up. */
    uint64_t el2_stub_phys;
    uint64_t reserved1[4];
};

#endif /* __ASSEMBLER__ */

#endif /* COSMOBOOT_H */
