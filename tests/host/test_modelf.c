/*
 * test_modelf.c - modelf_validate / modelf_check_info against synthetic
 * ET_REL images on the host under ASan/UBSan. A tiny builder assembles a
 * well-formed module image; each test mutates one thing and asserts on
 * the exact rule the validator reports.
 */

#include "harness.h"

#include "modelf_image.h"

#include <kernel/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int validate(const struct image *img, struct modelf_layout *l, const char **why)
{
    return modelf_validate(img->bytes, img->size, l, why);
}

/* Found by fuzz_modelf: the tables are read in place as structures, so an
 * unaligned section header table (or symbol table) is undefined behaviour
 * and must be rejected, not dereferenced. */
static void test_unaligned_tables(void)
{
    struct image img = build_image();
    struct modelf_layout l;
    const char *why = NULL;
    /* Move the section header table forward by 4 bytes inside a larger buffer. */
    size_t shsize = (size_t)img.eh->e_shnum * sizeof(struct elf64_shdr);
    uint8_t *bytes = calloc(1, img.size + 8);
    memcpy(bytes, img.bytes, img.size);
    memmove(bytes + img.eh->e_shoff + 4, bytes + img.eh->e_shoff, shsize);
    ((struct elf64_ehdr *)bytes)->e_shoff += 4;
    EXPECT(modelf_validate(bytes, img.size + 8, &l, &why) == -ENOEXEC);
    EXPECT(why && strstr(why, "aligned"));
    free(bytes);
    /* An unaligned symbol table offset, table in place. */
    img.sh[S_SYMTAB].sh_offset += 4;
    EXPECT(modelf_validate(img.bytes, img.size, &l, &why) == -ENOEXEC);
    EXPECT(why && strstr(why, "aligned"));
    free(img.bytes);
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
    img.eh->e_machine = 0x1234;
    EXPECT_REJECT(img, "not a native (x86-64) object");
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
    bad.abi_version = COSMO_MODULE_ABI_VERSION + 1;
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
    { "unaligned-tables", test_unaligned_tables },
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
