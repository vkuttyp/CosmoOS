/*
 * test_libc.c - Host test of the libc's pure parts (docs/libc/testing.md):
 * the formatting engine, the conversions, qsort, the string functions and
 * the allocator over a fake mmap. Compiled with ASan and UBSan against the
 * host's own libc, so every function under test is renamed with a prefix
 * to avoid clashing with the host's.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

/* --- the sources under test, renamed --- */

#define vsnprintf c_vsnprintf
#define snprintf c_snprintf
#define sprintf c_sprintf
#define vdprintf c_vdprintf
#define dprintf c_dprintf
#define vfprintf c_vfprintf
#define fprintf c_fprintf
#define vprintf c_vprintf
#define printf c_printf
#define fwrite c_fwrite
#define write c_write
#define strnlen c_strnlen
#define strlen c_strlen
#define FILE c_FILE
#undef stdout
#define stdout c_stdout
typedef struct c_FILE c_FILE;
static c_FILE *c_stdout;
static size_t c_fwrite(const void *b, size_t s, size_t n, c_FILE *f) { (void)b; (void)f; return s * n; }
static long c_write(int fd, const void *b, size_t n) { (void)fd; (void)b; return (long)n; }
static size_t c_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static size_t c_strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
#include "../../libc/src/printf.c"
#undef printf
#undef fprintf
#undef vfprintf
#undef vprintf
#undef dprintf
#undef vdprintf
#undef sprintf
#undef snprintf
#undef vsnprintf
#undef fwrite
#undef write
#undef strlen
#undef strnlen
#undef FILE
#undef stdout

/* The allocator over a fake mmap of static memory. */
static unsigned char g_arena_mem[1 << 20];
static size_t g_arena_used;
static int g_maps, g_unmaps;
static void *fake_mmap(size_t len)
{
    if (g_arena_used + len > sizeof(g_arena_mem))
        return MAP_FAILED;
    void *p = g_arena_mem + g_arena_used;
    g_arena_used += len;
    g_maps++;
    return p;
}
#define mmap(a, l, p, f, fd, o) fake_mmap(l)
#define munmap(a, l) (g_unmaps++, 0)
#define malloc c_malloc
#define calloc c_calloc
#define realloc c_realloc
#define free c_free
#define abort c_abort
static void c_abort(void) { printf("  allocator abort\n"); g_failures++; }
#include "../../libc/src/malloc.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef abort
#undef mmap
#undef munmap

/* Conversions and qsort. */
#define strtoll c_strtoll
#define strtoull c_strtoull
#define strtol c_strtol
#define strtoul c_strtoul
#define atoi c_atoi
#define atol c_atol
#define qsort c_qsort
#define abs c_abs
#define labs c_labs
#include "../../libc/src/conv.c"
#undef strtoll
#undef strtoull
#undef strtol
#undef strtoul
#undef atoi
#undef atol
#undef qsort
#undef abs
#undef labs

/*
 * The thread-local template's validation, which reads a structure the
 * program *image* controls -- so every refusal in it gets a table no real
 * linker would emit. That is why it lives in a file of its own with no
 * syscalls and no globals (libc/src/tlsscan.h).
 */
#include "../../libc/src/tlsscan.c"

#define PH_LOAD 1u
#define PH_TLS  7u

static struct elf_phdr ph_load(uint64_t vaddr, uint64_t filesz)
{
    struct elf_phdr p = { 0 };
    p.p_type = PH_LOAD;
    p.p_vaddr = vaddr;
    p.p_filesz = filesz;
    p.p_memsz = filesz;
    p.p_align = 4096;
    return p;
}

static struct elf_phdr ph_tls(uint64_t vaddr, uint64_t filesz, uint64_t memsz, uint64_t align)
{
    struct elf_phdr p = { 0 };
    p.p_type = PH_TLS;
    p.p_vaddr = vaddr;
    p.p_filesz = filesz;
    p.p_memsz = memsz;
    p.p_align = align;
    return p;
}

static void test_tls_scan(void)
{
    struct tls_template t;
    struct elf_phdr tab[4];
    const unsigned long ent = sizeof(struct elf_phdr);

    /* No headers at all: no template, and not an error. */
    CHECK(tls_scan(NULL, 0, ent, &t) == 0 && t.found == 0);

    /* The common case: a program with no thread-locals still gets a
     * PT_TLS from the linker, with memsz 0 and align 0. Not an error, and
     * not a template -- rejecting this killed every program once. */
    tab[0] = ph_load(0x400000, 0x1000);
    tab[1] = ph_tls(0, 0, 0, 0);
    CHECK(tls_scan(tab, 2, ent, &t) == 0 && t.found == 0);

    /* A sound one. */
    tab[1] = ph_tls(0x400800, 8, 64, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == 0 && t.found == 1);
    CHECK(t.filesz == 8 && t.memsz == 64 && t.align == 16);
    CHECK(t.src == (const char *)0x400800);

    /* Two segments: the ABI allows one, and a second is a broken image
     * rather than a later one to prefer. No real linker emits this, which
     * is exactly why it is tested here and not with a binary. */
    tab[2] = ph_tls(0x400800, 8, 64, 16);
    CHECK(tls_scan(tab, 3, ent, &t) == -1);

    /* memsz below filesz: more initialised bytes than storage. */
    tab[1] = ph_tls(0x400800, 64, 8, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);

    /* An alignment that is not a power of two, and one too large to
     * honour. Both must be refused rather than rounded away. */
    tab[1] = ph_tls(0x400800, 8, 64, 24);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);
    tab[1] = ph_tls(0x400800, 8, 64, 8192);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);

    /* Initialised bytes outside every PT_LOAD: this would be a pointer
     * libc copies from. */
    tab[1] = ph_tls(0x900000, 8, 64, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);
    /* ...and one that runs off the end of the segment it starts in. */
    tab[1] = ph_tls(0x400ff8, 16, 64, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);

    /* An entry size that is not this structure: the table and this code
     * disagree about the format, so walking it reads the wrong fields. */
    tab[1] = ph_tls(0x400800, 8, 64, 16);
    CHECK(tls_scan(tab, 2, ent + 8, &t) == -1);
    /* An absurd count. */
    CHECK(tls_scan(tab, 65, ent, &t) == -1);

    /* A template with no initialised bytes needs no PT_LOAD to live in:
     * .tbss alone is legitimate. */
    tab[1] = ph_tls(0x900000, 0, 64, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == 0 && t.found == 1 && t.filesz == 0);

    /* An enormous memsz is refused: one thread's copy, times every
     * thread, is not a number a program image gets to choose freely. */
    tab[1] = ph_tls(0x400800, 0, 1u << 21, 16);
    CHECK(tls_scan(tab, 2, ent, &t) == -1);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    char buf[128];
    test_tls_scan();
    /* printf */
    CHECK(c_snprintf(buf, sizeof(buf), "%d %i %u", -5, 7, 42u) == 7 && strcmp(buf, "-5 7 42") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42) == 25 &&
          strcmp(buf, "   42|42   |00042|+42| 42") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%x %X %o %#x %#o", 255, 255, 8, 255, 8) == 17 &&
          strcmp(buf, "ff FF 10 0xff 010") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%s|%.3s|%10s|%-10s|", "abc", "abcdef", "r", "l") == 30 &&
          strcmp(buf, "abc|abc|         r|l         |") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%c%c%%", 'a', 'b') == 3 && strcmp(buf, "ab%") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%ld %lld %zu %hd %hhd", -1L, 1LL << 40, (size_t)99, (short)-3, (char)7) ==
              24 &&
          strcmp(buf, "-1 1099511627776 99 -3 7") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%p", (void *)0x1234) == 6 && strcmp(buf, "0x1234") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%s", (char *)NULL) == 6 && strcmp(buf, "(null)") == 0);
    CHECK(c_snprintf(buf, 4, "%s", "truncated") == 9 && strcmp(buf, "tru") == 0);
    CHECK(c_snprintf(NULL, 0, "%d", 12345) == 5);
    CHECK(c_snprintf(buf, sizeof(buf), "%*d|%-*d|%.*d", 4, 7, 4, 7, 3, 7) == 13 && strcmp(buf, "   7|7   |007") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%.0d|%.0d", 0, 5) == 2 && strcmp(buf, "|5") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%lld", LLONG_MIN) == 20 && strcmp(buf, "-9223372036854775808") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%f", 1.5) == 8 && strcmp(buf, "1.500000") == 0);
    CHECK(c_snprintf(buf, sizeof(buf), "%q") == 2 && strcmp(buf, "%q") == 0);

    /* conversions */
    char *end;
    CHECK(c_strtol("  -123xyz", &end, 10) == -123 && strcmp(end, "xyz") == 0);
    CHECK(c_strtoul("0x1f", NULL, 0) == 31 && c_strtoul("017", NULL, 0) == 15 && c_strtoul("17", NULL, 0) == 17);
    CHECK(c_strtol("zz", &end, 36) == 35 * 36 + 35);
    CHECK(c_strtol("junk", &end, 10) == 0 && strcmp(end, "junk") == 0);
    errno = 0;
    CHECK(c_strtoll("99999999999999999999", NULL, 10) == LLONG_MAX && errno == ERANGE);
    errno = 0;
    CHECK(c_strtoll("-99999999999999999999", NULL, 10) == LLONG_MIN && errno == ERANGE);
    CHECK(c_atoi("+77") == 77 && c_atol("-9") == -9);

    /* qsort */
    int arr[200];
    for (int i = 0; i < 200; i++)
        arr[i] = (i * 7919) % 211;
    c_qsort(arr, 200, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 1; i < 200; i++)
        if (arr[i - 1] > arr[i])
            sorted = 0;
    CHECK(sorted);
    c_qsort(arr, 0, sizeof(int), cmp_int);
    c_qsort(arr, 1, sizeof(int), cmp_int);

    /* allocator */
    void *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = c_malloc((size_t)(i * 37 + 1));
        CHECK(blocks[i] != NULL && ((uintptr_t)blocks[i] & 15) == 0);
        memset(blocks[i], i, (size_t)(i * 37 + 1));
    }
    for (int i = 0; i < 64; i++)
        CHECK(((unsigned char *)blocks[i])[i * 37] == (unsigned char)i);
    for (int i = 0; i < 64; i += 2)
        c_free(blocks[i]);
    for (int i = 1; i < 64; i += 2) {
        blocks[i] = c_realloc(blocks[i], (size_t)(i * 37 + 500));
        CHECK(blocks[i] != NULL && ((unsigned char *)blocks[i])[i * 37] == (unsigned char)i);
    }
    for (int i = 1; i < 64; i += 2)
        c_free(blocks[i]);
    /* After freeing everything each arena is one block again: a request
     * below the big threshold fits without a new mapping. */
    int maps_before = g_maps;
    void *big = c_malloc(16000);
    CHECK(big != NULL && g_maps == maps_before);
    c_free(big);
    void *huge = c_malloc(100000);   /* its own mapping */
    CHECK(huge != NULL && g_maps == maps_before + 1);
    memset(huge, 1, 100000);
    c_free(huge);
    CHECK(g_unmaps == 1);
    void *z = c_calloc(10, 10);
    CHECK(z != NULL && ((char *)z)[99] == 0);
    c_free(z);
    CHECK(c_malloc((size_t)1 << 50) == NULL);
    void *p0 = c_malloc(0);
    CHECK(p0 != NULL);
    c_free(p0);

    /* The three floating conversions (libc/src/printf.c). Diagnostics,
     * not numerics: fixed precision, `double` only, no libm -- so the
     * checks are the shape of the output and the cases a naive
     * implementation gets wrong, not the last bit of the mantissa. */
    char f[64];
    c_snprintf(f, sizeof(f), "%f", 3.14159265);
    CHECK(strcmp(f, "3.141593") == 0);
    c_snprintf(f, sizeof(f), "%.2f", 3.14159265);
    CHECK(strcmp(f, "3.14") == 0);
    c_snprintf(f, sizeof(f), "%.0f", 2.5);
    CHECK(strcmp(f, "3") == 0);            /* rounds up into the next digit */
    c_snprintf(f, sizeof(f), "%.2f", 9.999);
    CHECK(strcmp(f, "10.00") == 0);        /* the carry reaches the integer part */
    c_snprintf(f, sizeof(f), "%f", 0.0);
    CHECK(strcmp(f, "0.000000") == 0);
    c_snprintf(f, sizeof(f), "%f", -0.5);
    CHECK(strcmp(f, "-0.500000") == 0);
    c_snprintf(f, sizeof(f), "%+.1f", 1.0);
    CHECK(strcmp(f, "+1.0") == 0);
    c_snprintf(f, sizeof(f), "%8.2f|", 1.5);
    CHECK(strcmp(f, "    1.50|") == 0);    /* width pads, and the sign is inside it */
    c_snprintf(f, sizeof(f), "%-8.2f|", 1.5);
    CHECK(strcmp(f, "1.50    |") == 0);
    c_snprintf(f, sizeof(f), "%08.2f", -1.5);
    CHECK(strcmp(f, "-0001.50") == 0);     /* the zeros go after the sign, as for an integer */
    c_snprintf(f, sizeof(f), "%e", 1234.5);
    CHECK(strcmp(f, "1.234500e+03") == 0);
    c_snprintf(f, sizeof(f), "%.2e", 0.00042);
    CHECK(strcmp(f, "4.20e-04") == 0);
    c_snprintf(f, sizeof(f), "%E", 1234.5);
    CHECK(strcmp(f, "1.234500E+03") == 0);
    c_snprintf(f, sizeof(f), "%g", 100.0);
    CHECK(strcmp(f, "100") == 0);          /* trailing zeros and the point go */
    c_snprintf(f, sizeof(f), "%g", 0.0001);
    CHECK(strcmp(f, "0.0001") == 0);
    c_snprintf(f, sizeof(f), "%g", 0.00001);
    CHECK(strcmp(f, "1e-05") == 0);        /* past the exponent where %g switches */
    c_snprintf(f, sizeof(f), "%g", 1234567.0);
    CHECK(strcmp(f, "1.23457e+06") == 0);
    /* A value with no integer part a 64-bit split could hold: exponent
     * form rather than a wrong answer. */
    c_snprintf(f, sizeof(f), "%f", 1e30);
    CHECK(f[0] == '1' && strchr(f, 'e') != NULL);
    /* '#' keeps the decimal point a precision of zero would drop, and
     * keeps %g's trailing zeros. */
    c_snprintf(f, sizeof(f), "%#.0f", 1.0);
    CHECK(strcmp(f, "1.") == 0);
    c_snprintf(f, sizeof(f), "%#.0e", 1.0);
    CHECK(strcmp(f, "1.e+00") == 0);
    c_snprintf(f, sizeof(f), "%#g", 100.0);
    CHECK(strcmp(f, "100.000") == 0);
    c_snprintf(f, sizeof(f), "%.0f", 1.0);
    CHECK(strcmp(f, "1") == 0);            /* and without it, no point */

    /* Not numbers. */
    double zero = 0.0;
    c_snprintf(f, sizeof(f), "%f", 1.0 / zero);
    CHECK(strcmp(f, "inf") == 0);
    c_snprintf(f, sizeof(f), "%f", -1.0 / zero);
    CHECK(strcmp(f, "-inf") == 0);
    c_snprintf(f, sizeof(f), "%F", zero / zero);
    CHECK(strcmp(f, "NAN") == 0);

    if (g_failures) {
        printf("libc                          FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("libc                          ok\n");
    return 0;
}
