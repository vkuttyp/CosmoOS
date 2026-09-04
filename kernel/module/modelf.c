/*
 * modelf.c - ET_REL module image validation and layout.
 *
 * Pure over the file bytes: every offset, size and index is checked
 * against the buffer before it is dereferenced, so the loader that
 * follows can act on the layout without repeating the checks. Compiled
 * on the host for tests/host/test_modelf.c under MODELF_HOST_TEST.
 */

#include <kernel/errno.h>
#include <kernel/modelf.h>
#include <kernel/string.h>

#define REJECT(cond, text)                                                     \
    do {                                                                       \
        if (cond) {                                                            \
            if (why)                                                           \
                *why = (text);                                                 \
            return -ENOEXEC;                                                   \
        }                                                                      \
    } while (0)

static bool in_file(uint64_t off, uint64_t len, size_t size)
{
    return off <= size && len <= size - off;
}

const struct elf64_shdr *modelf_shdr(const void *file, uint32_t index)
{
    const struct elf64_ehdr *eh = file;
    return (const struct elf64_shdr *)((const uint8_t *)file + eh->e_shoff) + index;
}

const char *modelf_section_name(const void *file, const struct modelf_layout *l, uint32_t index)
{
    const struct elf64_shdr *names = modelf_shdr(file, l->shstrtab);
    const struct elf64_shdr *sh = modelf_shdr(file, index);
    if (sh->sh_name >= names->sh_size)
        return "";
    return (const char *)file + names->sh_offset + sh->sh_name;
}

const struct modelf_section *modelf_find_section(const struct modelf_layout *l, uint32_t index)
{
    for (unsigned i = 0; i < l->nr_sections; i++) {
        if (l->sections[i].index == index)
            return &l->sections[i];
    }
    return NULL;
}

/* A NUL-terminated string table where every name lookup is bounded. */
static bool strtab_ok(const struct elf64_shdr *sh, size_t size)
{
    if (sh->sh_type != SHT_STRTAB || sh->sh_size == 0 || !in_file(sh->sh_offset, sh->sh_size, size))
        return false;
    return true;
}

static const char *strtab_get(const void *file, const struct elf64_shdr *strtab, uint64_t off)
{
    if (off >= strtab->sh_size)
        return NULL;
    const char *base = (const char *)file + strtab->sh_offset;
    /* The last byte of a valid string table is NUL. */
    if (base[strtab->sh_size - 1] != '\0')
        return NULL;
    return base + off;
}

int modelf_validate(const void *file, size_t size, struct modelf_layout *out, const char **why)
{
    const uint8_t *bytes = file;
    if (why)
        *why = "";
    memset(out, 0, sizeof(*out));

    REJECT(size < sizeof(struct elf64_ehdr), "file shorter than an ELF header");
    const struct elf64_ehdr *eh = file;
    REJECT(memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0, "bad ELF magic");
    REJECT(eh->e_ident[EI_CLASS] != ELFCLASS64, "not ELF64");
    REJECT(eh->e_ident[EI_DATA] != ELFDATA2LSB, "not little endian");
    REJECT(eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT, "bad ELF version");
    REJECT(eh->e_type != ET_REL, "not a relocatable object (ET_REL)");
    REJECT(eh->e_machine != EM_X86_64, "not an x86-64 object");
    REJECT(eh->e_ehsize != sizeof(struct elf64_ehdr), "bad e_ehsize");
    REJECT(eh->e_shentsize != sizeof(struct elf64_shdr), "bad e_shentsize");
    REJECT(eh->e_shnum < 2, "no sections");
    REJECT(eh->e_shnum >= SHN_LORESERVE, "too many sections");
    REJECT(!in_file(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(struct elf64_shdr), size),
           "section header table outside the file");
    REJECT(eh->e_shstrndx == 0 || eh->e_shstrndx >= eh->e_shnum, "bad e_shstrndx");

    const struct elf64_shdr *shstr = modelf_shdr(file, eh->e_shstrndx);
    REJECT(!strtab_ok(shstr, size), "bad section name table");
    out->shstrtab = eh->e_shstrndx;

    /* Pass 1: every header sane; find the symbol table and metadata. */
    for (uint32_t i = 1; i < eh->e_shnum; i++) {
        const struct elf64_shdr *sh = modelf_shdr(file, i);
        const char *name = strtab_get(file, shstr, sh->sh_name);
        REJECT(name == NULL, "section name outside the name table");
        REJECT(sh->sh_type == SHT_REL, "REL relocations unsupported (need RELA)");
        REJECT(sh->sh_type == SHT_DYNSYM, "dynamic symbol table in a module");
        if (sh->sh_type != SHT_NOBITS && sh->sh_type != SHT_NULL)
            REJECT(!in_file(sh->sh_offset, sh->sh_size, size), "section outside the file");
        if (sh->sh_type == SHT_SYMTAB) {
            REJECT(out->symtab != 0, "more than one symbol table");
            REJECT(sh->sh_entsize != sizeof(struct elf64_sym), "bad symbol entry size");
            REJECT(sh->sh_size % sizeof(struct elf64_sym) != 0, "symbol table size not a multiple");
            REJECT(sh->sh_link == 0 || sh->sh_link >= eh->e_shnum, "symbol table string link out of range");
            REJECT(!strtab_ok(modelf_shdr(file, sh->sh_link), size), "bad symbol string table");
            REJECT(sh->sh_size / sizeof(struct elf64_sym) > UINT32_MAX, "too many symbols");
            out->symtab = i;
            out->strtab = sh->sh_link;
            out->nr_symbols = (uint32_t)(sh->sh_size / sizeof(struct elf64_sym));
        }
        if (strcmp(name, ".cosmo.module") == 0) {
            REJECT(out->info_section != 0, "more than one .cosmo.module section");
            REJECT(sh->sh_type != SHT_PROGBITS, ".cosmo.module is not PROGBITS");
            REJECT((sh->sh_flags & SHF_ALLOC) == 0, ".cosmo.module is not allocatable");
            REJECT((sh->sh_flags & SHF_WRITE) != 0, ".cosmo.module is writable");
            REJECT(sh->sh_size != sizeof(struct cosmo_module_info), ".cosmo.module has the wrong size");
            out->info_section = i;
            out->info_file_off = sh->sh_offset;
        }
        if (strcmp(name, ".ksymtab") == 0) {
            REJECT((sh->sh_flags & SHF_ALLOC) == 0, ".ksymtab is not allocatable");
            REJECT(sh->sh_type != SHT_PROGBITS, ".ksymtab is not PROGBITS");
            REJECT(sh->sh_size % sizeof(struct ksym) != 0, ".ksymtab size is not a multiple of an entry");
            out->ksymtab_section = i;
        }
    }
    REJECT(out->symtab == 0, "no symbol table");
    REJECT(out->nr_symbols == 0, "empty symbol table");
    REJECT(out->info_section == 0, "no .cosmo.module section");

    /* Pass 2: allocatable sections into groups; relocation sections checked. */
    for (uint32_t i = 1; i < eh->e_shnum; i++) {
        const struct elf64_shdr *sh = modelf_shdr(file, i);

        if (sh->sh_type == SHT_RELA) {
            REJECT(sh->sh_entsize != sizeof(struct elf64_rela), "bad relocation entry size");
            REJECT(sh->sh_size % sizeof(struct elf64_rela) != 0, "relocation section size not a multiple");
            REJECT(sh->sh_link != out->symtab, "relocations linked to a foreign symbol table");
            REJECT(sh->sh_info == 0 || sh->sh_info >= eh->e_shnum, "relocation target out of range");
            const struct elf64_shdr *target = modelf_shdr(file, sh->sh_info);
            if ((target->sh_flags & SHF_ALLOC) == 0)
                continue;   /* relocations for debug info: ignored */
            REJECT(target->sh_type == SHT_NOBITS, "relocations against a NOBITS section");
            continue;
        }
        if ((sh->sh_flags & SHF_ALLOC) == 0)
            continue;
        REJECT(sh->sh_type != SHT_PROGBITS && sh->sh_type != SHT_NOBITS,
               "allocatable section of an unexpected type");
        bool w = (sh->sh_flags & SHF_WRITE) != 0;
        bool x = (sh->sh_flags & SHF_EXECINSTR) != 0;
        REJECT(w && x, "section is writable and executable (W^X)");
        REJECT(x && sh->sh_type == SHT_NOBITS, "executable NOBITS section");
        uint64_t align = sh->sh_addralign == 0 ? 1 : sh->sh_addralign;
        REJECT((align & (align - 1)) != 0, "section alignment is not a power of two");
        REJECT(align > MODELF_PAGE, "section alignment larger than a page");
        REJECT(out->nr_sections == MODELF_MAX_SECTIONS, "too many allocatable sections");
        REJECT(sh->sh_size > (1ULL << 30), "section larger than 1 GiB");

        struct modelf_section *ms = &out->sections[out->nr_sections++];
        ms->index = i;
        ms->group = x ? MODELF_TEXT : w ? MODELF_DATA : MODELF_RODATA;
        ms->nobits = sh->sh_type == SHT_NOBITS;
        ms->size = sh->sh_size;
        ms->file_off = sh->sh_offset;
        ms->align = align;

        uint64_t off = (out->group_size[ms->group] + align - 1) & ~(align - 1);
        ms->offset = off;
        out->group_size[ms->group] = off + sh->sh_size;
        REJECT(out->group_size[ms->group] > (1ULL << 31), "module larger than 2 GiB");
    }
    for (unsigned g = 0; g < MODELF_GROUPS; g++)
        out->group_size[g] = (out->group_size[g] + MODELF_PAGE - 1) & ~(MODELF_PAGE - 1);

    /* The metadata must land in a group we can protect read-only. */
    const struct modelf_section *info = modelf_find_section(out, out->info_section);
    REJECT(info == NULL || info->group != MODELF_RODATA, ".cosmo.module is not read-only data");
    if (out->ksymtab_section) {
        const struct modelf_section *ks = modelf_find_section(out, out->ksymtab_section);
        REJECT(ks == NULL || ks->group == MODELF_TEXT, ".ksymtab in the text group");
    }

    /* Symbols: section indices in range, no COMMON, no XINDEX. */
    const struct elf64_shdr *symtab = modelf_shdr(file, out->symtab);
    const struct elf64_shdr *strtab = modelf_shdr(file, out->strtab);
    const struct elf64_sym *syms = (const struct elf64_sym *)(bytes + symtab->sh_offset);
    REJECT(syms[0].st_shndx != SHN_UNDEF || syms[0].st_name != 0, "symbol 0 is not the null symbol");
    for (uint32_t i = 1; i < out->nr_symbols; i++) {
        REJECT(strtab_get(file, strtab, syms[i].st_name) == NULL, "symbol name outside the string table");
        uint16_t shn = syms[i].st_shndx;
        REJECT(shn == SHN_COMMON, "COMMON symbol (build with -fno-common)");
        REJECT(shn == SHN_XINDEX, "extended section index unsupported");
        REJECT(shn != SHN_UNDEF && shn != SHN_ABS && shn >= eh->e_shnum, "symbol section index out of range");
        if (shn == SHN_UNDEF)
            REJECT(ELF64_ST_BIND(syms[i].st_info) == STB_LOCAL, "undefined local symbol");
    }

    return 0;
}

static bool name_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int modelf_check_info(const struct cosmo_module_info *info, const char **why)
{
    REJECT(memcmp(info->magic, COSMO_MODULE_MAGIC, sizeof(info->magic)) != 0, "bad module metadata magic");
    REJECT(info->abi_version != COSMO_MODULE_ABI_VERSION, "module ABI version mismatch");
    REJECT(memchr(info->name, '\0', sizeof(info->name)) == NULL, "module name not terminated");
    REJECT(info->name[0] == '\0', "empty module name");
    for (const char *p = info->name; *p; p++)
        REJECT(!name_char_ok(*p), "module name has characters outside [A-Za-z0-9_-]");
    REJECT(memchr(info->version, '\0', sizeof(info->version)) == NULL, "module version not terminated");
    REJECT(memchr(info->deps, '\0', sizeof(info->deps)) == NULL, "module deps not terminated");
    unsigned deps = 0;
    unsigned run = 0;
    for (const char *p = info->deps; *p; p++) {
        if (*p == ',') {
            REJECT(run == 0, "empty dependency name");
            deps++;
            run = 0;
            continue;
        }
        REJECT(!name_char_ok(*p), "dependency name has characters outside [A-Za-z0-9_-]");
        run++;
        REJECT(run >= MODULE_NAME_MAX, "dependency name too long");
    }
    if (run > 0)
        deps++;
    else
        REJECT(deps > 0, "trailing comma in deps");
    REJECT(deps > MODULE_MAX_DEPS, "too many dependencies");
    for (unsigned i = 0; i < 4; i++)
        REJECT(info->reserved[i] != 0, "reserved metadata words are not zero");
    return 0;
}
