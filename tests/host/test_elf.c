/*
 * test_elf.c - Host test of the user executable validator
 * (kernel/process/elfparse; kernel/include/kernel/elf.h): direct checks,
 * and a recorded digest (roadmap 2.0 SA5's differential test for the ELF
 * family). 100,000 images from a seeded generator -- a valid executable
 * (two PT_LOAD segments, a CosmoOS note, an interpreter, PT_GNU_STACK)
 * with fields and bytes changed -- go through elf_validate_for under
 * both architectures' machines, then elf_rebase; every result (the info,
 * every segment, the interpreter, the reason) is a line, and the lines'
 * FNV-1a digest is recorded below. The C gave it; the CeeSafe must give
 * the same. ELF_DUMP=1 prints the lines; ELF_BENCH=1 times the validator.
 * ASan and UBSan.
 */

#include "harness.h"

#include <kernel/elf.h>
#include <kernel/elf64.h>
#include <kernel/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USER_LO 0x0000000000400000ull
#define USER_HI 0x00007fffffffffffull
#define IMG 1024

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* A program header at index i of a table at 64. */
static void phdr(uint8_t *img, unsigned i, uint32_t type, uint32_t flags, uint64_t off, uint64_t vaddr, uint64_t filesz,
                 uint64_t memsz, uint64_t align)
{
    uint8_t *p = img + 64 + 56 * i;
    put32(p, type);
    put32(p + 4, flags);
    put64(p + 8, off);
    put64(p + 16, vaddr);
    put64(p + 24, vaddr);
    put64(p + 32, filesz);
    put64(p + 40, memsz);
    put64(p + 48, align);
}

/* A valid executable for `machine`: text at 0x401000 (offset 0x?? congruent), data at 0x403000, a note, an
 * interpreter, a non-executable stack. */
static size_t valid_image(uint8_t *img, uint16_t machine, uint16_t type)
{
    memset(img, 0, IMG);
    memcpy(img, "\177ELF", 4);
    img[4] = ELFCLASS64;
    img[5] = ELFDATA2LSB;
    img[6] = EV_CURRENT;
    put16(img + 16, type);
    put16(img + 18, machine);
    put32(img + 20, EV_CURRENT);
    uint64_t base = type == ET_DYN ? 0 : 0x400000;
    put64(img + 24, base + 0x1010);   /* entry, in the text */
    put64(img + 32, 64);               /* e_phoff */
    put16(img + 52, 64);
    put16(img + 54, 56);
    put16(img + 56, 5);
    phdr(img, 0, PT_LOAD, ELF_PF_R | ELF_PF_X, 0, base + 0x1000, 0x200, 0x200, 0x1000);
    phdr(img, 1, PT_LOAD, ELF_PF_R | ELF_PF_W, 0x200, base + 0x3200, 0x100, 0x2000, 0x200000);
    phdr(img, 2, PT_NOTE, ELF_PF_R, 0x340, 0, 20, 20, 4);
    phdr(img, 3, PT_INTERP, ELF_PF_R, 0x360, 0, 15, 15, 1);
    phdr(img, 4, PT_GNU_STACK, ELF_PF_R | ELF_PF_W, 0, 0, 0, 0, 16);
    put32(img + 0x340, 8);
    put32(img + 0x344, 0);
    put32(img + 0x348, 1);
    memcpy(img + 0x34c, "CosmoOS", 8);
    memcpy(img + 0x360, "/lib/ld-musl.s", 15);
    return 0x400;
}

static int validate(const uint8_t *img, size_t size, uint16_t machine, struct elf_info *info, const char **why)
{
    memset(info, 0, sizeof(*info));
    return elf_validate_for(img, size, USER_LO, USER_HI, machine, "not this machine", info, info->interp, why);
}

static void test_valid(void)
{
    uint8_t img[IMG];
    size_t n = valid_image(img, ELF_MACHINE_NATIVE, ET_EXEC);
    struct elf_info info;
    const char *why = NULL;
    EXPECT(elf_validate(img, n, USER_LO, USER_HI, &info, &why) == 0 && why == NULL);
    EXPECT(info.nr_segments == 2 && info.cosmo_note && info.has_interp && strcmp(info.interp, "/lib/ld-musl.s") == 0);
    EXPECT(info.entry == 0x401010 && info.lo == 0x401000 && info.hi == 0x406000 && info.max_align == 0x200000);
    EXPECT(info.phdr_vaddr == 0x401040 && info.phnum == 5 && !info.is_dyn);
    n = valid_image(img, ELF_MACHINE_NATIVE, ET_DYN);
    EXPECT(elf_validate(img, n, USER_LO, USER_HI, &info, &why) == 0 && info.is_dyn && info.entry == 0x1010);
    elf_rebase(&info, 0x10000000);
    EXPECT(info.entry == 0x10001010 && info.segments[0].vaddr == 0x10001000 && info.phdr_vaddr == 0x10001040);
    n = valid_image(img, ELF_MACHINE_NATIVE == EM_X86_64 ? EM_AARCH64 : EM_X86_64, ET_EXEC);
    EXPECT(elf_validate(img, n, USER_LO, USER_HI, &info, &why) == -ENOEXEC && strncmp(why, "not ", 4) == 0);
}

static void test_refusals(void)
{
    uint8_t img[IMG];
    struct elf_info info;
    const char *why = NULL;
    size_t n = valid_image(img, EM_X86_64, ET_EXEC);
    put32(img + 64 + 4, ELF_PF_R | ELF_PF_W | ELF_PF_X);   /* text W^X */
    EXPECT(validate(img, n, EM_X86_64, &info, &why) == -ENOEXEC && strstr(why, "W^X") != NULL);
    n = valid_image(img, EM_X86_64, ET_EXEC);
    put32(img + 64 + 4 * 56 + 4, ELF_PF_R | ELF_PF_W | ELF_PF_X);   /* executable stack */
    EXPECT(validate(img, n, EM_X86_64, &info, &why) == -ENOEXEC && strstr(why, "stack") != NULL);
    n = valid_image(img, EM_X86_64, ET_EXEC);
    img[0x360 + 14] = 'x';   /* interpreter path not terminated */
    EXPECT(validate(img, n, EM_X86_64, &info, &why) == -ENOEXEC && strstr(why, "NUL") != NULL);
    n = valid_image(img, EM_X86_64, ET_EXEC);
    EXPECT(validate(img, 63, EM_X86_64, &info, &why) == -ENOEXEC);
    put64(img + 24, 0x403300);   /* entry in the data segment */
    EXPECT(validate(img, n, EM_X86_64, &info, &why) == -ENOEXEC && strstr(why, "entry") != NULL);
}

/* --- the recorded digest --------------------------------------------------- */

#define ROUNDS 100000
#define SEED 0xE1F5A5E2026D16E5ull
/* The digest of the C validator (elfparse.c, 2026-10-31). */
#define ELF_DIGEST 0x9a2e2083c6cf3e58ull

static uint64_t g_rng;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

struct input {
    uint8_t img[IMG];
    size_t size;
    uint16_t machine;
};

/* A valid image of either type and machine, then changes: a field of the header or of a program header set to an
 * edge value, bytes flipped, the size cut. */
static void make_input(struct input *in)
{
    uint16_t machines[2] = { EM_X86_64, EM_AARCH64 };
    in->machine = machines[next() % 2];
    in->size = valid_image(in->img, next() % 8 ? in->machine : machines[next() % 2], next() % 2 ? ET_EXEC : ET_DYN);
    unsigned edits = (unsigned)(next() % 4);
    for (unsigned k = 0; k < edits; k++) {
        static const uint64_t edges[] = { 0, 1, 2, 0xfff, 0x1000, 0x1001, 0x200000, 0x400000, 0x7fffffffffff,
                                          0xffffffffffffffffull, 0x8000000000000000ull, 255, 256, 4096, 16, 56 };
        uint64_t v = next() % 2 ? edges[next() % (sizeof(edges) / sizeof(edges[0]))] : next();
        switch (next() % 4) {
        case 0: {   /* a header field */
            static const unsigned offs[] = { 16, 18, 24, 32, 54, 56 };
            static const unsigned widths[] = { 2, 2, 8, 8, 2, 2 };
            unsigned f = (unsigned)(next() % 6);
            uint8_t *p = in->img + offs[f];
            if (widths[f] == 2) put16(p, (uint16_t)v); else put64(p, v);
            break;
        }
        case 1: {   /* a program header field */
            unsigned i = (unsigned)(next() % 6);
            static const unsigned offs[] = { 0, 4, 8, 16, 32, 40, 48 };
            unsigned f = (unsigned)(next() % 7);
            uint8_t *p = in->img + 64 + 56 * i + offs[f];
            if (f < 2) put32(p, (uint32_t)v); else put64(p, v);
            break;
        }
        case 2:
            in->img[next() % IMG] ^= (uint8_t)(1u << (next() % 8));
            break;
        default:
            in->size = (size_t)(next() % (IMG + 1));
            break;
        }
    }
}

static void run_one(const struct input *in, uint64_t *digest, bool dump, unsigned *accepted)
{
    struct elf_info info;
    const char *why = NULL;
    int rc = validate(in->img, in->size, in->machine, &info, &why);
    char line[2048];
    int n = snprintf(line, sizeof(line), "r%d %s e%llx l%llx h%llx n%u c%d p%llx %u:%u d%d a%llx i%d:%s", rc,
                     why ? why : "-", (unsigned long long)info.entry, (unsigned long long)info.lo,
                     (unsigned long long)info.hi, info.nr_segments, info.cosmo_note,
                     (unsigned long long)info.phdr_vaddr, info.phnum, info.phent, info.is_dyn,
                     (unsigned long long)info.max_align, info.has_interp, info.has_interp ? info.interp : "");
    for (unsigned i = 0; i < info.nr_segments && i < ELF_MAX_SEGMENTS; i++) {
        const struct elf_segment *s = &info.segments[i];
        n += snprintf(line + n, sizeof(line) - (size_t)n, " [%llx %llx %llx %llx %llx %llx %x]",
                      (unsigned long long)s->vaddr, (unsigned long long)s->memsz, (unsigned long long)s->offset,
                      (unsigned long long)s->filesz, (unsigned long long)s->file_vaddr,
                      (unsigned long long)s->file_memsz, s->flags);
    }
    if (rc == 0) {
        *accepted += 1;
        elf_rebase(&info, 0x7000000000ull);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " R%llx %llx %llx %llx", (unsigned long long)info.entry,
                      (unsigned long long)info.lo, (unsigned long long)info.phdr_vaddr,
                      (unsigned long long)info.segments[0].file_vaddr);
    }
    EXPECT(n > 0 && (size_t)n < sizeof(line));
    for (int i = 0; i < n; i++) {
        *digest ^= (uint8_t)line[i];
        *digest *= 0x100000001b3ull;
    }
    *digest ^= '\n';
    *digest *= 0x100000001b3ull;
    if (dump)
        printf("%s\n", line);
}

static void test_recorded_digest(void)
{
    bool dump = getenv("ELF_DUMP") != NULL;
    uint64_t digest = 0xcbf29ce484222325ull;
    unsigned accepted = 0;
    g_rng = SEED;
    struct input *in = malloc(sizeof(*in));
    EXPECT(in != NULL);
    if (in == NULL)
        return;
    for (unsigned r = 0; r < ROUNDS; r++) {
        make_input(in);
        run_one(in, &digest, dump, &accepted);
    }
    free(in);
    printf("  elf digest 0x%016llx over %u images (accepted %u)\n", (unsigned long long)digest, ROUNDS, accepted);
    EXPECT(accepted > 1000);
    EXPECT(digest == ELF_DIGEST);
}

static volatile uint64_t g_sink;

static int bench(void)
{
    enum { N = 20000 };
    struct input *ins = malloc(sizeof(*ins) * N);
    if (ins == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned r = 0; r < N; r++)
        make_input(&ins[r]);
    double best = 1e30;
    for (int run = 0; run < 9; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < N; r++) {
            struct elf_info info;
            const char *why;
            acc += (uint64_t)validate(ins[r].img, ins[r].size, ins[r].machine, &info, &why) + info.nr_segments;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("elf bench: %.2f ns per image (best of 9 runs of %u)\n", best / N, N);
    free(ins);
    return 0;
}

static const struct host_test tests[] = {
    { "valid executables", test_valid },
    { "refusals", test_refusals },
    { "recorded digest", test_recorded_digest },
};

int main(void)
{
    if (getenv("ELF_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
