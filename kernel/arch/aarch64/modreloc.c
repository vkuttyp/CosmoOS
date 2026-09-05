/*
 * modreloc.c - Kernel module relocations for AArch64
 * (docs/kernel/arch/aarch64/design.md, "Modules").
 *
 * Modules are `ld.lld -r` objects built with -mcmodel=small and land in
 * the near arena within +-128 MiB of the kernel, so CALL26/JUMP26 reach
 * every export. The patcher is a pure function over bytes (host-tested).
 */

#include <kernel/errno.h>
#include <kernel/string.h>
#include <arch/module.h>
#include <aarch64/modreloc.h>

static inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static inline void wr64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static bool fits_signed(int64_t v, unsigned bits)
{
    int64_t lim = (int64_t)1 << (bits - 1);
    return v >= -lim && v < lim;
}

static int patch_imm(uint8_t *where, uint32_t mask, uint32_t value, unsigned shift)
{
    uint32_t insn = rd32(where);
    insn = (insn & ~(mask << shift)) | ((value & mask) << shift);
    wr32(where, insn);
    return 0;
}

/* adr/adrp immediate: immlo (2 bits) at 29, immhi (19 bits) at 5. */
static void patch_adr(uint8_t *where, int64_t x)
{
    uint32_t insn = rd32(where);
    uint32_t immlo = (uint32_t)(x & 3);
    uint32_t immhi = (uint32_t)((x >> 2) & 0x7FFFF);
    insn &= ~((3u << 29) | (0x7FFFFu << 5));
    insn |= (immlo << 29) | (immhi << 5);
    wr32(where, insn);
}

unsigned aarch64_reloc_width(uint32_t type)
{
    switch (type) {
    case R_AARCH64_NONE:
    case R_AARCH64_NONE_ALT:
        return 0;
    case R_AARCH64_ABS64:
    case R_AARCH64_PREL64:
        return 8;
    case R_AARCH64_ABS16:
    case R_AARCH64_PREL16:
        return 2;
    default:
        return 4;
    }
}

int aarch64_reloc_apply(uint32_t type, uint8_t *where, uint64_t P, uint64_t S, int64_t A, const char **why)
{
    int64_t sa = (int64_t)S + A;
    int64_t x = sa - (int64_t)P;
    switch (type) {
    case R_AARCH64_NONE:
    case R_AARCH64_NONE_ALT:
        return 0;
    case R_AARCH64_ABS64:
        wr64(where, (uint64_t)sa);
        return 0;
    case R_AARCH64_ABS32:
        if (!fits_signed(sa, 32) && (sa < 0 || sa > (int64_t)UINT32_MAX)) {
            *why = "ABS32 relocation out of range";
            return -ERANGE;
        }
        wr32(where, (uint32_t)(uint64_t)sa);
        return 0;
    case R_AARCH64_ABS16: {
        if (sa < INT16_MIN || sa > (int64_t)UINT16_MAX) {
            *why = "ABS16 relocation out of range";
            return -ERANGE;
        }
        uint16_t v = (uint16_t)(uint64_t)sa;
        memcpy(where, &v, 2);
        return 0;
    }
    case R_AARCH64_PREL64:
        wr64(where, (uint64_t)x);
        return 0;
    case R_AARCH64_PREL32:
        if (!fits_signed(x, 32)) {
            *why = "PREL32 relocation out of range";
            return -ERANGE;
        }
        wr32(where, (uint32_t)(uint64_t)x);
        return 0;
    case R_AARCH64_PREL16: {
        if (!fits_signed(x, 16)) {
            *why = "PREL16 relocation out of range";
            return -ERANGE;
        }
        uint16_t v = (uint16_t)(uint64_t)x;
        memcpy(where, &v, 2);
        return 0;
    }
    case R_AARCH64_ADR_PREL_LO21:
        if (!fits_signed(x, 21)) {
            *why = "ADR_PREL_LO21 relocation out of range";
            return -ERANGE;
        }
        patch_adr(where, x);
        return 0;
    case R_AARCH64_ADR_PREL_PG_HI21: {
        int64_t page = (int64_t)((uint64_t)sa & ~0xFFFull) - (int64_t)(P & ~0xFFFull);
        if (!fits_signed(page, 33)) {
            *why = "ADR_PREL_PG_HI21 relocation out of range (+-4 GiB)";
            return -ERANGE;
        }
        patch_adr(where, page >> 12);
        return 0;
    }
    case R_AARCH64_ADD_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)(sa & 0xFFF), 10);
    case R_AARCH64_LDST8_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)(sa & 0xFFF), 10);
    case R_AARCH64_LDST16_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)((sa & 0xFFF) >> 1), 10);
    case R_AARCH64_LDST32_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)((sa & 0xFFF) >> 2), 10);
    case R_AARCH64_LDST64_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)((sa & 0xFFF) >> 3), 10);
    case R_AARCH64_LDST128_ABS_LO12_NC:
        return patch_imm(where, 0xFFF, (uint32_t)((sa & 0xFFF) >> 4), 10);
    case R_AARCH64_LD_PREL_LO19:
        if ((x & 3) || !fits_signed(x, 21)) {
            *why = "LD_PREL_LO19 relocation out of range";
            return -ERANGE;
        }
        return patch_imm(where, 0x7FFFF, (uint32_t)((x >> 2) & 0x7FFFF), 5);
    case R_AARCH64_TSTBR14:
        if ((x & 3) || !fits_signed(x, 16)) {
            *why = "TSTBR14 relocation out of range";
            return -ERANGE;
        }
        return patch_imm(where, 0x3FFF, (uint32_t)((x >> 2) & 0x3FFF), 5);
    case R_AARCH64_CONDBR19:
        if ((x & 3) || !fits_signed(x, 21)) {
            *why = "CONDBR19 relocation out of range (+-1 MiB)";
            return -ERANGE;
        }
        return patch_imm(where, 0x7FFFF, (uint32_t)((x >> 2) & 0x7FFFF), 5);
    case R_AARCH64_JUMP26:
    case R_AARCH64_CALL26:
        if ((x & 3) || !fits_signed(x, 28)) {
            *why = "CALL26/JUMP26 relocation out of range (+-128 MiB)";
            return -ERANGE;
        }
        return patch_imm(where, 0x3FFFFFF, (uint32_t)((x >> 2) & 0x3FFFFFF), 0);
    default:
        *why = "unsupported AArch64 relocation type";
        return -ENOEXEC;
    }
}

int arch_module_reloc(vaddr_t target, size_t target_size, const struct elf64_rela *rela, size_t count,
                      const uintptr_t *sym_addr, size_t nr_syms, const char **why)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym = ELF64_R_SYM(rela[i].r_info);
        uint64_t off = rela[i].r_offset;
        unsigned width = aarch64_reloc_width(type);
        if (width == 0 && (type == R_AARCH64_NONE || type == R_AARCH64_NONE_ALT))
            continue;
        if (width == 0)
            width = 4;
        if (sym >= nr_syms) {
            *why = "relocation names a symbol outside the table";
            return -EINVAL;
        }
        if (off > target_size || target_size - off < width) {
            *why = "relocation offset outside the section";
            return -EINVAL;
        }
        vaddr_t where = target + off;
        int rc = aarch64_reloc_apply(type, (uint8_t *)where, (uint64_t)where, (uint64_t)sym_addr[sym],
                                     rela[i].r_addend, why);
        if (rc)
            return rc;
    }
    return 0;
}
