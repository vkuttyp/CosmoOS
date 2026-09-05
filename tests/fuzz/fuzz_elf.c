/*
 * fuzz_elf.c - The user executable validator (kernel/process/elf.c,
 * elf_validate) under fuzzing. Built with ELF_HOST_TEST, which leaves out
 * elf_load_into (it needs the VMM). Any input must be validated without
 * reading outside it; an accepted image's segments must lie inside the
 * image and the user window.
 */

#include "fuzz.h"

#include <kernel/elf.h>
#include <kernel/elf64.h>
#include <kernel/errno.h>

#include <string.h>

#define USER_LO 0x0000000000400000ull
#define USER_HI 0x00007fffffffffffull

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct elf_info info;
    const char *why = NULL;
    memset(&info, 0, sizeof(info));
    int rc = elf_validate(data, size, USER_LO, USER_HI, &info, &why);
    if (rc == 0) {
        FUZZ_ASSERT(info.nr_segments <= ELF_MAX_SEGMENTS);
        for (unsigned i = 0; i < info.nr_segments; i++) {
            const struct elf_segment *s = &info.segments[i];
            FUZZ_ASSERT(s->offset <= size && s->filesz <= size - s->offset);
            FUZZ_ASSERT(s->vaddr >= USER_LO && s->vaddr + s->memsz <= USER_HI && s->memsz >= s->filesz);
        }
        FUZZ_ASSERT(info.entry >= info.lo && info.entry < info.hi);
    } else {
        FUZZ_ASSERT(rc == -ENOEXEC);
        FUZZ_ASSERT(why != NULL);
    }
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* A minimal valid static executable: one PT_LOAD RX segment holding the
 * headers, entry just past them. */
size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (i != 0 || cap < 256)
        return 0;
    memset(buf, 0, 256);
    memcpy(buf, "\x7f" "ELF", 4);
    buf[4] = 2;   /* ELFCLASS64 */
    buf[5] = 1;   /* little endian */
    buf[6] = 1;   /* EV_CURRENT */
    put16(buf + 16, 2);           /* ET_EXEC */
    put16(buf + 18, 62);          /* EM_X86_64 (the validator accepts the build's machine; the
                                     fuzzer mutates it anyway) */
    put32(buf + 20, 1);           /* e_version */
    put64(buf + 24, USER_LO + 0x80);   /* e_entry */
    put64(buf + 32, 64);          /* e_phoff */
    put16(buf + 52, 64);          /* e_ehsize */
    put16(buf + 54, 56);          /* e_phentsize */
    put16(buf + 56, 1);           /* e_phnum */
    put16(buf + 58, 64);          /* e_shentsize */
    uint8_t *ph = buf + 64;
    put32(ph + 0, 1);             /* PT_LOAD */
    put32(ph + 4, 5);             /* PF_R | PF_X */
    put64(ph + 8, 0);             /* p_offset */
    put64(ph + 16, USER_LO);      /* p_vaddr */
    put64(ph + 24, USER_LO);      /* p_paddr */
    put64(ph + 32, 256);          /* p_filesz */
    put64(ph + 40, 256);          /* p_memsz */
    put64(ph + 48, 4096);         /* p_align */
    buf[0x80] = 0xc3;             /* ret */
    return 256;
}
