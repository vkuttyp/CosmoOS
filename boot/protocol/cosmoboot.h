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
 *      COSMOBOOT_MEM_ARCHIVE) holding init and the boot-time kernel modules
 *   4  + boot_pagetable_root_user, for architectures with split roots
 *   5  + el2_stub_phys (AArch64 booted at EL2)
 *   6  + the framebuffer the firmware configured (fb_*), which spends the
 *      reserved words and grows the structure; `size` says how much the
 *      loader wrote and the ELF note pairs kernel and loader exactly
 *   7  + the command line from \cosmo\cmdline (cmdline_phys/cmdline_size,
 *      spending reserved2) and the GPT partition the loader was read
 *      from (boot_partuuid); the structure grows
 *   8  + verified boot (verify_flags, verify_key_id): whether the loader
 *      checked kernel, archive and command line against the signed
 *      manifest, and with which key; the structure grows
 *   9  + the serial console the firmware describes (ACPI SPCR), found by
 *      the loader while it can still read any table: console_type,
 *      console_base, console_gsiv, console_access; the structure grows
 *   10 + what the firmware offers before ExitBootServices: a seed from
 *      EFI_RNG_PROTOCOL (fw_seed, fw_seed_len) and the time from GetTime
 *      (fw_epoch); the structure grows */
#define COSMOBOOT_VERSION 10

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

/* v7: the longest command line the loader passes, excluding the NUL. */
#define COSMOBOOT_CMDLINE_MAX 1023u

/* v7 boot_flags. */
#define COSMOBOOT_BOOT_PARTUUID 1u   /* boot_partuuid names a GPT partition */

/* v9 console_type: the serial port ACPI's SPCR names, as the loader
 * found it, or, when there is no SPCR, the DBG2's debug serial port
 * (which names no interrupt: console_gsiv 0). NONE when neither table
 * names a port, it is of another kind, or its base is zero: the kernel
 * then has no serial console. */
#define COSMOBOOT_CONSOLE_NONE        0u
#define COSMOBOOT_CONSOLE_PL011       1u   /* ARM PL011 or SBSA generic UART, MMIO */
#define COSMOBOOT_CONSOLE_16550_MMIO  2u   /* 16550-compatible, memory mapped */
#define COSMOBOOT_CONSOLE_16550_PORT  3u   /* 16550-compatible, x86 I/O port */
/* v9 console_flags. */
#define COSMOBOOT_CONSOLE_FROM_DBG2   1u   /* no SPCR: the port is the DBG2's debug serial port */

/* v10 fw_flags. */
#define COSMOBOOT_FW_TIME      1u   /* fw_epoch holds GetTime's answer */
#define COSMOBOOT_FW_SEED_MAX  64u

/* v8 verify_flags. */
#define COSMOBOOT_VERIFIED 1u        /* every file matched a manifest signed by verify_key_id */
#define COSMOBOOT_KEY_ID_SIZE 8u

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
     * 0 <= p < hhdm_size. The loader chooses hhdm_size (a GiB multiple):
     * the UEFI loader covers all RAM and everything it hands over --
     * kernel, archive, boot info, ACPI tables, framebuffer, the serial
     * console's registers (docs/boot/design.md, "The direct map"); its
     * releases up to v1.0.4 mapped a fixed 4 GiB. The kernel assumes no
     * size, so either loader boots either kernel and the version stays. */
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

    /* v6: the framebuffer as the loader leaves it (UEFI: the Graphics
     * Output Protocol's current mode, which the loader sets to the one
     * the display's EDID prefers when the firmware offers it, else leaves
     * as the firmware set it; boot/uefi/display.c). All zero
     * when the firmware offered no linear framebuffer, and the kernel
     * then has no display, which is not an error.
     *
     * The pixel format is three (shift, bits) pairs rather than an
     * enumeration, so a bit-mask format is described exactly like the two
     * common ones: a channel's value occupies `bits` bits starting at
     * `shift` within a little-endian pixel of fb_bpp bits. */
    uint64_t fb_phys;    /* physical base; the range is not RAM */
    uint64_t fb_size;    /* bytes the firmware says the framebuffer spans */
    uint32_t fb_width;   /* visible pixels per row */
    uint32_t fb_height;  /* rows */
    uint32_t fb_pitch;   /* bytes per row, >= fb_width * fb_bpp / 8 */
    uint32_t fb_bpp;     /* bits per pixel; 32 on every mode seen so far */
    uint8_t  fb_red_shift, fb_red_bits;
    uint8_t  fb_green_shift, fb_green_bits;
    uint8_t  fb_blue_shift, fb_blue_bits;
    uint8_t  fb_pad[2];

    /* v7: the command line, the text of \cosmo\cmdline on the boot
     * volume up to its first NUL, NUL terminated, in memory of type
     * COSMOBOOT_MEM_BOOTINFO. cmdline_size excludes the NUL and is at
     * most COSMOBOOT_CMDLINE_MAX. Both zero when there is no such file.
     * The kernel parses it (docs/boot/design.md, "The command line"). */
    uint64_t cmdline_phys;
    uint32_t cmdline_size;
    uint32_t boot_flags;      /* COSMOBOOT_BOOT_* */
    /* v7: the unique GUID of the GPT partition the loader itself was read
     * from (UEFI: the hard-drive node of its device path), in on-disk
     * byte order; valid when boot_flags has COSMOBOOT_BOOT_PARTUUID. A
     * volume without a partition table (a "superfloppy") has none. */
    uint8_t  boot_partuuid[16];

    /* v8: verified boot (docs/boot/design.md, "Verified boot"). A loader
     * that verifies refuses to boot unless \cosmo\manifest carries a valid
     * signature by a key compiled into it and kernel.elf, boot.tar and
     * cmdline all match it; it then sets COSMOBOOT_VERIFIED and the key's
     * id (the first 8 bytes of SHA-512 of the public key, as the module
     * ring names keys). A loader built with VERIFY_BOOT=0 checks nothing
     * and writes zeros: the kernel then reports an unverified boot. */
    uint32_t verify_flags;    /* COSMOBOOT_VERIFIED */
    uint32_t reserved3;
    uint8_t  verify_key_id[COSMOBOOT_KEY_ID_SIZE];

    /* v9: the serial console the firmware describes (ACPI SPCR; docs/boot/
     * design.md, "The serial console"). console_base is the register
     * block's physical address (an I/O port for 16550_PORT);
     * console_access the width of one register access in bytes, from the
     * address descriptor's access size (1, 2, 4 or 8; 0 when it says
     * "undefined"); console_stride the spacing of the registers in bytes,
     * from the descriptor's register bit width (0 when it says nothing):
     * the two differ on a 16550 read a byte at a time from 32-bit-spaced
     * registers. console_gsiv the interrupt, 0 when the port is polled.
     * The loader writes its own output after ExitBootServices to this port
     * and to nothing else. */
    uint32_t console_type;    /* COSMOBOOT_CONSOLE_* */
    uint32_t console_gsiv;
    uint64_t console_base;
    uint32_t console_access;
    uint32_t console_flags;   /* COSMOBOOT_CONSOLE_FROM_DBG2 */
    uint32_t console_stride;
    uint32_t reserved4;

    /* v10: what the firmware offers before ExitBootServices (docs/boot/
     * design.md, "Firmware seed and time"). fw_seed holds fw_seed_len
     * bytes (0: the firmware has no EFI_RNG_PROTOCOL, or it failed) from
     * the firmware's default RNG; the kernel absorbs them and zeroes the
     * field. fw_epoch is GetTime's answer in seconds since 1970 UTC
     * (local time corrected by its time zone when it names one), read
     * just before ExitBootServices, with COSMOBOOT_FW_TIME in fw_flags;
     * the kernel uses it only when it has no clock of its own. */
    uint32_t fw_flags;        /* COSMOBOOT_FW_* */
    uint32_t fw_seed_len;
    int64_t  fw_epoch;
    uint8_t  fw_seed[COSMOBOOT_FW_SEED_MAX];
};

#endif /* __ASSEMBLER__ */

#endif /* COSMOBOOT_H */
