/*
 * elfparse.c - The user executable validator (kernel/include/kernel/elf.h):
 * every offset and size checked against the buffer before it is read. The
 * C step of roadmap 2.0 SA5's ELF family, moved unchanged from elf.c, to be
 * replaced by elfparse.cee once tests/host/test_elf.c's recorded digest
 * agrees. elf.c keeps the loader, which maps into the VMM.
 */

#include <kernel/elf.h>
#include <kernel/elf64.h>
#include <kernel/errno.h>
#include <kernel/string.h>

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

int elf_validate_for(const void *image, size_t size, uint64_t user_lo, uint64_t user_hi, uint16_t machine,
                     const char *not_machine, struct elf_info *info, char *interp, const char **why)
{
    const uint8_t *file = image;
    struct elf64_ehdr eh;

    if (why)
        *why = NULL;

    if (size < sizeof(eh))
        return fail(why, "file shorter than the ELF header");
    memcpy(&eh, file, sizeof(eh));

    if (memcmp(eh.e_ident, "\177ELF", 4) != 0)
        return fail(why, "bad ELF magic");
    if (eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB || eh.e_ident[6] != EV_CURRENT)
        return fail(why, "not ELF64 little-endian v1");
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
        return fail(why, "not ET_EXEC or ET_DYN");
    info->is_dyn = eh.e_type == ET_DYN;
    /* An ET_DYN image is validated relative to 0 and rebased by the caller;
     * it must fit the window from wherever it is placed. */
    uint64_t lo_bound = info->is_dyn ? 0 : user_lo;
    uint64_t hi_bound = info->is_dyn ? user_hi - user_lo : user_hi;
    if (eh.e_machine != machine)
        return fail(why, not_machine);
    if (eh.e_phentsize != sizeof(struct elf64_phdr) || eh.e_phnum == 0)
        return fail(why, "bad program header table");
    if (!in_file(eh.e_phoff, (uint64_t)eh.e_phnum * sizeof(struct elf64_phdr), size))
        return fail(why, "program header table outside the file");

    uint64_t lo = UINT64_MAX, hi = 0;
    info->phnum = eh.e_phnum;
    info->phent = eh.e_phentsize;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        memcpy(&ph, file + eh.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));

        if (ph.p_type == PT_INTERP) {
            /* The interpreter's path: inside the file, 1..255 bytes, NUL-terminated. */
            if (info->has_interp)
                return fail(why, "two PT_INTERP entries");
            if (ph.p_filesz < 2 || ph.p_filesz > ELF_INTERP_MAX - 1 || !in_file(ph.p_offset, ph.p_filesz, size))
                return fail(why, "PT_INTERP path is empty, too long or outside the file");
            if (file[ph.p_offset + ph.p_filesz - 1] != '\0')
                return fail(why, "PT_INTERP path is not NUL-terminated");
            memcpy(interp, file + ph.p_offset, (size_t)ph.p_filesz);
            info->has_interp = true;
            continue;
        }
        if (ph.p_type == PT_NOTE) {
            /* Notes: namesz, descsz, type, name (padded to 4), desc (padded to 4). */
            if (!in_file(ph.p_offset, ph.p_filesz, size))
                return fail(why, "PT_NOTE outside the file");
            uint64_t off = ph.p_offset, end = ph.p_offset + ph.p_filesz;
            while (off + 12 <= end) {
                uint32_t namesz, descsz, type;
                memcpy(&namesz, file + off, 4);
                memcpy(&descsz, file + off + 4, 4);
                memcpy(&type, file + off + 8, 4);
                if (namesz > 256 || descsz > 4096)
                    break;   /* not a note this loader recognises; sizes bounded before any arithmetic */
                uint64_t name_at = off + 12;
                uint64_t desc_at = name_at + (((uint64_t)namesz + 3u) & ~3ull);
                uint64_t next = desc_at + (((uint64_t)descsz + 3u) & ~3ull);
                if (next > end || next <= off)
                    break;
                if (namesz == 8 && type == 1 && memcmp(file + name_at, "CosmoOS", 8) == 0)
                    info->cosmo_note = true;
                off = next;
            }
            continue;
        }
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
        if (ph.p_align > ELF_PAGE && (ph.p_align & (ph.p_align - 1)) == 0) {
            uint64_t a = ph.p_align < ELF_ALIGN_MAX ? ph.p_align : ELF_ALIGN_MAX;
            if (a > info->max_align)
                info->max_align = a;
        }

        uint64_t seg_lo = ph.p_vaddr & ~(ELF_PAGE - 1);
        uint64_t seg_hi = (ph.p_vaddr + ph.p_memsz + ELF_PAGE - 1) & ~(ELF_PAGE - 1);
        if (seg_lo < lo_bound || seg_hi > hi_bound || seg_hi < seg_lo)
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
        seg->file_memsz = ph.p_memsz;
        seg->file_vaddr = ph.p_vaddr;
        seg->flags = ph.p_flags & (ELF_PF_R | ELF_PF_W | ELF_PF_X);

        if (seg_lo < lo)
            lo = seg_lo;
        if (seg_hi > hi)
            hi = seg_hi;
    }

    if (info->nr_segments == 0)
        return fail(why, "no PT_LOAD segments");
    if (info->max_align < ELF_PAGE)
        info->max_align = ELF_PAGE;

    bool entry_ok = false;
    for (unsigned j = 0; j < info->nr_segments; j++) {
        const struct elf_segment *s = &info->segments[j];
        if ((s->flags & ELF_PF_X) && eh.e_entry >= s->vaddr && eh.e_entry < s->vaddr + s->memsz)
            entry_ok = true;
    }
    if (!entry_ok)
        return fail(why, "entry point is not inside an executable segment");

    /* Where the program header table lands in memory, for AT_PHDR. */
    for (unsigned j = 0; j < info->nr_segments; j++) {
        const struct elf_segment *s = &info->segments[j];
        uint64_t table = (uint64_t)eh.e_phnum * sizeof(struct elf64_phdr);
        if (eh.e_phoff >= s->offset && eh.e_phoff + table <= s->offset + s->filesz)
            info->phdr_vaddr = s->file_vaddr + (eh.e_phoff - s->offset);
    }

    info->entry = eh.e_entry;
    info->lo = lo;
    info->hi = hi;
    return 0;
}

void elf_rebase(struct elf_info *info, uint64_t base)
{
    for (unsigned i = 0; i < info->nr_segments; i++) {
        info->segments[i].vaddr += base;
        info->segments[i].file_vaddr += base;
    }
    info->entry += base;
    info->lo += base;
    info->hi += base;
    if (info->phdr_vaddr)
        info->phdr_vaddr += base;
}
