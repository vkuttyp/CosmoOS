/*
 * malloc.c - A small allocator over anonymous mmap (docs/libc/design.md).
 *
 * Arenas of ARENA_SIZE hold blocks with a 16-byte header; a first-fit
 * free list threads through the free blocks; free coalesces with both
 * physical neighbours. Requests above BIG_THRESHOLD get their own mapping
 * and give it back on free. Single-threaded by design.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_SIZE    (64u * 1024u)
#define BIG_THRESHOLD (16u * 1024u)
#define ALIGN         16u
#define MIN_SPLIT     32u
#define PAGE          4096u

#define INUSE 1u
#define BIG   2u
#define FLAGS 3u

struct hdr {
    size_t size;        /* payload bytes (multiple of ALIGN); low bits: flags */
    size_t prev_size;   /* payload size of the physical predecessor (0: first in arena) */
};

struct free_blk {
    struct hdr h;
    struct free_blk *next;
};

static struct free_blk *g_free;

static size_t blk_size(const struct hdr *h) { return h->size & ~(size_t)FLAGS; }
static int blk_inuse(const struct hdr *h) { return (h->size & INUSE) != 0; }
static struct hdr *next_hdr(struct hdr *h) { return (struct hdr *)((char *)(h + 1) + blk_size(h)); }
static struct hdr *prev_hdr(struct hdr *h)
{
    return h->prev_size ? (struct hdr *)((char *)h - h->prev_size - sizeof(struct hdr)) : NULL;
}

/* Every arena ends with a zero-size in-use sentinel so next_hdr is always valid. */
static void set_sentinel(struct hdr *h, size_t prev)
{
    h->size = INUSE;
    h->prev_size = prev;
}

static void free_push(struct hdr *h)
{
    struct free_blk *f = (struct free_blk *)h;
    f->next = g_free;
    g_free = f;
}

static void free_remove(struct hdr *h)
{
    struct free_blk **pp = &g_free;
    while (*pp) {
        if ((struct hdr *)*pp == h) {
            *pp = (*pp)->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void *map(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int new_arena(void)
{
    struct hdr *h = map(ARENA_SIZE);
    if (h == NULL)
        return -1;
    h->size = ARENA_SIZE - 2 * sizeof(struct hdr);
    h->prev_size = 0;
    set_sentinel(next_hdr(h), h->size);
    free_push(h);
    return 0;
}

static size_t round_up(size_t n)
{
    if (n == 0)
        n = 1;
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

void *malloc(size_t n)
{
    if (n > ((size_t)1 << 40)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t need = round_up(n);
    if (need > BIG_THRESHOLD) {
        size_t total = (need + sizeof(struct hdr) + PAGE - 1) & ~(size_t)(PAGE - 1);
        struct hdr *h = map(total);
        if (h == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        h->size = (total - sizeof(struct hdr)) | INUSE | BIG;
        h->prev_size = total;   /* the mapping size, for munmap */
        return h + 1;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        struct free_blk **pp = &g_free;
        while (*pp) {
            struct hdr *h = (struct hdr *)*pp;
            size_t sz = blk_size(h);
            if (sz >= need) {
                *pp = (*pp)->next;
                if (sz - need >= MIN_SPLIT + sizeof(struct hdr)) {
                    struct hdr *rest = (struct hdr *)((char *)(h + 1) + need);
                    rest->size = sz - need - sizeof(struct hdr);
                    rest->prev_size = need;
                    next_hdr(rest)->prev_size = blk_size(rest);
                    h->size = need;
                    free_push(rest);
                }
                h->size |= INUSE;
                return h + 1;
            }
            pp = &(*pp)->next;
        }
        if (new_arena() < 0)
            break;
    }
    errno = ENOMEM;
    return NULL;
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void free(void *p)
{
    if (p == NULL)
        return;
    struct hdr *h = (struct hdr *)p - 1;
    if (!blk_inuse(h))
        abort();   /* double free */
    if (h->size & BIG) {
        munmap(h, h->prev_size);
        return;
    }
    h->size = blk_size(h);
    struct hdr *nx = next_hdr(h);
    if (!blk_inuse(nx)) {
        free_remove(nx);
        h->size += sizeof(struct hdr) + blk_size(nx);
    }
    struct hdr *pv = prev_hdr(h);
    if (pv && !blk_inuse(pv)) {
        free_remove(pv);
        pv->size += sizeof(struct hdr) + blk_size(h);
        h = pv;
    }
    next_hdr(h)->prev_size = blk_size(h);
    free_push(h);
}

void *realloc(void *p, size_t n)
{
    if (p == NULL)
        return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    struct hdr *h = (struct hdr *)p - 1;
    size_t have = blk_size(h);
    size_t need = round_up(n);
    if (need <= have)
        return p;
    if (!(h->size & BIG)) {
        struct hdr *nx = next_hdr(h);
        if (!blk_inuse(nx) && have + sizeof(struct hdr) + blk_size(nx) >= need) {
            free_remove(nx);
            h->size = (have + sizeof(struct hdr) + blk_size(nx)) | INUSE;
            next_hdr(h)->prev_size = blk_size(h);
            return p;
        }
    }
    void *q = malloc(n);
    if (q == NULL)
        return NULL;
    memcpy(q, p, have < n ? have : n);
    free(p);
    return q;
}
