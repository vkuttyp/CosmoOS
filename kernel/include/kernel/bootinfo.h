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

#endif /* KERNEL_BOOTINFO_H */
