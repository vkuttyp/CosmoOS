/*
 * elf.c - Static ELF64 executables for user processes.
 *
 * elf_validate is deliberately free of kernel dependencies beyond
 * string.h so the host unit tests can feed it crafted images. Every
 * offset and size is checked against the buffer before it is read.
 */

#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/string.h>

#ifndef ELF_HOST_TEST
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/vmm.h>
#endif

#define EI_NIDENT 16
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_GNU_STACK 0x6474e551u

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

#define ELF_PAGE 4096ULL

static bool in_file(uint64_t off, uint64_t len, size_t size)
{
    return off <= size && len <= size - off;
}

static int fail(const char **why, const char *msg)
{
    if (why)
        *why = msg;
    return -ENOEXEC;
}

int elf_validate(const void *image, size_t size, uint64_t user_lo, uint64_t user_hi, struct elf_info *info,
                 const char **why)
{
    const uint8_t *file = image;
    struct elf64_ehdr eh;

    memset(info, 0, sizeof(*info));
    if (why)
        *why = NULL;

    if (size < sizeof(eh))
        return fail(why, "file shorter than the ELF header");
    memcpy(&eh, file, sizeof(eh));

    if (memcmp(eh.e_ident, "\177ELF", 4) != 0)
        return fail(why, "bad ELF magic");
    if (eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB || eh.e_ident[6] != EV_CURRENT)
        return fail(why, "not ELF64 little-endian v1");
    if (eh.e_type != ET_EXEC)
        return fail(why, "not ET_EXEC (static executables only)");
    if (eh.e_machine != EM_X86_64)
        return fail(why, "not x86-64");
    if (eh.e_phentsize != sizeof(struct elf64_phdr) || eh.e_phnum == 0)
        return fail(why, "bad program header table");
    if (!in_file(eh.e_phoff, (uint64_t)eh.e_phnum * sizeof(struct elf64_phdr), size))
        return fail(why, "program header table outside the file");

    uint64_t lo = UINT64_MAX, hi = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        memcpy(&ph, file + eh.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));

        if (ph.p_type == PT_INTERP)
            return fail(why, "PT_INTERP: dynamic executables are not supported");
        if (ph.p_type == PT_GNU_STACK) {
            if (ph.p_flags & ELF_PF_X)
                return fail(why, "PT_GNU_STACK requests an executable stack");
            continue;
        }
        if (ph.p_type != PT_LOAD)
            continue;

        if (ph.p_memsz == 0)
            continue;
        if (ph.p_memsz < ph.p_filesz)
            return fail(why, "PT_LOAD memsz smaller than filesz");
        if (!in_file(ph.p_offset, ph.p_filesz, size))
            return fail(why, "PT_LOAD file bytes outside the file");
        if (ph.p_vaddr + ph.p_memsz < ph.p_vaddr)
            return fail(why, "PT_LOAD address range overflows");
        if ((ph.p_flags & ELF_PF_W) && (ph.p_flags & ELF_PF_X))
            return fail(why, "PT_LOAD is writable and executable (W^X)");
        if ((ph.p_vaddr & (ELF_PAGE - 1)) != (ph.p_offset & (ELF_PAGE - 1)))
            return fail(why, "PT_LOAD vaddr and offset are not congruent modulo the page size");

        uint64_t seg_lo = ph.p_vaddr & ~(ELF_PAGE - 1);
        uint64_t seg_hi = (ph.p_vaddr + ph.p_memsz + ELF_PAGE - 1) & ~(ELF_PAGE - 1);
        if (seg_lo < user_lo || seg_hi > user_hi || seg_hi < seg_lo)
            return fail(why, "PT_LOAD outside the user address range");

        for (unsigned j = 0; j < info->nr_segments; j++) {
            const struct elf_segment *o = &info->segments[j];
            if (seg_lo < o->vaddr + o->memsz && o->vaddr < seg_hi)
                return fail(why, "PT_LOAD segments share a page");
        }
        if (info->nr_segments >= ELF_MAX_SEGMENTS)
            return fail(why, "too many PT_LOAD segments");

        struct elf_segment *seg = &info->segments[info->nr_segments++];
        seg->vaddr = seg_lo;
        seg->memsz = seg_hi - seg_lo;
        seg->offset = ph.p_offset;
        seg->filesz = ph.p_filesz;
        seg->file_vaddr = ph.p_vaddr;
        seg->flags = ph.p_flags & (ELF_PF_R | ELF_PF_W | ELF_PF_X);

        if (seg_lo < lo)
            lo = seg_lo;
        if (seg_hi > hi)
            hi = seg_hi;
    }

    if (info->nr_segments == 0)
        return fail(why, "no PT_LOAD segments");

    bool entry_ok = false;
    for (unsigned j = 0; j < info->nr_segments; j++) {
        const struct elf_segment *s = &info->segments[j];
        if ((s->flags & ELF_PF_X) && eh.e_entry >= s->vaddr && eh.e_entry < s->vaddr + s->memsz)
            entry_ok = true;
    }
    if (!entry_ok)
        return fail(why, "entry point is not inside an executable segment");

    info->entry = eh.e_entry;
    info->lo = lo;
    info->hi = hi;
    return 0;
}

#ifndef ELF_HOST_TEST

static vm_prot_t seg_prot(uint32_t flags)
{
    vm_prot_t p = 0;
    if (flags & ELF_PF_R)
        p |= VM_PROT_READ;
    if (flags & ELF_PF_W)
        p |= VM_PROT_WRITE;
    if (flags & ELF_PF_X)
        p |= VM_PROT_EXEC;
    if (p == 0)
        p = VM_PROT_READ;
    return p;
}

int elf_load_into(struct vm_space *space, const void *image, const struct elf_info *info)
{
    const uint8_t *file = image;

    for (unsigned i = 0; i < info->nr_segments; i++) {
        const struct elf_segment *s = &info->segments[i];

        /* Map writable while populating, then set the final protection:
         * the copy goes through the direct map, but a read-only region
         * would still be recorded read-only and query would disagree. */
        int rc = vm_user_map_anon(space, s->vaddr, (size_t)s->memsz, VM_PROT_RW, VM_REGION_POPULATED,
                                  "elf-segment");
        if (rc)
            return rc;

        /* Copy file bytes frame by frame through the direct map. */
        uint64_t src_off = s->offset;
        uint64_t dst = s->file_vaddr;
        uint64_t remaining = s->filesz;
        while (remaining > 0) {
            paddr_t pa;
            bool present = arch_mmu_query(&space->mmu, (vaddr_t)dst, &pa, NULL, NULL, NULL);
            if (!present)
                return -EFAULT; /* populated region: cannot happen */
            size_t in_page = (size_t)(PAGE_SIZE - (dst & (PAGE_SIZE - 1)));
            size_t n = remaining < in_page ? (size_t)remaining : in_page;
            memcpy(phys_to_virt(pa), file + src_off, n);
            src_off += n;
            dst += n;
            remaining -= n;
        }

        rc = vm_user_protect(space, s->vaddr, (size_t)s->memsz, seg_prot(s->flags));
        if (rc)
            return rc;
    }
    return 0;
}

#endif /* ELF_HOST_TEST */
