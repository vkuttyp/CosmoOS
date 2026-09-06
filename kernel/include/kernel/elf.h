/*
 * elf.h - Static ELF64 executable validation and loading into a user
 * address space.
 *
 * elf_validate is a pure function over a byte buffer (host-testable):
 * it either fills `struct elf_info` or returns -ENOEXEC with the failing
 * rule logged. elf_load_into maps and copies segments into a user
 * vm_space and never touches the image through user mappings.
 */

#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <kernel/types.h>

#define ELF_MAX_SEGMENTS 16

#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

struct elf_segment {
    uint64_t vaddr;    /* page aligned */
    uint64_t memsz;    /* page rounded span from vaddr */
    uint64_t offset;   /* file offset of the first byte */
    uint64_t filesz;
    uint64_t file_vaddr; /* unaligned p_vaddr, where file bytes land */
    uint32_t flags;    /* ELF_PF_* */
};

struct elf_info {
    uint64_t entry;
    uint64_t lo;       /* lowest segment start */
    uint64_t hi;       /* highest segment end */
    unsigned nr_segments;
    struct elf_segment segments[ELF_MAX_SEGMENTS];
    bool cosmo_note;   /* PT_NOTE "CosmoOS" type 1: a native program (docs/compat/linux/) */
    uint64_t phdr_vaddr; /* the program header table's address in the image, 0 when not loaded */
    uint16_t phnum, phent;
    /* Milestone 10: position-independent executables and interpreters. */
    bool is_dyn;       /* ET_DYN: every address above is relative until elf_rebase */
    bool has_interp;   /* PT_INTERP named a program interpreter */
    char interp[256];  /* its path (NUL-terminated, at most 255 bytes) */
};

/* Validate a native static executable for the user range
 * [user_lo, user_hi). On failure returns -ENOEXEC and, when `why` is
 * non-NULL, points it at an immortal string naming the rule. */
int elf_validate(const void *image, size_t size, uint64_t user_lo, uint64_t user_hi, struct elf_info *info,
                 const char **why);

/* Add `base` to every address of an ET_DYN image's info (segments, entry,
 * lo, hi, the program header table). The caller checks that the result
 * still lies inside the user window. */
void elf_rebase(struct elf_info *info, uint64_t base);

struct vm_space;

/* Map every segment of a validated image into `space` and copy its
 * bytes. Returns 0, -ENOMEM, or -EEXIST (overlap with an existing
 * region). On failure the caller destroys the space. */
int elf_load_into(struct vm_space *space, const void *image, const struct elf_info *info);

#endif /* KERNEL_ELF_H */
