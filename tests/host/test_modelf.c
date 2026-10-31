/*
 * test_modelf.c - modelf_validate / modelf_check_info against synthetic
 * ET_REL images on the host under ASan/UBSan. A tiny builder assembles a
 * well-formed module image; each test mutates one thing and asserts on
 * the exact rule the validator reports.
 *
 * And a recorded digest (roadmap 2.0 SA5's differential test for the
 * module-ELF family): 50,000 images from a seeded generator -- the
 * builder's, with header, section-header and symbol fields set to edge
 * values, bytes flipped, the size cut -- through modelf_validate_for under
 * both machines, the accessors on what it accepts, and modelf_check_info_for
 * on metadata records with names, versions and dependency lists good and
 * bad. Every result is a line; the lines' FNV-1a digest is recorded below.
 * The C gave it (2026-10-31), and the CeeSafe that replaced it
 * (modelf.cee) gives the same. MODELF_DUMP=1
 * prints the lines; MODELF_BENCH=1 times the validator.
 */

#include "harness.h"

#include "modelf_image.h"

#include <kernel/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* --- the recorded digest --------------------------------------------------- */

#define ROUNDS 50000
#define SEED 0x40DE1F5A52026E11ull
/* The digest of the C validator (2026-10-31), which modelf.cee gives too. */
#define MODELF_DIGEST 0xeaa7bf3ed05aa6ffull

static uint64_t g_rng;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static const uint64_t EDGES[] = { 0, 1, 2, 3, 4, 7, 8, 9, 16, 24, 64, 0x1000, 0x1001, 0xff00, 0xffff, 0x40000000,
                                  0x80000001, 0xffffffff, 0xffffffffffffffffull, 0x8000000000000000ull };

static uint64_t value(void)
{
    return next() % 2 ? EDGES[next() % (sizeof(EDGES) / sizeof(EDGES[0]))] : next() % 4096;
}

/* The builder's image, changed: an ELF header field, a section header's field, a symbol's, a flipped byte, a cut. */
static struct image make_image(uint16_t *machine)
{
    struct image img = build_image();
    *machine = next() % 4 ? EM_X86_64 : EM_AARCH64;
    unsigned edits = (unsigned)(next() % 4);
    for (unsigned k = 0; k < edits && img.size > 0; k++) {
        uint64_t v = value();
        switch (next() % 6) {
        case 0: {
            switch (next() % 7) {
            case 0: img.eh->e_type = (uint16_t)v; break;
            case 1: img.eh->e_shoff = v; break;
            case 2: img.eh->e_shnum = (uint16_t)v; break;
            case 3: img.eh->e_shstrndx = (uint16_t)v; break;
            case 4: img.eh->e_shentsize = (uint16_t)v; break;
            case 5: img.eh->e_ehsize = (uint16_t)v; break;
            default: img.eh->e_version = (uint32_t)v; break;
            }
            break;
        }
        case 1:
        case 2: {
            uint16_t shnum = 11;   /* S_COUNT */
            struct elf64_shdr *sh = &img.sh[next() % shnum];
            switch (next() % 9) {
            case 0: sh->sh_name = (uint32_t)v; break;
            case 1: sh->sh_type = (uint32_t)v; break;
            case 2: sh->sh_flags = v; break;
            case 3: sh->sh_offset = v; break;
            case 4: sh->sh_size = v; break;
            case 5: sh->sh_link = (uint32_t)v; break;
            case 6: sh->sh_info = (uint32_t)v; break;
            case 7: sh->sh_addralign = v; break;
            default: sh->sh_entsize = v; break;
            }
            break;
        }
        case 3:
        case 4:
            img.bytes[next() % img.size] ^= (uint8_t)(1u << (next() % 8));
            break;
        default:
            img.size = (size_t)(next() % (img.size + 1));
            break;
        }
    }
    return img;
}

static void emit(uint64_t *digest, const char *line, int n, bool dump)
{
    for (int i = 0; i < n; i++) {
        *digest ^= (uint8_t)line[i];
        *digest *= 0x100000001b3ull;
    }
    *digest ^= '\n';
    *digest *= 0x100000001b3ull;
    if (dump)
        printf("%s\n", line);
}

static int validate_for(const struct image *img, uint16_t machine, struct modelf_layout *l, const char **why)
{
    memset(l, 0, sizeof(*l));
    return modelf_validate_for(img->bytes, img->size, machine, "not this machine", l, why);
}

static void run_image(uint64_t *digest, bool dump, unsigned *accepted)
{
    uint16_t machine;
    struct image img = make_image(&machine);
    struct modelf_layout l;
    const char *why = NULL;
    int rc = validate_for(&img, machine, &l, &why);
    char line[4096];
    int n = snprintf(line, sizeof(line), "v%d %s g%llx:%llx:%llx n%u s%u t%u h%u i%u k%u o%llx y%u", rc, why ? why : "-",
                     (unsigned long long)l.group_size[0], (unsigned long long)l.group_size[1],
                     (unsigned long long)l.group_size[2], l.nr_sections, l.symtab, l.strtab, l.shstrtab, l.info_section,
                     l.ksymtab_section, (unsigned long long)l.info_file_off, l.nr_symbols);
    for (unsigned i = 0; i < l.nr_sections && i < MODELF_MAX_SECTIONS; i++) {
        const struct modelf_section *s = &l.sections[i];
        n += snprintf(line + n, sizeof(line) - (size_t)n, " [%u %u %d %llx %llx %llx %llx]", s->index, s->group,
                      s->nobits, (unsigned long long)s->offset, (unsigned long long)s->size,
                      (unsigned long long)s->file_off, (unsigned long long)s->align);
    }
    if (rc == 0) {
        *accepted += 1;
        uint16_t shnum = ((const struct elf64_ehdr *)img.bytes)->e_shnum;
        for (uint32_t i = 0; i < shnum; i++) {
            const struct modelf_section *f = modelf_find_section(&l, i);
            n += snprintf(line + n, sizeof(line) - (size_t)n, " (%s %td %td)", modelf_section_name(img.bytes, &l, i),
                          f ? f - l.sections : -1, (const uint8_t *)modelf_shdr(img.bytes, i) - img.bytes);
        }
    }
    EXPECT(n > 0 && (size_t)n < sizeof(line));
    emit(digest, line, n, dump);
    free(img.bytes);
}

/* A metadata record: magic, ABI version, name, version, dependencies, reserved words, each good or bad. */
static void run_info(uint64_t *digest, bool dump, unsigned *accepted)
{
    struct cosmo_module_info info;
    memset(&info, 0, sizeof(info));
    memcpy(info.magic, next() % 8 ? COSMO_MODULE_MAGIC : "COSMOMOX", 8);
    info.abi_version = next() % 8 ? COSMO_MODULE_ABI_VERSION : (uint32_t)next();
    static const char alphabet[] = "abcXYZ019_-,. /";
    size_t nl = next() % 8 ? (size_t)(next() % MODULE_NAME_MAX) : MODULE_NAME_MAX;
    for (size_t i = 0; i < nl; i++)
        info.name[i] = next() % 16 ? alphabet[next() % 10] : alphabet[next() % (sizeof(alphabet) - 1)];
    size_t vl = (size_t)(next() % (MODULE_VERSION_MAX + 1));
    for (size_t i = 0; i < vl; i++)
        info.version[i] = (char)('0' + next() % 10);
    size_t dl = next() % 3 ? (size_t)(next() % 64) : (size_t)(next() % (MODULE_DEPS_MAX + 1));
    for (size_t i = 0; i < dl; i++)
        info.deps[i] = next() % 6 ? alphabet[next() % 10] : alphabet[10 + next() % 5];
    if (next() % 8 == 0)
        info.reserved[next() % 4] = next();
    const char *why = NULL;
    int rc = modelf_check_info_for((const uint8_t *)info.magic, info.abi_version, COSMO_MODULE_ABI_VERSION, (const uint8_t *)info.name,
                                   (const uint8_t *)info.version, (const uint8_t *)info.deps, info.reserved, &why);
    *accepted += rc == 0;
    char line[256];
    int n = snprintf(line, sizeof(line), "c%d %s", rc, rc == 0 ? "-" : why);
    emit(digest, line, n, dump);
}

static void test_recorded_digest(void)
{
    bool dump = getenv("MODELF_DUMP") != NULL;
    uint64_t digest = 0xcbf29ce484222325ull;
    unsigned images = 0, infos = 0;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++) {
        run_image(&digest, dump, &images);
        run_info(&digest, dump, &infos);
    }
    printf("  modelf digest 0x%016llx over %u images and records (accepted %u images, %u records)\n",
           (unsigned long long)digest, ROUNDS, images, infos);
    EXPECT(images > 1000 && infos > 1000);
    EXPECT(digest == MODELF_DIGEST);
}

static volatile uint64_t g_sink;

static int bench(void)
{
    enum { N = 20000 };
    struct image *imgs = malloc(sizeof(*imgs) * N);
    uint16_t *machines = malloc(sizeof(*machines) * N);
    if (imgs == NULL || machines == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned r = 0; r < N; r++)
        imgs[r] = make_image(&machines[r]);
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < N; r++) {
            struct modelf_layout l;
            const char *why;
            acc += (uint64_t)validate_for(&imgs[r], machines[r], &l, &why) + l.nr_sections;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("modelf bench: %.2f ns per image (best of 15 runs of %u)\n", best / N, N);
    for (unsigned r = 0; r < N; r++)
        free(imgs[r].bytes);
    free(imgs);
    free(machines);
    return 0;
}

static const struct host_test tests[] = {
    { "unaligned-tables", test_unaligned_tables },
    { "modelf-good", test_good_image },
    { "modelf-header", test_header_rules },
    { "modelf-sections", test_section_rules },
    { "modelf-symbols", test_symbol_rules },
    { "modelf-info", test_info_rules },
    { "recorded digest", test_recorded_digest },
};

int main(void)
{
    if (getenv("MODELF_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
