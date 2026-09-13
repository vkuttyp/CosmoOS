/*
 * tcb.c - the thread pointer, and errno behind it.
 *
 * One accessor with no cases (cosmo/tcb.h says why there cannot be one):
 * read the thread pointer, add an offset. The two architectures differ only
 * in how the pointer is read.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <cosmo/auxv.h>
#include <cosmo/syscall.h>
#include <cosmo/tcb.h>

#include "libc.h"

/*
 * The program's own thread-local template, read from its own program
 * headers. The kernel knows nothing about thread-local storage: it passes
 * the header table's address in the auxiliary vector, and everything below
 * is this library reading its own ELF
 * (docs/audit/next-subsystem-pt-tls.md).
 */
struct elf_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
#define PT_LOAD_ 1u
#define PT_TLS_  7u

static struct {
    const char *src;     /* the initialised bytes, in the image */
    size_t filesz;       /* how many of them */
    size_t memsz;        /* total, the rest zero */
    size_t align;        /* what the ABI demands of the image's address */
    int found;
} g_tls;

/* A power of two, and no larger than a page: an alignment this library
 * cannot honour must be refused rather than rounded away. */
static int align_ok(size_t a)
{
    return a != 0 && (a & (a - 1)) == 0 && a <= 4096u;
}

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/*
 * Find the one `PT_TLS` and check everything about it that a program image
 * could get wrong, because a program image is what this reads. Returns 0
 * when there is no template (the common case) or when one was found and is
 * sound; -1 when the headers say something this library will not act on.
 */
static int tls_find(void)
{
    unsigned long at_phdr = cosmo_getauxval(COSMO_AT_PHDR);
    unsigned long at_phent = cosmo_getauxval(COSMO_AT_PHENT);
    unsigned long at_phnum = cosmo_getauxval(COSMO_AT_PHNUM);
    if (at_phdr == 0 || at_phnum == 0)
        return 0;   /* no headers to read: a program without them has no TLS */
    if (at_phent != sizeof(struct elf_phdr) || at_phnum > 64u)
        return -1;
    const struct elf_phdr *ph = (const struct elf_phdr *)at_phdr;
    const struct elf_phdr *tls = NULL;
    for (unsigned long i = 0; i < at_phnum; i++) {
        if (ph[i].p_type != PT_TLS_)
            continue;
        if (tls != NULL)
            return -1;   /* the ABI allows one; two is a broken image */
        tls = &ph[i];
    }
    /*
     * No segment, or an empty one, is no template -- and the empty case is
     * the common one: `ld.lld` emits a `PT_TLS` for every program linked
     * with a script that declares one, with `memsz` 0 and `p_align` 0.
     * Rejecting that alignment as not-a-power-of-two killed every program
     * in the system at startup, which is how this case was found. Nothing
     * to place, nothing to validate.
     */
    if (tls == NULL || tls->p_memsz == 0)
        return 0;
    if (tls->p_memsz < tls->p_filesz || tls->p_memsz > (1u << 20))
        return -1;
    if (!align_ok((size_t)tls->p_align))
        return -1;
    /* The initialised bytes must be inside a mapped PT_LOAD: this is a
     * pointer into the image, and nothing else has checked it. */
    if (tls->p_filesz != 0) {
        int inside = 0;
        for (unsigned long i = 0; i < at_phnum && !inside; i++) {
            if (ph[i].p_type != PT_LOAD_)
                continue;
            if (tls->p_vaddr >= ph[i].p_vaddr &&
                tls->p_vaddr + tls->p_filesz <= ph[i].p_vaddr + ph[i].p_filesz)
                inside = 1;
        }
        if (!inside)
            return -1;
    }
    g_tls.src = (const char *)(uintptr_t)tls->p_vaddr;
    g_tls.filesz = (size_t)tls->p_filesz;
    g_tls.memsz = (size_t)tls->p_memsz;
    g_tls.align = (size_t)tls->p_align;
    g_tls.found = 1;
    return 0;
}

/*
 * How much storage one thread needs, and where the thread pointer goes
 * inside it. The two architectures differ because their ELF TLS ABIs do:
 *
 *   x86-64  [ image ][ block ]          TP = &block, variables below TP
 *   AArch64 [ block ][ head ][ image ]  TP = &head,  variables above TP
 *
 * The thread pointer is aligned to the image's alignment as well as to 16,
 * because on x86-64 the image's base is `TP - round_up(memsz, align)` and
 * on AArch64 it is `round_up(TP + 16, align)` -- in both cases an
 * unaligned thread pointer would put a `_Alignas(64) __thread` variable
 * somewhere its own relocation did not mean.
 */
size_t cosmo_tcb_storage(void)
{
    return __cosmo_tcb_storage();
}

size_t __cosmo_tcb_storage(void)
{
    if (!g_tls.found)
        return COSMO_TCB_STORAGE;
    size_t a = g_tls.align > 16u ? g_tls.align : 16u;
    return COSMO_TCB_STORAGE + round_up(g_tls.memsz, a) + a;
}

/*
 * Lay a thread's storage out and return the thread pointer, or NULL if
 * `len` is too small. Copies the initialised bytes and zeroes the rest --
 * per thread, which is the whole point: a template is not an allocation.
 */
void *__cosmo_tcb_place(void *storage, size_t len)
{
    if (storage == NULL || len < __cosmo_tcb_storage())
        return NULL;
    char *lo = (char *)storage;
    if (!g_tls.found) {
        /* No template: the thread pointer sits where the block puts it. */
        return lo + COSMO_TCB_TP_OFFSET;
    }
    size_t a = g_tls.align > 16u ? g_tls.align : 16u;
    char *tp;
#if defined(__aarch64__)
    /* [ block ][ head ][ image ] -- the block is below the thread pointer,
     * so the storage starts with it and `tp` is aligned upward from there. */
    tp = (char *)round_up((size_t)(lo + COSMO_TCB_TP_OFFSET), a);
    char *image = (char *)round_up((size_t)(tp + COSMO_TCB_ABI_HEAD), g_tls.align);
    /* The block must still be reachable at a fixed offset below `tp`. */
    char *blk = tp - COSMO_TCB_TP_OFFSET;
    if (blk < lo || image + g_tls.memsz > lo + len)
        return NULL;
#else
    /* [ image ][ block ] -- the block is the thread pointer, and the image
     * is immediately below it. */
    size_t span = round_up(g_tls.memsz, a);
    tp = (char *)round_up((size_t)lo + span, a);
    char *image = tp - span;
    if (image < lo || tp + COSMO_TCB_SIZE > lo + len)
        return NULL;
#endif
    memcpy(image, g_tls.src, g_tls.filesz);
    memset(image + g_tls.filesz, 0, g_tls.memsz - g_tls.filesz);
    return tp;
}

/*
 * The first thread's block. Static, not mapped, on purpose: a mapping can
 * fail, and a startup path that has to handle failing to give the process
 * an `errno` has no good answer -- it cannot report the failure through the
 * thing it just failed to provide. A .bss object removes the question.
 */
/*
 * The first thread's storage: the block, and on AArch64 the ABI's 16-byte
 * head above it. Declared as bytes rather than as the struct, because the
 * thread pointer is not the struct's address on every architecture.
 */
static __attribute__((aligned(16))) char main_storage[COSMO_TCB_STORAGE];

/*
 * x86-64 cannot read the FS base without `rdfsbase` (CR4.FSGSBASE, not
 * guaranteed), so the block's first word points at itself and this loads
 * it through the segment. AArch64 reads TPIDR_EL0 directly.
 */
static inline struct __cosmo_tcb *tcb_self(void)
{
#if defined(__x86_64__)
    /* The thread pointer *is* the block: variant II wants a self-pointer
     * at %fs:0, and TLS variables live below it. */
    struct __cosmo_tcb *t;
    __asm__("movq %%fs:0, %0" : "=r"(t));
    return t;
#elif defined(__aarch64__)
    /* The block is below the thread pointer: variant I reserves 16 bytes
     * at TP and puts TLS variables above them, so the block cannot be
     * there (cosmo/tcb.h). One register read and one constant offset --
     * the same cost as before, and still no branch. */
    return (struct __cosmo_tcb *)((char *)__builtin_thread_pointer() - COSMO_TCB_TP_OFFSET);
#else
#error "no thread-pointer read for this architecture"
#endif
}

int *__errno_location(void)
{
    return &tcb_self()->err;
}

/*
 * The calling thread's id, cached in its block. Zero means "not asked
 * yet", which is unambiguous because no thread's id is ever zero: a
 * process's first thread answers its pid and every other answers
 * 0x10000 + tid.
 *
 * Lazy rather than filled at install time, for two reasons that both came
 * out of review. A tid read during `__libc_start` would make the thread
 * pointer's installation two syscalls rather than one, and the second
 * would not be in the filter's always-allowed set -- so a child of a
 * filtered process died in startup on `SYS_thread_self` instead of on
 * whatever it was confined for. And a thread that never asks for its id
 * should not pay a syscall to be told it.
 */
unsigned __cosmo_tcb_tid(void)
{
    struct __cosmo_tcb *t = tcb_self();
    if (t->tid == 0)
        t->tid = (unsigned)cosmo_thread_self();
    return t->tid;
}

/*
 * Point the calling thread at `blk`, whose `self` word this writes because
 * x86-64's accessor depends on it. The tid is cached here rather than on
 * first use: a thread that has a block has always had one, so there is no
 * "not yet cached" value to distinguish from a real tid.
 */
/*
 * Take `storage` as this thread's: the block lives at its start and the
 * thread pointer at `COSMO_TCB_TP_OFFSET` into it, which is the block
 * itself on x86-64 and the ABI head above it on AArch64.
 */
static int tcb_use(void *storage, size_t len)
{
    if (len < __cosmo_tcb_storage())
        return -E2BIG;   /* the caller's storage is too small for this program */
    char *tp = (char *)__cosmo_tcb_place(storage, len);
    if (tp == NULL)
        return -EFAULT;  /* it did not fit where it had to go */
    struct __cosmo_tcb *blk = (struct __cosmo_tcb *)(tp - COSMO_TCB_TP_OFFSET);
    blk->self = blk;
    blk->err = 0;
    blk->tid = 0;   /* asked for on first use; see __cosmo_tcb_tid */
    return (int)cosmo_set_tls((unsigned long)tp);
}

/*
 * Called by __libc_start before anything else, including __stdio_init and
 * anything that could set errno. Returns 0, or what SYS_set_tls refused --
 * which for a linked static object it cannot, since the address is aligned
 * and inside the program's own image; the caller has the policy for the
 * case that cannot happen.
 */
/*
 * The first thread's storage. Static when the program has no template,
 * which is every program until one uses `__thread`: nothing to size, and
 * nothing that can fail. With a template the size depends on the program,
 * so it is mapped -- which brings back the startup failure the errno unit
 * removed on purpose, and gets the same answer from its caller: one line
 * to file descriptor 2 and exit 127, because a process whose thread-local
 * storage is wrong cannot be allowed to run `main`.
 */
int __cosmo_tcb_init(void)
{
    if (tls_find() != 0)
        return -ENOEXEC;   /* the program's own headers say something this library will not act on */
    size_t need = __cosmo_tcb_storage();
    if (need <= sizeof(main_storage))
        return tcb_use(main_storage, sizeof(main_storage));
    /*
     * The raw syscall, not libc's `mmap`: that wrapper writes `errno` on
     * failure, and `errno` is what this function exists to make reachable.
     * A failure here must return, not fault.
     */
    /* A whole number of pages: the kernel refuses anything else, which
     * every other caller in this library satisfies by accident because it
     * is already mapping pages. This one is sized by the program. */
    size_t span = (need + 4095u) & ~(size_t)4095u;
    long p = cosmo_mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE);
    if (p < 0)
        return (int)p;
    return tcb_use((void *)(unsigned long)p, span);
}

int cosmo_tcb_install(void *block, size_t len)
{
    if (block == NULL || len < __cosmo_tcb_storage())
        return -EINVAL;
    if ((unsigned long)block % 16u)
        return -EINVAL;
    return tcb_use(block, len);
}
