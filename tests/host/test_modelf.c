/*
 * test_modelf.c - modelf_validate / modelf_check_info against synthetic
 * ET_REL images on the host under ASan/UBSan. A tiny builder assembles a
 * well-formed module image; each test mutates one thing and asserts on
 * the exact rule the validator reports.
 */

#include "harness.h"

#include <kernel/elf64.h>
#include <kernel/errno.h>
#include <kernel/modelf.h>

#include <stdio.h>
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

static uint32_t name_off(const char *table, size_t table_len, const char *name)
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
static struct image build_image(void)
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

static int validate(const struct image *img, struct modelf_layout *l, const char **why)
{
    return modelf_validate(img->bytes, img->size, l, why);
}

static void test_good_image(void)
{
    struct image img = build_image();
    struct modelf_layout l;
    const char *why = NULL;
    EXPECT(validate(&img, &l, &why) == 0);
    EXPECT(l.symtab == S_SYMTAB && l.strtab == S_STRTAB && l.shstrtab == S_SHSTRTAB);
    EXPECT(l.info_section == S_INFO && l.ksymtab_section == S_KSYMTAB);
    EXPECT(l.nr_symbols == 5);
    EXPECT(l.nr_sections == 6);
    EXPECT(l.group_size[MODELF_TEXT] == 4096 && l.group_size[MODELF_RODATA] == 4096 &&
           l.group_size[MODELF_DATA] == 4096);
    const struct modelf_section *bss = modelf_find_section(&l, S_BSS);
    EXPECT(bss != NULL && bss->nobits && bss->group == MODELF_DATA && bss->offset == 16 && bss->size == 64);
    const struct modelf_section *info = modelf_find_section(&l, S_INFO);
    EXPECT(info != NULL && info->group == MODELF_RODATA && info->offset == 16);
    EXPECT(modelf_find_section(&l, S_SYMTAB) == NULL);
    EXPECT(strcmp(modelf_section_name(img.bytes, &l, S_TEXT), ".text") == 0);

    const struct cosmo_module_info *ci = (const struct cosmo_module_info *)(img.bytes + l.info_file_off);
    EXPECT(modelf_check_info(ci, &why) == 0);
    free(img.bytes);
}

#define EXPECT_REJECT(img, text)                                               \
    do {                                                                       \
        struct modelf_layout l_;                                               \
        const char *why_ = "";                                                 \
        int rc_ = validate(&(img), &l_, &why_);                                \
        EXPECT(rc_ == -ENOEXEC);                                               \
        if (strcmp(why_, (text)) != 0) {                                       \
            printf("    got '%s', want '%s'\n", why_, (text));                 \
            harness_fail(__FILE__, __LINE__, "reason");                        \
        }                                                                      \
    } while (0)

static void test_header_rules(void)
{
    struct image img;

    img = build_image();
    img.bytes[0] = 0;
    EXPECT_REJECT(img, "bad ELF magic");
    free(img.bytes);

    img = build_image();
    img.eh->e_type = ET_EXEC;
    EXPECT_REJECT(img, "not a relocatable object (ET_REL)");
    free(img.bytes);

    img = build_image();
    img.eh->e_machine = EM_AARCH64;
    EXPECT_REJECT(img, "not an x86-64 object");
    free(img.bytes);

    img = build_image();
    img.eh->e_ident[EI_CLASS] = 1;
    EXPECT_REJECT(img, "not ELF64");
    free(img.bytes);

    img = build_image();
    img.eh->e_shoff = img.size - 8;
    EXPECT_REJECT(img, "section header table outside the file");
    free(img.bytes);

    img = build_image();
    img.eh->e_shstrndx = S_COUNT;
    EXPECT_REJECT(img, "bad e_shstrndx");
    free(img.bytes);

    img = build_image();
    struct modelf_layout l;
    const char *why = "";
    EXPECT(modelf_validate(img.bytes, 10, &l, &why) == -ENOEXEC);
    EXPECT(strcmp(why, "file shorter than an ELF header") == 0);
    free(img.bytes);
}

static void test_section_rules(void)
{
    struct image img;

    img = build_image();
    img.sh[S_TEXT].sh_flags |= SHF_WRITE;
    EXPECT_REJECT(img, "section is writable and executable (W^X)");
    free(img.bytes);

    img = build_image();
    img.sh[S_RODATA].sh_offset = img.size - 4;
    EXPECT_REJECT(img, "section outside the file");
    free(img.bytes);

    img = build_image();
    img.sh[S_RODATA].sh_addralign = 8192;
    EXPECT_REJECT(img, "section alignment larger than a page");
    free(img.bytes);

    img = build_image();
    img.sh[S_RODATA].sh_addralign = 12;
    EXPECT_REJECT(img, "section alignment is not a power of two");
    free(img.bytes);

    img = build_image();
    img.sh[S_INFO].sh_size = 100;
    EXPECT_REJECT(img, ".cosmo.module has the wrong size");
    free(img.bytes);

    img = build_image();
    img.sh[S_INFO].sh_flags |= SHF_WRITE;
    EXPECT_REJECT(img, ".cosmo.module is writable");
    free(img.bytes);

    img = build_image();
    img.sh[S_INFO].sh_name = img.sh[S_RODATA].sh_name;   /* no .cosmo.module by name */
    EXPECT_REJECT(img, "no .cosmo.module section");
    free(img.bytes);

    img = build_image();
    img.sh[S_SYMTAB].sh_type = SHT_PROGBITS;
    EXPECT_REJECT(img, "no symbol table");
    free(img.bytes);

    img = build_image();
    img.sh[S_SYMTAB].sh_entsize = 16;
    EXPECT_REJECT(img, "bad symbol entry size");
    free(img.bytes);

    img = build_image();
    img.sh[S_RELA_TEXT].sh_link = S_STRTAB;
    EXPECT_REJECT(img, "relocations linked to a foreign symbol table");
    free(img.bytes);

    img = build_image();
    img.sh[S_RELA_TEXT].sh_info = S_BSS;
    EXPECT_REJECT(img, "relocations against a NOBITS section");
    free(img.bytes);

    img = build_image();
    img.sh[S_RELA_TEXT].sh_type = SHT_REL;
    EXPECT_REJECT(img, "REL relocations unsupported (need RELA)");
    free(img.bytes);

    img = build_image();
    img.sh[S_TEXT].sh_type = SHT_NOBITS;
    EXPECT_REJECT(img, "executable NOBITS section");
    free(img.bytes);

    img = build_image();
    img.sh[S_KSYMTAB].sh_flags |= SHF_EXECINSTR;
    EXPECT_REJECT(img, ".ksymtab in the text group");
    free(img.bytes);
}

static void test_symbol_rules(void)
{
    struct image img;
    struct elf64_sym *syms;

    img = build_image();
    syms = (struct elf64_sym *)(img.bytes + img.sh[S_SYMTAB].sh_offset);
    syms[3].st_shndx = SHN_COMMON;
    EXPECT_REJECT(img, "COMMON symbol (build with -fno-common)");
    free(img.bytes);

    img = build_image();
    syms = (struct elf64_sym *)(img.bytes + img.sh[S_SYMTAB].sh_offset);
    syms[3].st_shndx = SHN_XINDEX;
    EXPECT_REJECT(img, "extended section index unsupported");
    free(img.bytes);

    img = build_image();
    syms = (struct elf64_sym *)(img.bytes + img.sh[S_SYMTAB].sh_offset);
    syms[2].st_shndx = S_COUNT + 5;
    EXPECT_REJECT(img, "symbol section index out of range");
    free(img.bytes);

    img = build_image();
    syms = (struct elf64_sym *)(img.bytes + img.sh[S_SYMTAB].sh_offset);
    syms[1].st_name = 5000;
    EXPECT_REJECT(img, "symbol name outside the string table");
    free(img.bytes);

    img = build_image();
    syms = (struct elf64_sym *)(img.bytes + img.sh[S_SYMTAB].sh_offset);
    syms[3].st_info = (STB_LOCAL << 4);
    EXPECT_REJECT(img, "undefined local symbol");
    free(img.bytes);
}

static void test_info_rules(void)
{
    struct cosmo_module_info info;
    const char *why = "";
    memset(&info, 0, sizeof(info));
    memcpy(info.magic, COSMO_MODULE_MAGIC, 8);
    info.abi_version = COSMO_MODULE_ABI_VERSION;
    strcpy(info.name, "ok_name-1");
    strcpy(info.version, "1");
    EXPECT(modelf_check_info(&info, &why) == 0);

    struct cosmo_module_info bad = info;
    bad.abi_version = 2;
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "module ABI version mismatch") == 0);

    bad = info;
    bad.magic[7] = 0;
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "bad module metadata magic") == 0);

    bad = info;
    memset(bad.name, 'a', sizeof(bad.name));
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "module name not terminated") == 0);

    bad = info;
    bad.name[0] = 0;
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "empty module name") == 0);

    bad = info;
    strcpy(bad.name, "has space");
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC);

    bad = info;
    strcpy(bad.deps, "a,b,c,d,e,f,g,h,i");
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "too many dependencies") == 0);

    bad = info;
    strcpy(bad.deps, "a,");
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "trailing comma in deps") == 0);

    bad = info;
    strcpy(bad.deps, ",a");
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "empty dependency name") == 0);

    bad = info;
    bad.reserved[0] = 7;
    EXPECT(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "reserved metadata words are not zero") == 0);
}

static const struct host_test tests[] = {
    { "modelf-good", test_good_image },
    { "modelf-header", test_header_rules },
    { "modelf-sections", test_section_rules },
    { "modelf-symbols", test_symbol_rules },
    { "modelf-info", test_info_rules },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
