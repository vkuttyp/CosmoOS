/*
 * elf.c - ELF64 validation and segment loading for the kernel image.
 *
 * Input is an untrusted byte buffer read from the boot volume. Every
 * offset and size is bounds-checked against the buffer before use. The
 * kernel is linked at a fixed higher-half virtual address; the loader
 * places it anywhere in low physical memory and the bootstrap page tables
 * bridge the two, so no relocation processing is needed here.
 */

#include "loader.h"
#include "cosmoboot.h"

#define EI_NIDENT 16
#define ELFMAG    "\177ELF"
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC    2
#define EM_X86_64  62
#define PT_LOAD    1
#define PT_NOTE    4

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

struct elf64_nhdr {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
};

static bool range_in_file(uint64_t off, uint64_t len, size_t size)
{
    return off <= size && len <= size - off;
}

/* Scan a PT_NOTE segment for the cosmoboot version note. */
static void parse_notes(const uint8_t *file, const struct elf64_phdr *ph, struct elf_image *img)
{
    uint64_t pos = ph->p_offset;
    uint64_t end = ph->p_offset + ph->p_filesz;

    while (pos + sizeof(struct elf64_nhdr) <= end) {
        const struct elf64_nhdr *nh = (const struct elf64_nhdr *)(file + pos);
        uint64_t name_off = pos + sizeof(*nh);
        uint64_t desc_off = name_off + ALIGN_UP(nh->n_namesz, 4);
        uint64_t next = desc_off + ALIGN_UP(nh->n_descsz, 4);

        if (next > end)
            break;

        if (nh->n_type == COSMOBOOT_NOTE_TYPE &&
            nh->n_namesz == sizeof(COSMOBOOT_NOTE_NAME) &&
            memcmp(file + name_off, COSMOBOOT_NOTE_NAME, sizeof(COSMOBOOT_NOTE_NAME)) == 0 &&
            nh->n_descsz == sizeof(uint32_t)) {
            uint32_t v;
            memcpy(&v, file + desc_off, sizeof(v));
            img->note_version = v;
            return;
        }
        pos = next;
    }
}

EFI_STATUS elf_load(const uint8_t *file, size_t size, struct elf_image *img, bool *fallback_used)
{
    const struct elf64_ehdr *eh;
    uint64_t lo = UINT64_MAX;
    uint64_t hi = 0;

    memset(img, 0, sizeof(*img));

    if (size < sizeof(*eh))
        return EFI_LOAD_ERROR;
    eh = (const struct elf64_ehdr *)file;

    if (memcmp(eh->e_ident, ELFMAG, 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_ident[6] != EV_CURRENT) {
        lputs("cosmoboot: not an ELF64 little-endian file\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_type != ET_EXEC) {
        lputs("cosmoboot: kernel must be ET_EXEC\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_machine != EM_X86_64) {
        lputs("cosmoboot: kernel is not x86-64\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr) || eh->e_phnum == 0 ||
        !range_in_file(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr), size)) {
        lputs("cosmoboot: bad program header table\n");
        return EFI_LOAD_ERROR;
    }

    /* Pass 1: validate every PT_LOAD and compute the virtual span. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff + (uint64_t)i * sizeof(*ph));

        if (ph->p_type == PT_NOTE) {
            if (range_in_file(ph->p_offset, ph->p_filesz, size))
                parse_notes(file, ph, img);
            continue;
        }
        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz < ph->p_filesz ||
            !range_in_file(ph->p_offset, ph->p_filesz, size) ||
            ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
            lputs("cosmoboot: PT_LOAD out of bounds\n");
            return EFI_LOAD_ERROR;
        }
        if (img->segment_count >= ELF_MAX_SEGMENTS) {
            lputs("cosmoboot: too many PT_LOAD segments\n");
            return EFI_LOAD_ERROR;
        }
        if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X)) {
            lputs("cosmoboot: refusing writable+executable segment (W^X)\n");
            return EFI_LOAD_ERROR;
        }

        uint64_t seg_lo = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
        uint64_t seg_hi = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

        /* Segments with different permissions must not share a page. */
        for (uint32_t j = 0; j < img->segment_count; j++) {
            const struct elf_segment *o = &img->segments[j];
            if (seg_lo < o->vaddr + o->size && o->vaddr < seg_hi) {
                lputs("cosmoboot: PT_LOAD segments overlap at page granularity\n");
                return EFI_LOAD_ERROR;
            }
        }

        struct elf_segment *seg = &img->segments[img->segment_count++];
        seg->vaddr = seg_lo;
        seg->size = seg_hi - seg_lo;
        seg->flags = ph->p_flags & (PF_R | PF_W | PF_X);

        if (seg_lo < lo)
            lo = seg_lo;
        if (seg_hi > hi)
            hi = seg_hi;
    }

    if (img->segment_count == 0) {
        lputs("cosmoboot: no PT_LOAD segments\n");
        return EFI_LOAD_ERROR;
    }
    /* The entry must land inside an executable segment: gaps between
     * segments are unmapped and non-executable segments are NX, so any
     * other entry would fault on the first instruction. */
    bool entry_ok = false;
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const struct elf_segment *seg = &img->segments[i];
        if ((seg->flags & PF_X) && eh->e_entry >= seg->vaddr &&
            eh->e_entry < seg->vaddr + seg->size) {
            entry_ok = true;
            break;
        }
    }
    if (!entry_ok) {
        lputs("cosmoboot: entry point not inside an executable segment\n");
        return EFI_LOAD_ERROR;
    }
    if (img->note_version == 0) {
        lputs("cosmoboot: kernel carries no .note.cosmoboot\n");
        return EFI_LOAD_ERROR;
    }
    if (img->note_version != COSMOBOOT_VERSION) {
        lprintf("cosmoboot: kernel wants boot protocol v%u, loader speaks v%u\n",
                img->note_version, COSMOBOOT_VERSION);
        return EFI_UNSUPPORTED;
    }

    img->virt_base = lo;
    img->virt_end = hi;
    img->entry = eh->e_entry;

    /* One contiguous physical span keeps the phys/virt relation a single
     * offset, which is all the bootstrap page tables need. */
    EFI_PHYSICAL_ADDRESS phys;
    EFI_STATUS st = alloc_pages_low(BYTES_TO_PAGES(hi - lo), EFI_MEMORY_TYPE_COSMO_KERNEL, &phys, fallback_used);
    if (EFI_ERROR(st))
        return st;
    img->phys_base = phys;

    /* Pass 2: copy. alloc_pages_low zeroed the span, so .bss is already
     * clean and inter-segment gaps are zero. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff + (uint64_t)i * sizeof(*ph));
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
            continue;
        memcpy((void *)(uintptr_t)(phys + (ph->p_vaddr - lo)), file + ph->p_offset, ph->p_filesz);
    }

    return EFI_SUCCESS;
}
