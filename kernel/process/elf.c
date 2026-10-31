/*
 * elf.c - Static ELF64 executables for user processes.
 *
 * The loader. The validator, which the host tests and fuzz_elf feed
 * crafted images, is CeeSafe: elfparse.cee (roadmap 2.0 SA5).
 */

#include <kernel/elf.h>
#include <kernel/elf64.h>
#include <kernel/errno.h>
#include <kernel/string.h>

#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/vmm.h>

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

/*
 * Can this segment be shared from the file rather than copied?
 *
 * Three conditions, and each is a rule rather than a preference:
 *
 *  - **Not writable.** A writable segment must be private, or one
 *    process's store reaches the file and every other process running
 *    the program.
 *  - **No zero tail** (`file_memsz == filesz`). The bytes a segment
 *    needs beyond what the file holds are zeroes, and a shared mapping
 *    has nowhere to put them: writing them would dirty the page cache
 *    and change the file for everyone. A private mapping has somewhere
 *    -- its own copy -- which is why the writable case below may have a
 *    tail.
 *
 * Note which size that compares. `memsz` is the *page rounded* span and
 * is almost never equal to `filesz`; comparing against it rejects every
 * real text segment, which is what the first version of this did. The
 * padding between `filesz` and the end of its last page is not a zero
 * tail: it is the file's next bytes, and mapping them is what every
 * other system does too.
 *
 * `elf_validate` has already required that `vaddr` and `offset` are
 * congruent modulo the page size, which is what makes any of this a
 * mapping rather than a rearrangement.
 */
static bool seg_shareable(const struct elf_segment *s)
{
    return (s->flags & ELF_PF_W) == 0 && s->file_memsz == s->filesz;
}

int elf_load_into(struct vm_space *space, const void *image, const struct elf_info *info,
                  struct vnode *vn)
{
    const uint8_t *file = image;

    for (unsigned i = 0; i < info->nr_segments; i++) {
        const struct elf_segment *s = &info->segments[i];

        /*
         * The file's own pages, shared, when this segment qualifies:
         * one set of frames for every process running the program
         * (docs/audit/next-subsystem-elf-shared-text.md). `maxprot`
         * excludes W, which is what stops a later mprotect from turning
         * shared text writable -- the whole safety argument for sharing
         * it. The segment's own file offset is page-aligned by the
         * congruence elf_validate enforced.
         */
        if (vn != NULL && seg_shareable(s)) {
            uint64_t file_off = s->offset - (s->file_vaddr - s->vaddr);
            /* The pages the file's bytes touch, rounded up. Past the
             * end of the file the page cache gives zeroes, which is
             * what the padding of a last partial page should read as. */
            size_t span = (size_t)(((s->file_vaddr - s->vaddr) + s->filesz + PAGE_SIZE - 1) &
                                   ~(uint64_t)(PAGE_SIZE - 1));
            int rc = vm_user_map_file(space, s->vaddr, span, seg_prot(s->flags),
                                      seg_prot(s->flags), VM_MAP_SHARED | VM_MAP_TEXT, vn, file_off,
                                      "elf-text");
            if (rc == 0)
                continue;
            /* Fall through to the copy: a file whose pages cannot be
             * mapped here still has to run. */
        }

        /*
         * The copy path, for a segment that cannot come from the file:
         * writable, or with a zero tail, or from an image that has no
         * file at all.
         *
         * It is split in two, and the split is where the pages this
         * segment's *file bytes* touch end. The first part is populated
         * and copied into, as it always was. The second is the segment's
         * zero tail -- `.bss`, usually almost all of it -- and it is
         * left demand-paged: an anonymous page arrives zero, which is
         * exactly what a zero tail needs, so populating it eagerly buys
         * nothing and costs a frame per page whether the program touches
         * it or not. For the binary this unit was measured on the tail
         * is 26 of the segment's 27 pages
         * (docs/audit/next-subsystem-elf-shared-text.md).
         */
        uint64_t file_end = s->filesz ? ((s->file_vaddr + s->filesz + PAGE_SIZE - 1) &
                                         ~(uint64_t)(PAGE_SIZE - 1))
                                      : s->vaddr;
        if (file_end > s->vaddr + s->memsz)
            file_end = s->vaddr + s->memsz;
        size_t copied_span = (size_t)(file_end - s->vaddr);

        /* Map writable while populating, then set the final protection:
         * the copy goes through the direct map, but a read-only region
         * would still be recorded read-only and query would disagree. */
        int rc;
        if (copied_span > 0) {
            rc = vm_user_map_anon(space, s->vaddr, copied_span, VM_PROT_RW, VM_REGION_POPULATED,
                                  "elf-segment");
            if (rc)
                return rc;
        }
        if (file_end < s->vaddr + s->memsz) {
            rc = vm_user_map_anon(space, file_end, (size_t)(s->vaddr + s->memsz - file_end),
                                  seg_prot(s->flags), 0, "elf-bss");
            if (rc)
                return rc;
        }

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

        if (copied_span > 0) {
            rc = vm_user_protect(space, s->vaddr, copied_span, seg_prot(s->flags));
            if (rc)
                return rc;
        }
    }
    return 0;
}

