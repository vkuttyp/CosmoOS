/*
 * tlsscan.c - the validation, as a pure function. See tlsscan.h for why
 * it is a file of its own.
 */

#include "tlsscan.h"

#define PT_LOAD_ 1u
#define PT_TLS_  7u

/* A power of two, and no larger than a page: an alignment this library
 * cannot honour must be refused rather than rounded away. */
static int align_ok(uint64_t a)
{
    return a != 0 && (a & (a - 1)) == 0 && a <= 4096u;
}

int tls_scan(const struct elf_phdr *ph, unsigned long n, unsigned long phent, struct tls_template *out)
{
    out->src = 0;
    out->filesz = out->memsz = out->align = 0;
    out->found = 0;
    /*
     * No headers is a **refusal**, not "no TLS". The kernel publishes
     * `AT_PHDR` for every native binary because the linker script puts the
     * header table in the text segment; a binary whose headers are not
     * mapped, or a kernel that did not pass the tags, leaves this library
     * unable to see whether the program has a template at all -- and the
     * silent answer would be a program running with thread-local storage
     * that was never initialised, which is the corruption this unit exists
     * to prevent. Refusing is the safe half of an unknowable question.
     */
    if (ph == 0 || n == 0)
        return -1;
    if (phent != sizeof(struct elf_phdr) || n > 64u)
        return -1;

    const struct elf_phdr *tls = 0;
    for (unsigned long i = 0; i < n; i++) {
        if (ph[i].p_type != PT_TLS_)
            continue;
        if (tls != 0)
            return -1;   /* the ABI allows one; two is a broken image */
        tls = &ph[i];
    }
    /*
     * No segment, or an empty one, is no template -- and empty is the
     * common case: `ld.lld` emits a `PT_TLS` for every program linked
     * against a script that declares one, with `memsz` 0 and `p_align` 0.
     * Rejecting that alignment as not-a-power-of-two killed every program
     * in the system at startup, which is how this case was found.
     */
    if (tls == 0 || tls->p_memsz == 0)
        return 0;
    if (tls->p_memsz < tls->p_filesz || tls->p_memsz > (1u << 20))
        return -1;
    /*
     * The template's range must not wrap. A crafted `p_vaddr` near the top
     * of the address space plus a `p_filesz` that carries makes an
     * out-of-range template look contained in a low `PT_LOAD` below --
     * and the address that survives that check is handed straight to
     * `memcpy`. The loader checks this for `PT_LOAD` and leaves `PT_TLS`
     * alone, so nothing else has.
     */
    if (tls->p_vaddr + tls->p_filesz < tls->p_vaddr)
        return -1;
    if (!align_ok(tls->p_align))
        return -1;
    /*
     * The initialised bytes must lie inside a mapped `PT_LOAD`: this
     * becomes a pointer this library copies from, and nothing else has
     * checked it.
     */
    if (tls->p_filesz != 0) {
        int inside = 0;
        /*
         * The segment's own range is *not* checked for wrapping, and that
         * is a conclusion rather than an omission. A wrapped `seg_end` is
         * smaller than `seg_start`, so `tls_end <= seg_end` demands a
         * small `tls_end`, while `tls_start >= seg_start` demands a huge
         * `tls_start` -- and the template's range cannot wrap between the
         * two, because the check above refused that. No pair of values
         * satisfies both. A guard here was written and then removed when
         * the case meant to fail without it passed anyway: an untriggerable
         * guard with a test beside it claims a coverage it does not have.
         */
        for (unsigned long i = 0; i < n && !inside; i++) {
            if (ph[i].p_type != PT_LOAD_)
                continue;
            if (tls->p_vaddr >= ph[i].p_vaddr &&
                tls->p_vaddr + tls->p_filesz <= ph[i].p_vaddr + ph[i].p_filesz)
                inside = 1;
        }
        if (!inside)
            return -1;
    }
    out->src = (const char *)(uintptr_t)tls->p_vaddr;
    out->filesz = (size_t)tls->p_filesz;
    out->memsz = (size_t)tls->p_memsz;
    out->align = (size_t)tls->p_align;
    out->found = 1;
    return 0;
}
