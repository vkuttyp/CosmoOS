/*
 * bootinfo.h - Access to the boot protocol data handed over by the loader.
 *
 * bootinfo_init() validates the structure once at boot (magic, version,
 * size, memory-map bounds) and panics on any inconsistency: a kernel that
 * cannot trust its memory map has nothing safe to do. After that every
 * accessor is read-only and lock-free. The data itself lives in
 * loader-provided memory of type COSMOBOOT_MEM_BOOTINFO, which the future
 * physical memory manager must keep reserved for as long as any of these
 * accessors is in use.
 */

#ifndef KERNEL_BOOTINFO_H
#define KERNEL_BOOTINFO_H

#include <stdbool.h>
#include <stdint.h>

#include <cosmoboot.h>

void bootinfo_init(const struct cosmoboot_info *info);

const struct cosmoboot_info *bootinfo_get(void);

/* Memory map array and entry count. */
const struct cosmoboot_mem_entry *bootinfo_mem_map(uint32_t *count);

/* Sum of COSMOBOOT_MEM_USABLE bytes. */
uint64_t bootinfo_usable_bytes(void);

/* Highest physical address + 1 of any RAM entry (usable, reclaimable,
 * kernel, firmware runtime). MMIO and reserved ranges are excluded. */
uint64_t bootinfo_phys_limit(void);

/* Translate a physical address through the higher-half direct map.
 * Panics if the address is outside the mapped range. */
void *bootinfo_phys_to_virt(uint64_t phys);

const char *bootinfo_mem_type_name(uint32_t type);

/* True for types that are RAM (usable, reclaimable, kernel, boot data,
 * ACPI, firmware runtime), false for MMIO, reserved, bad, persistent. */
bool bootinfo_mem_type_is_ram(uint32_t type);

/* True if `pa` lies inside a memory-map entry of a RAM type, i.e. the
 * direct map covers it after vmm_init. */
bool bootinfo_phys_is_ram(uint64_t pa);

/* The framebuffer the firmware had already configured, as the loader
 * found it (boot protocol v6). Fields are exactly the loader's. */
struct bootinfo_framebuffer {
    uint64_t phys;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;    /* bytes per row */
    uint32_t bpp;      /* bits per pixel */
    uint8_t  red_shift, red_bits;
    uint8_t  green_shift, green_bits;
    uint8_t  blue_shift, blue_bits;
};

/*
 * Decide whether a framebuffer description is one the kernel may write
 * to, and copy it out when it is. This is the whole trust boundary for
 * the display: everything a fbcon does is bounded by these checks, so it
 * is a pure function of the boot fields, tested on the host and fuzzed
 * (kernel/core/fbvalid.c). `why` receives a static reason when the
 * answer is false; either pointer may be NULL.
 *
 * False for a framebuffer that is absent (all zero), one whose rows do
 * not fit the memory it claims, one whose pixels are not 8, 16, 24 or 32
 * bits, and one whose colour fields run off the end of a pixel.
 */
bool bootinfo_fb_validate(const struct cosmoboot_info *info, struct bootinfo_framebuffer *out,
                          const char **why);

/* The validated framebuffer, or NULL when the machine has none. Set by
 * bootinfo_init. */
const struct bootinfo_framebuffer *bootinfo_framebuffer(void);

#endif /* KERNEL_BOOTINFO_H */
