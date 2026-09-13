/*
 * tlsscan.h - finding a program's thread-local template in its own
 * program headers (docs/audit/next-subsystem-pt-tls.md).
 *
 * Kept apart from tcb.c, and free of syscalls and globals, for one
 * reason: it reads a structure the *program image* controls, so every
 * refusal in it needs a test, and a pure function of (table, count) can
 * be handed a deliberately malformed table on the host. The alternative
 * was a crafted binary in the boot archive, which would have proved one
 * case instead of eight.
 */

#ifndef LIBC_TLSSCAN_H
#define LIBC_TLSSCAN_H

#include <stddef.h>
#include <stdint.h>

struct elf_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

struct tls_template {
    const char *src;   /* the initialised bytes, in the image */
    size_t filesz;     /* how many of them */
    size_t memsz;      /* total; the rest is zero */
    size_t align;      /* what the ABI demands of the image's address */
    int found;
};

/*
 * Fill `*out` from the one `PT_TLS` in `ph[0..n)`. Returns 0 when there is
 * no template (`out->found == 0`) or a sound one, and -1 when the table
 * says something this library will not act on -- which the caller turns
 * into a refusal to start, because a process whose thread-local storage is
 * wrong cannot be allowed to run `main`.
 *
 * `phent` is the size the kernel reported for one entry: anything but
 * `sizeof(struct elf_phdr)` means this code and that table disagree about
 * the format, and walking it would be reading the wrong fields.
 */
int tls_scan(const struct elf_phdr *ph, unsigned long n, unsigned long phent, struct tls_template *out);

#endif /* LIBC_TLSSCAN_H */
