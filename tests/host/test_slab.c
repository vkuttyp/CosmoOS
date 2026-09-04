/*
 * test_slab.c - Host unit tests for kernel/memory/slab.c and kmalloc.c,
 * running on a real arena under ASan/UBSan so out-of-bounds writes into
 * slab headers or neighbouring objects are caught by the sanitizer.
 */

#include <stdlib.h>
#include <string.h>

#include <kernel/kmalloc.h>
#include <kernel/page.h>

#include "harness.h"
#include "slab.h"

#define ARENA_BYTES (32u << 20)

/* kmalloc_init runs once per kernel, so once per process here. Tests
 * share the arena and prove cleanliness through free-page counts. */
static void setup(void)
{
}

static void teardown(void)
{
}

static void global_setup(void)
{
    host_arena_init(ARENA_BYTES);
    kmalloc_init();
    /* Warm up: the first create/destroy leaves a retained empty slab in
     * the cache-of-caches; take it out of every test's baseline. */
    struct kmem_cache *w = kmem_cache_create("warm", 64, 0);
    void *o = kmem_cache_alloc(w, 0);
    kmem_cache_free(w, o);
    kmem_cache_destroy(w);
}

static void test_cache_basic(void)
{
    setup();
    uint64_t before = host_arena_free_pages();

    struct kmem_cache *c = kmem_cache_create("obj48", 48, 0);
    EXPECT(c != NULL);
    EXPECT(c->slot_size == 48);
    EXPECT(c->objects_per_slab >= 8);

    void *a = kmem_cache_alloc(c, KMEM_ZERO);
    void *b = kmem_cache_alloc(c, 0);
    EXPECT(a != NULL && b != NULL && a != b);
    EXPECT(((uintptr_t)a & 15) == 0);
    EXPECT(memcmp(a, "\0\0\0\0\0\0\0\0", 8) == 0);
    memset(a, 0xAA, 48);
    memset(b, 0xBB, 48);
    EXPECT(((uint8_t *)a)[47] == 0xAA && ((uint8_t *)b)[0] == 0xBB);
    EXPECT(slab_of(a) == slab_of(b));
    EXPECT(slab_of(a)->cache == c);
    EXPECT(c->nr_allocated == 2);

    kmem_cache_free(c, a);
    kmem_cache_free(c, b);
    EXPECT(c->nr_allocated == 0);
    kmem_cache_destroy(c);
    EXPECT(host_arena_free_pages() == before);
    teardown();
}

static void test_cache_growth_and_shrink(void)
{
    setup();
    uint64_t before = host_arena_free_pages();
    struct kmem_cache *c = kmem_cache_create("obj200", 200, 64);
    EXPECT(c != NULL);

    enum { N = 3000 };
    void **objs = malloc(N * sizeof(void *));
    for (unsigned i = 0; i < N; i++) {
        objs[i] = kmem_cache_alloc(c, 0);
        EXPECT(objs[i] != NULL);
        EXPECT(((uintptr_t)objs[i] & 63) == 0);
        memset(objs[i], (int)i, 200);
    }
    EXPECT(c->nr_slabs > 1);
    EXPECT(c->nr_allocated == N);
    for (unsigned i = 0; i < N; i++)
        EXPECT(((uint8_t *)objs[i])[199] == (uint8_t)i);

    /* Free every other object: slabs go full -> partial, none released. */
    for (unsigned i = 0; i < N; i += 2)
        kmem_cache_free(c, objs[i]);
    unsigned slabs_mid = c->nr_slabs;
    /* Free the rest: slabs empty out; at most SLAB_KEEP_EMPTY retained. */
    for (unsigned i = 1; i < N; i += 2)
        kmem_cache_free(c, objs[i]);
    EXPECT(c->nr_allocated == 0);
    EXPECT(c->nr_slabs <= 2);
    EXPECT(c->nr_slabs < slabs_mid);
    EXPECT(host_arena_free_pages() >= before - 2 * (1u << c->slab_order));

    kmem_cache_destroy(c);
    EXPECT(host_arena_free_pages() == before);
    free(objs);
    teardown();
}

static void test_cache_misuse(void)
{
    setup();
    struct kmem_cache *c = kmem_cache_create("obj32", 32, 0);
    struct kmem_cache *d = kmem_cache_create("other", 32, 0);
    void *a = kmem_cache_alloc(c, 0);
    EXPECT(a != NULL);

    EXPECT_PANIC(kmem_cache_free(d, a));                  /* wrong cache */
    EXPECT_PANIC(kmem_cache_free(c, (uint8_t *)a + 8));   /* interior pointer */
    kmem_cache_free(c, a);
    EXPECT_PANIC(kmem_cache_free(c, a));                  /* double free */

    void *live = kmem_cache_alloc(c, 0);
    EXPECT(live != NULL);
    EXPECT_PANIC(kmem_cache_destroy(c));                  /* live objects */
    kmem_cache_free(c, live);
    kmem_cache_destroy(c);
    kmem_cache_destroy(d);

    /* Too large for any slab order. */
    EXPECT(kmem_cache_create("huge", 40000, 0) == NULL);
    teardown();
}

static void test_kmalloc_classes(void)
{
    setup();
    uint64_t before = host_arena_free_pages();
    static const size_t sizes[] = { 1, 16, 17, 48, 49, 96, 97, 128, 129, 192, 256, 384, 512,
                                    768, 1024, 2048, 4096, 8192, 8193, 20000, 65536, 1u << 20 };
    void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ptrs[i] = kzalloc(sizes[i]);
        EXPECT(ptrs[i] != NULL);
        EXPECT(((uintptr_t)ptrs[i] & 15) == 0);
        EXPECT(kmalloc_size(ptrs[i]) >= sizes[i]);
        /* Writing the full usable size must be in bounds (ASan checks
         * neighbours in the arena are not touched). */
        memset(ptrs[i], 0x5A, kmalloc_size(ptrs[i]));
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        kfree(ptrs[i]);

    struct kmalloc_stats st;
    kmalloc_get_stats(&st);
    EXPECT(st.live_objects == 0);
    EXPECT(st.large_pages == 0);
    EXPECT(host_arena_free_pages() + 64 >= before);
    teardown();
}

static void test_krealloc(void)
{
    setup();
    uint8_t *p = kmalloc(10, 0);
    for (unsigned i = 0; i < 10; i++)
        p[i] = (uint8_t)(i + 1);
    p = krealloc(p, 5000, 0);
    EXPECT(p != NULL);
    for (unsigned i = 0; i < 10; i++)
        EXPECT(p[i] == i + 1);
    /* KMEM_ZERO on grow zeroes beyond the old *usable* size (8192 for a
     * 5000-byte request), not beyond the old requested size: the heap
     * does not record request sizes. */
    size_t old_usable = kmalloc_size(p);
    EXPECT(old_usable == 8192);
    p = krealloc(p, 100000, KMEM_ZERO);
    EXPECT(p != NULL);
    for (unsigned i = 0; i < 10; i++)
        EXPECT(p[i] == i + 1);
    EXPECT(p[old_usable] == 0 && p[99999] == 0);
    p = krealloc(p, 8, 0);
    EXPECT(p != NULL && p[7] == 8);
    EXPECT(krealloc(p, 0, 0) == NULL);
    EXPECT(kmalloc(0, 0) == NULL);
    EXPECT(kmalloc(KMALLOC_MAX_SIZE + 1, 0) == NULL);
    kfree(NULL);
    teardown();
}

static void test_kfree_misuse(void)
{
    setup();
    void *a = kmalloc(100, 0);
    EXPECT(a != NULL);
    kfree(a);
    EXPECT_PANIC(kfree(a));                      /* double free via kfree */
    void *big = kmalloc(20000, 0);
    EXPECT(big != NULL);
    EXPECT_PANIC(kfree((uint8_t *)big + 4096));  /* interior of large alloc */
    kfree(big);
    int on_stack;
    EXPECT_PANIC(kfree(&on_stack));              /* not heap memory */
    teardown();
}

/* Interleaved random kmalloc/kfree with content verification. */
static void test_random_stress(void)
{
    setup();
    struct { uint8_t *p; size_t n; uint8_t tag; } live[256];
    unsigned nlive = 0;
    unsigned seed = 777;

    for (unsigned step = 0; step < 30000; step++) {
        seed = seed * 1103515245u + 12345u;
        if (nlive == 0 || (nlive < 256 && (seed >> 16) % 3 != 0)) {
            size_t n = 1 + ((seed >> 4) % 12000);
            uint8_t *p = kmalloc(n, 0);
            EXPECT(p != NULL);
            uint8_t tag = (uint8_t)(seed >> 24);
            memset(p, tag, n);
            live[nlive].p = p;
            live[nlive].n = n;
            live[nlive].tag = tag;
            nlive++;
        } else {
            unsigned idx = (seed >> 8) % nlive;
            EXPECT(live[idx].p[0] == live[idx].tag && live[idx].p[live[idx].n - 1] == live[idx].tag);
            kfree(live[idx].p);
            live[idx] = live[--nlive];
        }
    }
    while (nlive > 0)
        kfree(live[--nlive].p);

    struct kmalloc_stats st;
    kmalloc_get_stats(&st);
    EXPECT(st.live_objects == 0);
    EXPECT(st.large_pages == 0);
    teardown();
}

int main(void)
{
    static const struct host_test tests[] = {
        { "cache_basic", test_cache_basic },
        { "cache_growth_and_shrink", test_cache_growth_and_shrink },
        { "cache_misuse", test_cache_misuse },
        { "kmalloc_classes", test_kmalloc_classes },
        { "krealloc", test_krealloc },
        { "kfree_misuse", test_kfree_misuse },
        { "random_stress", test_random_stress },
    };
    global_setup();
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    host_arena_destroy();
    return rc;
}
