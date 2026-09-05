/*
 * elf64.h - ELF64 file structures and the constants the kernel uses.
 *
 * Shared by the user executable loader (kernel/process/elf.c, ET_EXEC)
 * and the kernel module loader (kernel/module/modelf.c, ET_REL). Pure
 * definitions; both users treat the bytes behind them as untrusted.
 */

#ifndef KERNEL_ELF64_H
#define KERNEL_ELF64_H

#include <stdint.h>

#define EI_NIDENT   16
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3

#define EM_X86_64  62
#define EM_AARCH64 183
/* The machine this kernel runs and loads programs and modules for. */
#if defined(ARCH_AARCH64)
#define ELF_MACHINE_NATIVE EM_AARCH64
#define ELF_MACHINE_NATIVE_NAME "AArch64"
#else
#define ELF_MACHINE_NATIVE EM_X86_64
#define ELF_MACHINE_NATIVE_NAME "x86-64"
#endif

#define PT_LOAD      1
#define PT_NOTE      4
#define PT_INTERP    3
#define PT_TLS       7
#define PT_GNU_STACK 0x6474e551u

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_DYNSYM   11

#define SHF_WRITE     0x1u
#define SHF_ALLOC     0x2u
#define SHF_EXECINSTR 0x4u

#define SHN_UNDEF  0
#define SHN_LORESERVE 0xff00
#define SHN_ABS    0xfff1
#define SHN_COMMON 0xfff2
#define SHN_XINDEX 0xffff

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STT_SECTION 3

#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xffffffffu))

/* x86-64 relocation types the module loader understands. */
#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct elf64_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};

#endif /* KERNEL_ELF64_H */
