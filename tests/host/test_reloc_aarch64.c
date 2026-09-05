/*
 * test_reloc_aarch64.c - Host test of the AArch64 module relocation patcher
 * (docs/kernel/arch/aarch64/testing.md): every encoding the loader
 * supports, its range checks, and arch_module_reloc's bounds. ASan/UBSan.
 * Runs on any host: the patcher is pure arithmetic over a buffer.
 */

#include "harness.h"

#include <kernel/elf64.h>
#include <kernel/errno.h>
#include <arch/module.h>
#include <aarch64/modreloc.h>

#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int main(void)
{
    uint8_t buf[16];
    const char *why = "";
    const uint64_t P = 0xFFFFFFFF88001000ull;            /* an instruction in the near arena */

    /* widths */
    CHECK(aarch64_reloc_width(R_AARCH64_ABS64) == 8 && aarch64_reloc_width(R_AARCH64_CALL26) == 4 &&
          aarch64_reloc_width(R_AARCH64_NONE) == 0 && aarch64_reloc_width(R_AARCH64_ABS16) == 2);

    /* ABS64 / PREL32 / PREL64 */
    memset(buf, 0, sizeof(buf));
    CHECK(aarch64_reloc_apply(R_AARCH64_ABS64, buf, P, 0x1122334455667788ull, 8, &why) == 0);
    CHECK(rd64(buf) == 0x1122334455667790ull);
    CHECK(aarch64_reloc_apply(R_AARCH64_PREL32, buf, P, P + 0x1000, -16, &why) == 0 && rd32(buf) == 0x1000 - 16);
    CHECK(aarch64_reloc_apply(R_AARCH64_PREL32, buf, P, P + (1ull << 40), 0, &why) == -ERANGE);
    CHECK(aarch64_reloc_apply(R_AARCH64_PREL64, buf, P, P - 8, 0, &why) == 0 && rd64(buf) == (uint64_t)-8);
    CHECK(aarch64_reloc_apply(R_AARCH64_ABS32, buf, P, 0xFFFFFFFFull, 0, &why) == 0 && rd32(buf) == 0xFFFFFFFFu);
    CHECK(aarch64_reloc_apply(R_AARCH64_ABS32, buf, P, 0x1FFFFFFFFull, 0, &why) == -ERANGE);

    /* CALL26: bl to +0x1000 -> imm26 = 0x400; opcode bits preserved */
    uint32_t bl = 0x94000000u;
    memcpy(buf, &bl, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_CALL26, buf, P, P + 0x1000, 0, &why) == 0);
    CHECK(rd32(buf) == (0x94000000u | 0x400u));
    memcpy(buf, &bl, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_JUMP26, buf, P, P - 0x2000, 0, &why) == 0);
    CHECK((rd32(buf) & 0x3FFFFFFu) == ((uint32_t)(-0x2000 >> 2) & 0x3FFFFFFu));
    CHECK(aarch64_reloc_apply(R_AARCH64_CALL26, buf, P, P + (128ull << 20), 0, &why) == -ERANGE);   /* +128 MiB */
    CHECK(aarch64_reloc_apply(R_AARCH64_CALL26, buf, P, P + 2, 0, &why) == -ERANGE);               /* unaligned */
    CHECK(aarch64_reloc_apply(R_AARCH64_CALL26, buf, P, P - (128ull << 20), 0, &why) == 0);        /* -128 MiB fits */

    /* ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC: the adrp/add pair for S = P + 0x12345 */
    uint32_t adrp = 0x90000000u;   /* adrp x0, . */
    uint32_t add = 0x91000000u;    /* add x0, x0, #0 */
    uint64_t S = P + 0x12345;
    memcpy(buf, &adrp, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_ADR_PREL_PG_HI21, buf, P, S, 0, &why) == 0);
    {
        uint32_t insn = rd32(buf);
        int64_t pages = (int64_t)((S & ~0xFFFull) - (P & ~0xFFFull)) >> 12;   /* 0x12 */
        uint32_t immlo = (insn >> 29) & 3, immhi = (insn >> 5) & 0x7FFFF;
        int64_t got = (int64_t)((immhi << 2) | immlo);
        CHECK(got == pages && (insn & 0x9F00001Fu) == 0x90000000u);
    }
    memcpy(buf + 4, &add, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_ADD_ABS_LO12_NC, buf + 4, P + 4, S, 0, &why) == 0);
    CHECK(((rd32(buf + 4) >> 10) & 0xFFF) == (S & 0xFFF));
    CHECK(aarch64_reloc_apply(R_AARCH64_ADR_PREL_PG_HI21, buf, P, P + (5ull << 30), 0, &why) == -ERANGE);   /* > 4 GiB */

    /* LDST scaled immediates */
    uint32_t ldr = 0xF9400000u;   /* ldr x0, [x0] */
    memcpy(buf, &ldr, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_LDST64_ABS_LO12_NC, buf, P, 0x1000 + 0x568, 0, &why) == 0);
    CHECK(((rd32(buf) >> 10) & 0xFFF) == 0x568 / 8);
    memcpy(buf, &ldr, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_LDST32_ABS_LO12_NC, buf, P, 0x1000 + 0x564, 0, &why) == 0);
    CHECK(((rd32(buf) >> 10) & 0xFFF) == 0x564 / 4);
    memcpy(buf, &ldr, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_LDST8_ABS_LO12_NC, buf, P, 0x1000 + 0x567, 0, &why) == 0);
    CHECK(((rd32(buf) >> 10) & 0xFFF) == 0x567);
    memcpy(buf, &ldr, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_LDST128_ABS_LO12_NC, buf, P, 0x1000 + 0x560, 0, &why) == 0);
    CHECK(((rd32(buf) >> 10) & 0xFFF) == 0x560 / 16);

    /* CONDBR19 / TSTBR14 / LD_PREL_LO19 */
    uint32_t bcond = 0x54000000u;
    memcpy(buf, &bcond, 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_CONDBR19, buf, P, P + 0x100, 0, &why) == 0);
    CHECK(((rd32(buf) >> 5) & 0x7FFFF) == 0x100 / 4);
    CHECK(aarch64_reloc_apply(R_AARCH64_CONDBR19, buf, P, P + (1ull << 20), 0, &why) == -ERANGE);
    CHECK(aarch64_reloc_apply(R_AARCH64_TSTBR14, buf, P, P + 0x40, 0, &why) == 0 && ((rd32(buf) >> 5) & 0x3FFF) == 0x10);
    CHECK(aarch64_reloc_apply(R_AARCH64_TSTBR14, buf, P, P + (1ull << 15), 0, &why) == -ERANGE);
    CHECK(aarch64_reloc_apply(R_AARCH64_LD_PREL_LO19, buf, P, P + 0x200, 0, &why) == 0);

    /* unknown type */
    CHECK(aarch64_reloc_apply(999, buf, P, 0, 0, &why) == -ENOEXEC && why[0] != '\0');
    CHECK(aarch64_reloc_apply(R_AARCH64_NONE, buf, P, 0, 0, &why) == 0);

    /* arch_module_reloc: bounds and symbol checks over a section image */
    uint8_t section[64];
    memset(section, 0, sizeof(section));
    memcpy(section + 8, &bl, 4);
    uintptr_t syms[2] = { 0, (uintptr_t)section + 0x100 };
    struct elf64_rela r[2] = {
        { .r_offset = 8, .r_info = ((uint64_t)1 << 32) | R_AARCH64_CALL26, .r_addend = 0 },
        { .r_offset = 16, .r_info = ((uint64_t)1 << 32) | R_AARCH64_ABS64, .r_addend = 4 },
    };
    CHECK(arch_module_reloc((vaddr_t)section, sizeof(section), r, 2, syms, 2, &why) == 0);
    CHECK((rd32(section + 8) & 0x3FFFFFFu) == ((0x100u - 8u) >> 2));
    CHECK(rd64(section + 16) == (uint64_t)((uintptr_t)section + 0x104));
    struct elf64_rela bad = { .r_offset = 60, .r_info = ((uint64_t)1 << 32) | R_AARCH64_ABS64, .r_addend = 0 };
    CHECK(arch_module_reloc((vaddr_t)section, sizeof(section), &bad, 1, syms, 2, &why) == -EINVAL);
    struct elf64_rela badsym = { .r_offset = 0, .r_info = ((uint64_t)7 << 32) | R_AARCH64_ABS64, .r_addend = 0 };
    CHECK(arch_module_reloc((vaddr_t)section, sizeof(section), &badsym, 1, syms, 2, &why) == -EINVAL);

    if (g_failures) {
        printf("reloc-aarch64                 FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("reloc-aarch64                 ok\n");
    return 0;
}
