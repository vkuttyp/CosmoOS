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
    if (ph == 0 || n == 0)
        return 0;   /* no headers to read: a program without them has no TLS */
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
    if (!align_ok(tls->p_align))
        return -1;
    /*
     * The initialised bytes must lie inside a mapped `PT_LOAD`: this
     * becomes a pointer this library copies from, and nothing else has
     * checked it.
     */
    if (tls->p_filesz != 0) {
        int inside = 0;
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
