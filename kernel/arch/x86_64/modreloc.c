/*
 * modreloc.c - x86-64 relocations for ET_REL kernel modules.
 *
 * Modules are compiled like the kernel (-mcmodel=kernel, non-PIC) and
 * live inside the top 2 GiB, so clang emits only R_X86_64_64, PC32,
 * PLT32 (a direct call; there is no PLT), 32 and 32S. GOT-relative
 * forms never appear for non-PIC code and are refused, as is anything
 * else. Every write goes through the read-write mapping of the target
 * section while it is still RW; the caller flips protections afterwards.
 */

#include <kernel/errno.h>
#include <kernel/string.h>

#include <arch/module.h>

static int put32(vaddr_t where, int64_t value, bool is_signed, const char **why)
{
    if (is_signed) {
        if (value < INT32_MIN || value > INT32_MAX) {
            *why = "relocation overflow (32S/PC32)";
            return -ERANGE;
        }
    } else if (value < 0 || value > (int64_t)UINT32_MAX) {
        *why = "relocation overflow (32)";
        return -ERANGE;
    }
    uint32_t v = (uint32_t)(uint64_t)value;
    memcpy((void *)where, &v, sizeof(v));
    return 0;
}

int arch_module_reloc(vaddr_t target, size_t target_size, const struct elf64_rela *rela, size_t count,
                      const uintptr_t *sym_addr, size_t nr_syms, const char **why)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym = ELF64_R_SYM(rela[i].r_info);
        uint64_t off = rela[i].r_offset;
        size_t width = (type == R_X86_64_64) ? 8 : 4;

        if (type == R_X86_64_NONE)
            continue;
        if (sym >= nr_syms) {
            *why = "relocation symbol index out of range";
            return -EINVAL;
        }
        if (off > target_size || target_size - off < width) {
            *why = "relocation offset outside its section";
            return -EINVAL;
        }

        vaddr_t where = target + off;
        int64_t s = (int64_t)sym_addr[sym];
        int64_t a = rela[i].r_addend;
        int rc = 0;

        switch (type) {
        case R_X86_64_64: {
            uint64_t v = (uint64_t)(s + a);
            memcpy((void *)where, &v, sizeof(v));
            break;
        }
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            rc = put32(where, s + a - (int64_t)where, true, why);
            break;
        case R_X86_64_32:
            rc = put32(where, s + a, false, why);
            break;
        case R_X86_64_32S:
            rc = put32(where, s + a, true, why);
            break;
        default:
            *why = "unsupported relocation type";
            return -ENOEXEC;
        }
        if (rc)
            return rc;
    }
    return 0;
}
