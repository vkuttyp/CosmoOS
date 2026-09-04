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

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    char buf[128];
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
    CHECK(c_snprintf(buf, sizeof(buf), "%f", 1.5) == 1 && strcmp(buf, "?") == 0);
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

    if (g_failures) {
        printf("libc                          FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("libc                          ok\n");
    return 0;
}
