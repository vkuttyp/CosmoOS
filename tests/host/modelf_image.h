/*
 * modelf_image.h - A tiny builder for well-formed synthetic ET_REL module
 * images, shared by tests/host/test_modelf.c and tests/fuzz/fuzz_modelf.c.
 * Include once per translation unit.
 */

#ifndef COSMO_MODELF_IMAGE_H
#define COSMO_MODELF_IMAGE_H

#include <kernel/elf64.h>
#include <kernel/modelf.h>
#include <kernel/module.h>

#include <stdlib.h>
#include <string.h>

enum {
    S_NULL,
    S_TEXT,
    S_RODATA,
    S_DATA,
    S_BSS,
    S_INFO,
    S_KSYMTAB,
    S_SYMTAB,
    S_STRTAB,
    S_SHSTRTAB,
    S_RELA_TEXT,
    S_COUNT,
};

static const char shstr[] = "\0.text\0.rodata\0.data\0.bss\0.cosmo.module\0.ksymtab\0.symtab\0.strtab\0.shstrtab\0.rela.text";
static const char strtab[] = "\0mod_init\0mod_shutdown\0kprintf\0__cosmo_module_info";

static inline uint32_t name_off(const char *table, size_t table_len, const char *name)
{
    for (size_t i = 0; i < table_len; i++) {
        if (strcmp(table + i, name) == 0)
            return (uint32_t)i;
    }
    abort();
}

struct image {
    uint8_t *bytes;
    size_t size;
    struct elf64_ehdr *eh;
    struct elf64_shdr *sh;
};

/* Layout: ehdr, section bodies, then the section header table. */
static inline struct image build_image(void)
{
    static const uint8_t text[32] = { 0xc3 };
    static const uint8_t rodata[16] = { 1, 2, 3 };
    static const uint8_t data[8] = { 9 };
    struct cosmo_module_info info;
    memset(&info, 0, sizeof(info));
    memcpy(info.magic, COSMO_MODULE_MAGIC, 8);
    info.abi_version = COSMO_MODULE_ABI_VERSION;
    strcpy(info.name, "synthetic");
    strcpy(info.version, "0.1");
    strcpy(info.deps, "alpha,beta");
    struct ksym ks[1] = { { 0, 0 } };
    struct elf64_sym syms[5];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = name_off(strtab, sizeof(strtab), "mod_init");
    syms[1].st_info = (STB_LOCAL << 4);
    syms[1].st_shndx = S_TEXT;
    syms[2].st_name = name_off(strtab, sizeof(strtab), "mod_shutdown");
    syms[2].st_info = (STB_LOCAL << 4);
    syms[2].st_shndx = S_TEXT;
    syms[2].st_value = 8;
    syms[3].st_name = name_off(strtab, sizeof(strtab), "kprintf");
    syms[3].st_info = (STB_GLOBAL << 4);
    syms[3].st_shndx = SHN_UNDEF;
    syms[4].st_name = name_off(strtab, sizeof(strtab), "__cosmo_module_info");
    syms[4].st_info = (STB_GLOBAL << 4);
    syms[4].st_shndx = S_INFO;
    struct elf64_rela rela[1] = { { 4, ((uint64_t)3 << 32) | R_X86_64_PLT32, -4 } };

    struct {
        const void *body;
        size_t len;
        uint32_t type;
        uint64_t flags;
        uint32_t link, info;
        uint64_t align, entsize;
        const char *name;
    } secs[S_COUNT] = {
        [S_NULL] = { NULL, 0, SHT_NULL, 0, 0, 0, 0, 0, "" },
        [S_TEXT] = { text, sizeof(text), SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, 0, 16, 0, ".text" },
        [S_RODATA] = { rodata, sizeof(rodata), SHT_PROGBITS, SHF_ALLOC, 0, 0, 8, 0, ".rodata" },
        [S_DATA] = { data, sizeof(data), SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0, 0, 8, 0, ".data" },
        [S_BSS] = { NULL, 64, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0, 0, 16, 0, ".bss" },
        [S_INFO] = { &info, sizeof(info), SHT_PROGBITS, SHF_ALLOC, 0, 0, 8, 0, ".cosmo.module" },
        [S_KSYMTAB] = { ks, sizeof(ks), SHT_PROGBITS, SHF_ALLOC, 0, 0, 8, 0, ".ksymtab" },
        [S_SYMTAB] = { syms, sizeof(syms), SHT_SYMTAB, 0, S_STRTAB, 3, 8, sizeof(struct elf64_sym), ".symtab" },
        [S_STRTAB] = { strtab, sizeof(strtab), SHT_STRTAB, 0, 0, 0, 1, 0, ".strtab" },
        [S_SHSTRTAB] = { shstr, sizeof(shstr), SHT_STRTAB, 0, 0, 0, 1, 0, ".shstrtab" },
        [S_RELA_TEXT] = { rela, sizeof(rela), SHT_RELA, 0, S_SYMTAB, S_TEXT, 8, sizeof(struct elf64_rela), ".rela.text" },
    };

    size_t off = sizeof(struct elf64_ehdr);
    size_t offsets[S_COUNT];
    for (int i = 1; i < S_COUNT; i++) {
        off = (off + 15) & ~(size_t)15;
        offsets[i] = off;
        if (secs[i].type != SHT_NOBITS)
            off += secs[i].len;
    }
    off = (off + 15) & ~(size_t)15;
    size_t shoff = off;
    size_t total = shoff + S_COUNT * sizeof(struct elf64_shdr);

    struct image img;
    img.bytes = calloc(1, total);
    img.size = total;
    img.eh = (struct elf64_ehdr *)img.bytes;
    memcpy(img.eh->e_ident, "\x7f" "ELF", 4);
    img.eh->e_ident[EI_CLASS] = ELFCLASS64;
    img.eh->e_ident[EI_DATA] = ELFDATA2LSB;
    img.eh->e_ident[EI_VERSION] = EV_CURRENT;
    img.eh->e_type = ET_REL;
    img.eh->e_machine = EM_X86_64;
    img.eh->e_version = EV_CURRENT;
    img.eh->e_shoff = shoff;
    img.eh->e_ehsize = sizeof(struct elf64_ehdr);
    img.eh->e_shentsize = sizeof(struct elf64_shdr);
    img.eh->e_shnum = S_COUNT;
    img.eh->e_shstrndx = S_SHSTRTAB;
    img.sh = (struct elf64_shdr *)(img.bytes + shoff);
    for (int i = 1; i < S_COUNT; i++) {
        img.sh[i].sh_name = name_off(shstr, sizeof(shstr), secs[i].name);
        img.sh[i].sh_type = secs[i].type;
        img.sh[i].sh_flags = secs[i].flags;
        img.sh[i].sh_offset = offsets[i];
        img.sh[i].sh_size = secs[i].len;
        img.sh[i].sh_link = secs[i].link;
        img.sh[i].sh_info = secs[i].info;
        img.sh[i].sh_addralign = secs[i].align;
        img.sh[i].sh_entsize = secs[i].entsize;
        if (secs[i].type != SHT_NOBITS && secs[i].body)
            memcpy(img.bytes + offsets[i], secs[i].body, secs[i].len);
    }
    return img;
}


#endif /* COSMO_MODELF_IMAGE_H */
