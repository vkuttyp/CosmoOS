/*
 * dynhello.c - a dynamically linked musl program (roadmap 1.1 P4;
 * docs/compat/linux/testing.md, "Shared libraries"): PT_INTERP is
 * /lib/ld-musl-ARCH.so.1, which the kernel maps and starts. Checked: the
 * auxiliary vector against what the program can see for itself (AT_BASE
 * is libc.so's base, which is the dynamic linker; AT_PHDR and AT_PHNUM the
 * executable's table; AT_ENTRY its _start; AT_EXECFN the path; AT_HWCAP),
 * the layout (the executable in the PIE range, libc.so in the mmap range),
 * RELRO read-only after startup, W^X (no writable and executable page),
 * and thread-local storage in two threads.
 *
 *   dynhello            the checks, then "dynhello: ok"
 *   dynhello addr       prints "dynhello: libc PTR exe PTR" (ASLR: two
 *                       runs must differ; dltest compares them)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define USER_PIE_BASE  0x555500000000UL
#define USER_MMAP_BASE 0x100000000000UL

extern char _start[];

static int g_fails;

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            printf("dynhello: FAIL %s (line %d, errno %d)\n", #c, __LINE__, errno); \
            g_fails++;                                                             \
        }                                                                          \
    } while (0)

struct phdrs {
    int n;
    uintptr_t base[8];
    const ElfW(Phdr) *phdr[8];
    int phnum[8];
    char name[8][64];
};

static int collect(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size;
    struct phdrs *p = arg;
    if (p->n < 8) {
        p->base[p->n] = info->dlpi_addr;
        p->phdr[p->n] = info->dlpi_phdr;
        p->phnum[p->n] = info->dlpi_phnum;
        snprintf(p->name[p->n], sizeof(p->name[0]), "%s", info->dlpi_name ? info->dlpi_name : "");
        p->n++;
    }
    return 0;
}

/* A write to `addr` in a child: 0 when it died of SIGSEGV, as a write to
 * a read-only page must. */
static int write_faults(volatile char *addr)
{
    pid_t c = fork();
    if (c == 0) {
        *addr = *addr;
        _exit(0);
    }
    int st;
    if (c < 0 || waitpid(c, &st, 0) != c)
        return -1;
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV ? 0 : 1;
}

/* The first page the dynamic linker makes read-only for the object's
 * PT_GNU_RELRO -- musl protects [start, end) rounded down to pages -- or
 * NULL when that range is empty. */
static volatile char *relro_page(uintptr_t base, const ElfW(Phdr) *ph, int n)
{
    for (int i = 0; i < n; i++) {
        if (ph[i].p_type != PT_GNU_RELRO)
            continue;
        uintptr_t start = (base + ph[i].p_vaddr) & ~(uintptr_t)4095;
        uintptr_t end = (base + ph[i].p_vaddr + ph[i].p_memsz) & ~(uintptr_t)4095;
        return end > start ? (volatile char *)start : NULL;
    }
    return NULL;
}

/* Relocated read-only data over more than two pages (.data.rel.ro):
 * the executable's PT_GNU_RELRO then covers a whole page however the
 * linker places it, so the RELRO check below always has a page to test. */
#define RO_N 1024
static const char *const ro_table[RO_N] = { [0] = "first", [RO_N - 1] = "last" };

static __thread int tls_value = 5;

static void *thread_main(void *arg)
{
    (void)arg;
    tls_value += 10;
    return (void *)(intptr_t)tls_value;
}

static uint64_t expect_hwcap(void)
{
#if defined(__x86_64__)
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return d;
#else
    return 3;   /* HWCAP_FP | HWCAP_ASIMD */
#endif
}

int main(int argc, char **argv)
{
    uintptr_t at_base = getauxval(AT_BASE);
    struct phdrs p = { 0 };
    dl_iterate_phdr(collect, &p);
    if (argc == 2 && strcmp(argv[1], "addr") == 0) {
        printf("dynhello: libc %#lx exe %#lx\n", (unsigned long)at_base, (unsigned long)p.base[0]);
        return 0;
    }

    /* The vector. */
    CHECK(getauxval(AT_PAGESZ) == 4096);
    CHECK(at_base != 0);
    Dl_info di;
    CHECK(dladdr((void *)printf, &di) != 0 && (uintptr_t)di.dli_fbase == at_base);   /* libc.so is the linker */
    CHECK(getauxval(AT_ENTRY) == (uintptr_t)_start);
    CHECK(p.n >= 2 && (uintptr_t)p.phdr[0] == getauxval(AT_PHDR) && (unsigned long)p.phnum[0] == getauxval(AT_PHNUM));
    CHECK(getauxval(AT_PHENT) == sizeof(ElfW(Phdr)));
    CHECK(getauxval(AT_RANDOM) != 0);
    const char *fn = (const char *)getauxval(AT_EXECFN);
    CHECK(fn != NULL && strcmp(fn, argv[0]) == 0);
    errno = 0;
    CHECK(getauxval(AT_SECURE) == 0 && errno == 0);
#if defined(__x86_64__)
    CHECK(getauxval(AT_HWCAP) == expect_hwcap());
#else
    CHECK((getauxval(AT_HWCAP) & expect_hwcap()) == expect_hwcap());
#endif
    errno = 0;
    (void)getauxval(AT_HWCAP2);
    CHECK(errno == 0);   /* present (getauxval says ENOENT for an absent entry) */

    /* The layout: a PIE in the PIE range, the linker in the mmap range. */
    CHECK(p.base[0] >= USER_PIE_BASE && p.base[0] - USER_PIE_BASE < (1UL << 40) && (p.base[0] & 4095) == 0);
    CHECK(at_base >= USER_MMAP_BASE && (at_base & 4095) == 0);

    /* RELRO: read-only once the linker has relocated. */
    volatile char *r = relro_page(p.base[0], p.phdr[0], p.phnum[0]);
    CHECK(ro_table[0][0] == 'f' && ro_table[RO_N - 1][0] == 'l');
    CHECK(r != NULL && write_faults(r) == 0);
#if defined(__x86_64__)
    /* libc.so's own: musl protects the linker's RELRO only where PAGE_SIZE
     * is a constant. On AArch64 it is libc.page_size, still 0 when the
     * linker records its own range (ldso/dynlink.c, __dls2b before
     * __dls3), so the range is empty there -- musl's, on Linux too. */
    for (int i = 1; i < p.n; i++) {
        if ((uintptr_t)p.base[i] == at_base) {
            r = relro_page(p.base[i], p.phdr[i], p.phnum[i]);
            CHECK(r != NULL && write_faults(r) == 0);
        }
    }
#endif

    /* W^X: no page writable and executable, by mmap or by mprotect. */
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
          errno == EINVAL);
    errno = 0;
    CHECK(mprotect((void *)((uintptr_t)main & ~(uintptr_t)4095), 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 &&
          errno == EINVAL);

    /* TLS: the main thread's copy and a second thread's are distinct. */
    pthread_t t;
    void *ret = NULL;
    CHECK(pthread_create(&t, NULL, thread_main, NULL) == 0 && pthread_join(t, &ret) == 0);
    CHECK((intptr_t)ret == 15 && tls_value == 5);

    printf(g_fails ? "dynhello: FAIL (%d)\n" : "dynhello: ok\n", g_fails);
    return g_fails ? 1 : 0;
}
