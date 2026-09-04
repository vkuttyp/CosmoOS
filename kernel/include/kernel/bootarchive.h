/*
 * bootarchive.h - The boot archive: init and boot-time kernel modules.
 *
 * The loader delivers one ustar archive (boot protocol v3,
 * COSMOBOOT_MEM_ARCHIVE). The kernel validates every header once at
 * bootarchive_init() and afterwards answers name lookups with pointers
 * into the archive's reserved memory. The bytes are untrusted input to
 * whoever consumes them (elf_validate, modelf_validate); this layer only
 * guarantees that every (data, size) it hands out lies inside the archive.
 */

#ifndef KERNEL_BOOTARCHIVE_H
#define KERNEL_BOOTARCHIVE_H

#include <kernel/types.h>

#define BOOTARCHIVE_MAX_ENTRIES 64
#define BOOTARCHIVE_NAME_MAX    100  /* ustar name field, NUL included */

struct bootarchive_entry {
    char name[BOOTARCHIVE_NAME_MAX + 1];
    const void *data;   /* HHDM virtual */
    size_t size;
};

/* Parse the archive named by the boot info. No archive is not an error
 * (count stays 0). A malformed archive panics: the loader handed us
 * bytes we cannot make sense of, and running without init or modules
 * would only produce a less useful failure later. Call once after
 * bootinfo_init(). */
void bootarchive_init(void);

/* Exact-name lookup. Returns true and fills the data and size outputs on a hit. */
bool bootarchive_find(const char *name, const void **data, size_t *size);

unsigned bootarchive_count(void);
const struct bootarchive_entry *bootarchive_entry(unsigned index);

#endif /* KERNEL_BOOTARCHIVE_H */
