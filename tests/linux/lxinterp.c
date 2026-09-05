/*
 * lxinterp.c - a program interpreter for lxdyn (docs/compat/linux/testing.md).
 *
 * The kernel loads this ET_DYN image at USER_INTERP_BASE (or above) and
 * starts the process here with the original stack. It checks the
 * auxiliary vector the kernel built (AT_BASE is its own load address,
 * AT_PHDR/AT_PHNUM/AT_ENTRY describe the executable, AT_EXECFN names
 * the path, AT_PLATFORM the machine), applies the executable's
 * R_X86_64_RELATIVE relocations from PT_DYNAMIC (the one kind a
 * position-independent freestanding program needs), prints a line and
 * jumps to AT_ENTRY with the stack untouched, so lxdyn sees argc/argv.
 * It is itself relocation-free: no absolute pointers in its data.
 */

#define LXABI_NO_START
#include "lxabi.h"

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define DT_RELA 7
#define DT_RELASZ 8
#if defined(__x86_64__)
#define R_RELATIVE 8      /* R_X86_64_RELATIVE */
#else
#define R_RELATIVE 1027   /* R_AARCH64_RELATIVE */
#endif

struct phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct dyn { int64_t tag; uint64_t val; };
struct rela { uint64_t off, info; int64_t addend; };

extern char __ehdr_start[] __attribute__((visibility("hidden")));   /* the ELF header: this image's load address; hidden: a PC-relative reference, no GOT slot to relocate */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void fail(const char *what)
{
    lx_puts("lxinterp: FAIL ");
    lx_puts(what);
    lx_puts("\n");
    lx_exit(1);
}

/* Returns the entry to jump to; the caller restores the original rsp. */
unsigned long interp_main(unsigned long *sp)
{
    unsigned long argc = sp[0];
    unsigned long *envp = sp + 1 + argc + 1;
    while (*envp)
        envp++;
    unsigned long *auxv = envp + 1;
    unsigned long phdr = 0, phnum = 0, entry = 0, base = 0, execfn = 0, platform = 0, pagesz = 0, random = 0;
    for (; auxv[0]; auxv += 2) {
        switch (auxv[0]) {
        case LX_AT_PHDR: phdr = auxv[1]; break;
        case LX_AT_PHNUM: phnum = auxv[1]; break;
        case LX_AT_ENTRY: entry = auxv[1]; break;
        case LX_AT_BASE: base = auxv[1]; break;
        case LX_AT_EXECFN: execfn = auxv[1]; break;
        case LX_AT_PLATFORM: platform = auxv[1]; break;
        case LX_AT_PAGESZ: pagesz = auxv[1]; break;
        case LX_AT_RANDOM: random = auxv[1]; break;
        default: break;
        }
    }
    if (base != (unsigned long)__ehdr_start)
        fail("AT_BASE is not the interpreter's load address");
    if (base < 0x7F0000000000UL)
        fail("interpreter below USER_INTERP_BASE");
    if (phdr == 0 || phnum == 0 || entry == 0 || pagesz != 4096 || random == 0)
        fail("auxv incomplete");
    if (execfn == 0 || !streq((const char *)execfn, "/boot/tests/linux/lxdyn"))
        fail("AT_EXECFN");
    if (platform == 0 || !streq((const char *)platform, LX_MACHINE))
        fail("AT_PLATFORM");

    /* The executable's bias: AT_PHDR is its table's runtime address; the
     * PT_PHDR entry (or the first PT_LOAD, at offset 0) gives the link-time one. */
    const struct phdr *ph = (const struct phdr *)phdr;
    unsigned long bias = 0;
    int have_bias = 0;
    const struct phdr *dynamic = 0;
    for (unsigned long i = 0; i < phnum; i++) {
        if (ph[i].type == PT_PHDR) {
            bias = phdr - ph[i].vaddr;
            have_bias = 1;
        }
        if (ph[i].type == PT_DYNAMIC)
            dynamic = &ph[i];
    }
    if (!have_bias) {
        for (unsigned long i = 0; i < phnum; i++)
            if (ph[i].type == PT_LOAD && ph[i].off == 0) {
                bias = (phdr - 64) - ph[i].vaddr;
                have_bias = 1;
                break;
            }
    }
    if (!have_bias)
        fail("cannot find the executable's bias");
    if (bias != 0x555500000000UL)
        fail("executable not at USER_PIE_BASE");
    if (entry < bias)
        fail("AT_ENTRY below the executable");

    /* RELATIVE relocations: *(bias + off) = bias + addend. */
    unsigned long nrel = 0;
    if (!dynamic)
        fail("lxdyn has no PT_DYNAMIC");
    {
        const struct dyn *d = (const struct dyn *)(bias + dynamic->vaddr);
        unsigned long rela = 0, relasz = 0;
        for (; d->tag; d++) {
            if (d->tag == DT_RELA)
                rela = bias + d->val;
            if (d->tag == DT_RELASZ)
                relasz = d->val;
        }
        const struct rela *r = (const struct rela *)rela;
        for (unsigned long i = 0; rela && i < relasz / sizeof(*r); i++) {
            if ((uint32_t)r[i].info != R_RELATIVE)
                fail("unsupported relocation type");
            *(unsigned long *)(bias + r[i].off) = bias + (unsigned long)r[i].addend;
            nrel++;
        }
    }
    if (nrel == 0)
        fail("lxdyn carries no RELATIVE relocation to apply");
    lx_puts("lxinterp: ok\n");
    return entry;
}

#if defined(__x86_64__)
__asm__(".section .text\n"
        ".globl _start\n"
        "_start:\n"
        "    movq %rsp, %rbx\n"          /* the original stack, for the executable */
        "    movq %rsp, %rdi\n"
        "    andq $-16, %rsp\n"
        "    call interp_main\n"
        "    movq %rbx, %rsp\n"
        "    xorl %edx, %edx\n"          /* no fini function for the executable */
        "    jmp *%rax\n"
        ".section .note.GNU-stack,\"\",@progbits\n");
#else
__asm__(".section .text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x19, sp\n"               /* the original stack, for the executable */
        "    mov x0, sp\n"
        "    bl interp_main\n"
        "    mov sp, x19\n"
        "    mov x16, x0\n"
        "    mov x0, #0\n"                /* no fini function for the executable */
        "    br x16\n"
        ".section .note.GNU-stack,\"\",@progbits\n");
#endif
