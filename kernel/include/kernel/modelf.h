/*
 * modelf.h - ET_REL module image validation and layout.
 *
 * modelf_validate() is a pure function over the file bytes (host tests
 * compile it with MODELF_HOST_TEST): it either fills a layout that the
 * loader can act on without re-checking bounds, or returns -ENOEXEC and
 * the failing rule in `why`. modelf_check_info() validates the metadata
 * block the same way.
 */

#ifndef KERNEL_MODELF_H
#define KERNEL_MODELF_H

#include <kernel/elf64.h>
#include <kernel/module.h>
#include <kernel/types.h>

#define MODELF_MAX_SECTIONS 32
#define MODELF_PAGE         4096ULL

enum modelf_group {
    MODELF_TEXT,     /* SHF_EXECINSTR */
    MODELF_RODATA,   /* allocatable, neither writable nor executable */
    MODELF_DATA,     /* SHF_WRITE, PROGBITS or NOBITS */
    MODELF_GROUPS,
};

struct modelf_section {
    uint32_t index;      /* ELF section index */
    uint8_t  group;      /* enum modelf_group */
    bool     nobits;
    uint64_t offset;     /* within the group, aligned */
    uint64_t size;
    uint64_t file_off;   /* PROGBITS: where the bytes are in the file */
    uint64_t align;
};

struct modelf_layout {
    uint64_t group_size[MODELF_GROUPS];   /* page rounded */
    unsigned nr_sections;
    struct modelf_section sections[MODELF_MAX_SECTIONS];
    uint32_t symtab;                      /* section indices */
    uint32_t strtab;
    uint32_t shstrtab;
    uint32_t info_section;                /* .cosmo.module */
    uint32_t ksymtab_section;             /* .ksymtab or 0 */
    uint64_t info_file_off;
    uint32_t nr_symbols;                  /* entries in .symtab, incl. index 0 */
};

int modelf_validate(const void *file, size_t size, struct modelf_layout *out, const char **why);

/* Metadata rules: magic, ABI version, name syntax, deps syntax and
 * count, reserved words zero. -ENOEXEC and `why` on failure. */
int modelf_check_info(const struct cosmo_module_info *info, const char **why);

/* Allocatable section by ELF index, or NULL. */
const struct modelf_section *modelf_find_section(const struct modelf_layout *l, uint32_t index);

/* Section header and name accessors for callers that already hold a
 * validated file. */
const struct elf64_shdr *modelf_shdr(const void *file, uint32_t index);
const char *modelf_section_name(const void *file, const struct modelf_layout *l, uint32_t index);

#endif /* KERNEL_MODELF_H */
