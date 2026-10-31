/*
 * test_handles.c - Host test of the kernel object and handle layer
 * (kernel/object/object.c, handle.c; docs/kernel/object/testing.md).
 * ASan/UBSan.
 *
 * Besides direct checks, a recorded trace: 10,000 operations, chosen by a
 * seeded generator, over three handle tables and eight objects (installs,
 * lookups with rights, gets, flags, closes, close-on-exec, transfers, limit
 * changes, references taken and dropped, tryget, put_and_lock, a table
 * destroyed and remade). Every result -- return values, the reference and
 * handle counts it leaves, which releases, flushes and epoll notices ran --
 * is a line, and the lines' FNV-1a digest is recorded below. This is the
 * differential test of roadmap 2.0 SA2: an implementation of this layer in
 * CeeSafe must give the same digest before the C is deleted.
 * HANDLES_TRACE_DUMP=1 prints the lines.
 */

#include "harness.h"

#include <kernel/epoll.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/module.h>
#include <kernel/object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- what the layer calls outside itself ----------------------------------- */

struct module *module_owner_of(uintptr_t addr)
{
    (void)addr;
    return NULL;   /* the kernel's own code: no owner to count */
}

void module_object_released(struct module *m)
{
    (void)m;
}

static unsigned g_epoll_notices;

void epoll_last_handle_closed(struct kobject *obj)
{
    (void)obj;
    g_epoll_notices++;
}

/* --- the objects ------------------------------------------------------------- */

#define OBJECTS 8

struct counted {
    struct kobject obj;
    unsigned id;
    unsigned generation;   /* how many times this slot was made: a released object is made again */
    unsigned released;     /* releases of this generation: must end at 1 */
    unsigned flushes;
    bool alive;
};

static struct counted g_objects[OBJECTS];
static unsigned g_releases;

static void counted_release(struct kobject *obj)
{
    struct counted *c = (struct counted *)obj;
    c->released++;
    c->alive = false;
    g_releases++;
}

static int counted_flush(struct kobject *obj)
{
    struct counted *c = (struct counted *)obj;
    c->flushes++;
    return c->id % 3 == 0 ? -EIO : 0;   /* some objects report a write-back error at close */
}

static const struct kobject_type plain_type = { .name = "counted", .release = counted_release };
static const struct kobject_io_type io_type = {
    .base = { .name = "counted-io", .release = counted_release, .flags = KOBJECT_TYPE_IO },
    .flush = counted_flush,
};

static void make_object(unsigned i)
{
    struct counted *c = &g_objects[i];
    unsigned generation = c->generation + 1;
    memset(c, 0, sizeof(*c));
    c->id = i;
    c->generation = generation;
    c->alive = true;
    kobject_init(&c->obj, i % 2 ? &io_type.base : &plain_type);
    c->obj.watched = i % 4 == 1;   /* as if registered with epoll once: close tells epoll at the last handle */
}

static int object_index(const struct kobject *obj)
{
    for (unsigned i = 0; i < OBJECTS; i++)
        if (obj == &g_objects[i].obj)
            return (int)i;
    return -1;
}

/* --- direct checks ------------------------------------------------------------ */

static void test_refcount(void)
{
    uint64_t census = kobject_census();
    make_object(0);
    struct counted *c = &g_objects[0];
    EXPECT(kobject_census() == census + 1);
    EXPECT(kobject_refcount(&c->obj) == 1);
    kobject_get(&c->obj);
    kobject_get_n(&c->obj, 3);
    EXPECT(kobject_refcount(&c->obj) == 5);
    EXPECT(kobject_tryget(&c->obj));
    for (int i = 0; i < 5; i++)
        kobject_put(&c->obj);
    EXPECT(c->released == 0 && kobject_refcount(&c->obj) == 1);
    kobject_put(&c->obj);
    EXPECT(c->released == 1 && kobject_census() == census);
    EXPECT(!kobject_tryget(&c->obj));
    EXPECT_PANIC(kobject_get(&c->obj));     /* OB1: a get from zero (it raised the count before panicking) */
    c->obj.refcount = 0;
    EXPECT_PANIC(kobject_put(&c->obj));     /* OB1: an underflow */
    c->obj.refcount = 0;
    EXPECT(c->released == 1);
}

static void test_discard(void)
{
    uint64_t census = kobject_census();
    make_object(1);
    EXPECT(kobject_census() == census + 1);
    kobject_discard(&g_objects[1].obj);
    EXPECT(kobject_census() == census && g_objects[1].released == 0);
    make_object(1);
    kobject_get(&g_objects[1].obj);
    EXPECT_PANIC(kobject_discard(&g_objects[1].obj));   /* someone else holds it */
    g_objects[1].obj.refcount = 1;
    kobject_put(&g_objects[1].obj);
    EXPECT(kobject_census() == census);
}

static void test_put_and_lock(void)
{
    spinlock_t lock;
    spinlock_init(&lock, "test");
    make_object(2);
    struct counted *c = &g_objects[2];
    kobject_get(&c->obj);
    arch_irq_state_t s;
    EXPECT(!kobject_put_and_lock(&c->obj, &lock, &s));          /* 2 -> 1, without the lock */
    EXPECT(kobject_put_and_lock(&c->obj, &lock, &s));           /* 1 -> 0, the lock held */
    EXPECT(c->released == 0);
    spin_unlock_irqrestore(&lock, s);
    kobject_release_final(&c->obj);
    EXPECT(c->released == 1);
}

static void test_table(void)
{
    struct handle_table *t = calloc(1, sizeof(*t));
    handle_table_init(t);
    make_object(3);
    struct counted *c = &g_objects[3];
    EXPECT(handle_install(t, &c->obj, HANDLE_RIGHT_READ) == 0);
    EXPECT(c->obj.handles == 1 && kobject_refcount(&c->obj) == 2);
    bool missing = false;
    EXPECT(handle_lookup_rights(t, 0, HANDLE_RIGHT_WRITE, &missing) == NULL && missing);
    EXPECT(handle_install_at(t, 5, &c->obj, HANDLE_RIGHT_ALL) == 5);
    EXPECT(handle_set_flags(t, 5, HANDLE_FLAG_CLOEXEC) == 0);
    handle_close_on_exec(t);
    EXPECT(handle_table_count(t) == 1 && c->obj.handles == 1);
    EXPECT(handle_close(t, 0) == -EIO);   /* id 3's flush fails; the handle is closed regardless */
    EXPECT(handle_close(t, 0) == -EBADF && c->obj.handles == 0);
    handle_table_destroy(t);
    EXPECT(handle_install(t, &c->obj, HANDLE_RIGHT_READ) == -EBADF);   /* the exit's gate */
    free(t);
    kobject_put(&c->obj);
    EXPECT(c->released == 1);
}

/* --- the recorded trace ---------------------------------------------------- */

#define TABLES 3
#define TRACE_OPS 10000
#define HELD_MAX 256
#define TRACE_SEED 0x5A2C0FFEE2026ull
/* The digest of the C implementation's trace (2026-10-30). An
 * implementation that disagrees changes it: find the first differing line
 * with HANDLES_TRACE_DUMP=1 against the C. */
#define TRACE_DIGEST 0xfd5772457a0ad4a1ull

static uint64_t g_rng;
static uint64_t g_digest;
static bool g_dump;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n)
{
    return (unsigned)(next() % n);
}

static void emit(const char *line)
{
    for (const char *p = line; *p; p++) {
        g_digest ^= (uint8_t)*p;
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

/* The references the trace holds: each object's creator's (until the trace
 * drops it), and a pool of those lookups and gets returned, which the trace
 * drops in its own time. A released object is made again at the next step. */
static bool g_creator[OBJECTS];
static struct kobject *g_held[HELD_MAX];
static unsigned g_nheld;

static void hold(struct kobject *obj)
{
    if (g_nheld < HELD_MAX)
        g_held[g_nheld++] = obj;
    else
        kobject_put(obj);   /* never reached at these sizes */
}

static void drop_held(unsigned k)
{
    struct kobject *obj = g_held[k];
    g_held[k] = g_held[--g_nheld];
    kobject_put(obj);
}

/* An object the trace holds a reference to (so it is alive), or -1. */
static int held_object(void)
{
    unsigned i = pick(OBJECTS);
    if (g_creator[i])
        return (int)i;
    for (unsigned k = 0; k < g_nheld; k++)
        if (g_held[k] == &g_objects[i].obj)
            return (int)i;
    return -1;
}

static unsigned random_rights(void)
{
    static const unsigned choices[] = {
        HANDLE_RIGHT_READ, HANDLE_RIGHT_WRITE, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE, HANDLE_RIGHT_ALL,
        HANDLE_RIGHT_READ | HANDLE_RIGHT_TRANSFER, HANDLE_RIGHT_SOCK_ALL, HANDLE_RIGHT_DUP, 0,
    };
    return choices[pick(sizeof(choices) / sizeof(choices[0]))];
}

static void line_with_state(char *buf, size_t n, const char *what, int o, long result)
{
    if (o >= 0) {
        const struct counted *c = &g_objects[o];
        snprintf(buf, n, "%s -> %ld | o%d.%u refs %u handles %u released %u flushes %u | epoll %u releases %u", what,
                 result, o, c->generation, kobject_refcount(&c->obj), c->obj.handles, c->released, c->flushes,
                 g_epoll_notices, g_releases);
    } else {
        snprintf(buf, n, "%s -> %ld | epoll %u releases %u", what, result, g_epoll_notices, g_releases);
    }
}

static void test_trace(void)
{
    struct handle_table *tables[TABLES];
    uint64_t census = kobject_census();
    g_rng = TRACE_SEED;
    g_digest = 0xcbf29ce484222325ull;
    g_dump = getenv("HANDLES_TRACE_DUMP") != NULL;
    g_nheld = 0;
    g_epoll_notices = g_releases = 0;
    memset(g_objects, 0, sizeof(g_objects));
    for (unsigned i = 0; i < OBJECTS; i++) {
        make_object(i);
        g_creator[i] = true;
    }
    for (unsigned t = 0; t < TABLES; t++) {
        tables[t] = calloc(1, sizeof(*tables[t]));
        handle_table_init(tables[t]);
    }

    char line[256], what[96];
    for (unsigned step = 0; step < TRACE_OPS; step++) {
        for (unsigned i = 0; i < OBJECTS; i++) {
            if (!g_objects[i].alive) {   /* released at the last step: a new generation */
                make_object(i);
                g_creator[i] = true;
            }
        }
        unsigned ti = pick(TABLES);
        struct handle_table *t = tables[ti];
        /* Installs take the lowest free slot, so the low ones are where the handles are; a quarter of the
         * picks range over -2..65, out of range too. */
        int h = pick(4) == 0 ? (int)pick(HANDLE_TABLE_SIZE + 4) - 2 : (int)pick(8);
        unsigned op = pick(19);
        int o = -1;
        long r = 0;
        switch (op) {
        case 0: case 1: case 2: {   /* install, the commonest */
            o = held_object();
            if (o < 0)
                continue;
            unsigned rights = random_rights(), flags = pick(4) == 0 ? HANDLE_FLAG_CLOEXEC : 0;
            r = flags ? handle_install_flags(t, &g_objects[o].obj, rights, flags) : handle_install(t, &g_objects[o].obj, rights);
            snprintf(what, sizeof(what), "%u install t%u o%d rights %#x flags %u", step, ti, o, rights, flags);
            break;
        }
        case 3: {
            o = held_object();
            if (o < 0)
                continue;
            unsigned rights = random_rights(), flags = pick(2) ? HANDLE_FLAG_CLOEXEC : 0;
            r = flags ? handle_install_at_flags(t, h, &g_objects[o].obj, rights, flags)
                      : handle_install_at(t, h, &g_objects[o].obj, rights);
            snprintf(what, sizeof(what), "%u install_at t%u h%d o%d rights %#x flags %u", step, ti, h, o, rights, flags);
            break;
        }
        case 4: case 5: {
            unsigned need = random_rights();
            bool missing = false;
            struct kobject *obj = op == 4 ? handle_lookup(t, h, need) : handle_lookup_rights(t, h, need, &missing);
            o = obj ? object_index(obj) : -1;
            r = obj ? 1 : 0;
            if (obj)
                hold(obj);
            snprintf(what, sizeof(what), "%u lookup%s t%u h%d need %#x missing %d", step, op == 5 ? "_rights" : "", ti, h,
                     need, missing);
            break;
        }
        case 6: {
            unsigned rights = 0, flags = 0;
            struct kobject *obj = pick(2) ? handle_get(t, h, &rights) : handle_get_flags(t, h, &rights, &flags);
            o = obj ? object_index(obj) : -1;
            r = obj ? 1 : 0;
            if (obj)
                hold(obj);
            snprintf(what, sizeof(what), "%u get t%u h%d rights %#x flags %u", step, ti, h, rights, flags);
            break;
        }
        case 7: {
            unsigned flags = pick(2) ? HANDLE_FLAG_CLOEXEC : 0;
            r = handle_set_flags(t, h, flags);
            snprintf(what, sizeof(what), "%u set_flags t%u h%d flags %u", step, ti, h, flags);
            break;
        }
        case 8: case 9: {
            r = handle_close(t, h);
            snprintf(what, sizeof(what), "%u close t%u h%d", step, ti, h);
            break;
        }
        case 10: {
            handle_close_on_exec(t);
            r = handle_table_count(t);
            snprintf(what, sizeof(what), "%u close_on_exec t%u", step, ti);
            break;
        }
        case 11: {
            unsigned give = pick(3) == 0 ? COSMO_RIGHTS_SAME : random_rights();
            struct kobject *obj = NULL;
            unsigned rights = 0;
            r = handle_transfer_check(t, h, give, &obj, &rights);
            o = obj ? object_index(obj) : -1;
            if (obj)
                hold(obj);
            snprintf(what, sizeof(what), "%u transfer t%u h%d give %#x rights %#x", step, ti, h, give, rights);
            break;
        }
        case 12: {
            t->limit = 1 + pick(HANDLE_TABLE_SIZE);   /* COSMO_RLIMIT_NOFILE, as process.c sets it */
            r = handle_table_count(t);
            snprintf(what, sizeof(what), "%u limit t%u %u", step, ti, t->limit);
            break;
        }
        case 13: case 14: {
            if (g_nheld == 0)
                continue;
            unsigned k = pick(g_nheld);
            o = object_index(g_held[k]);
            drop_held(k);
            snprintf(what, sizeof(what), "%u put", step);
            break;
        }
        case 15: {
            o = held_object();
            if (o < 0)
                continue;
            if (pick(2)) {
                kobject_get(&g_objects[o].obj);
                hold(&g_objects[o].obj);
                r = 1;
            } else {
                r = kobject_tryget(&g_objects[o].obj);
                if (r)
                    hold(&g_objects[o].obj);
            }
            snprintf(what, sizeof(what), "%u get-ref", step);
            break;
        }
        case 16: {   /* a drop through put_and_lock, as a lookup table's owner does */
            if (g_nheld == 0)
                continue;
            unsigned k = pick(g_nheld);
            struct kobject *obj = g_held[k];
            g_held[k] = g_held[--g_nheld];
            o = object_index(obj);
            spinlock_t lock;
            spinlock_init(&lock, "trace");
            arch_irq_state_t s;
            r = kobject_put_and_lock(obj, &lock, &s);
            if (r) {
                spin_unlock_irqrestore(&lock, s);
                kobject_release_final(obj);
            }
            snprintf(what, sizeof(what), "%u put_and_lock", step);
            break;
        }
        case 17: {   /* the creator lets go: the object lives on in tables and the pool, or is released */
            o = (int)pick(OBJECTS);
            if (!g_creator[o])
                continue;
            g_creator[o] = false;
            kobject_put(&g_objects[o].obj);
            snprintf(what, sizeof(what), "%u creator-put", step);
            break;
        }
        default: {   /* a table torn down and made again, rarely */
            if (pick(8) != 0)
                continue;
            handle_table_destroy(t);
            r = handle_table_count(t);
            handle_table_init(t);
            snprintf(what, sizeof(what), "%u destroy t%u", step, ti);
            break;
        }
        }
        line_with_state(line, sizeof(line), what, o, r);
        emit(line);
    }

    for (unsigned t = 0; t < TABLES; t++) {
        handle_table_destroy(tables[t]);
        EXPECT(handle_table_count(tables[t]) == 0);
        free(tables[t]);
    }
    while (g_nheld > 0) {
        struct kobject *obj = g_held[--g_nheld];
        kobject_put(obj);
    }
    for (unsigned i = 0; i < OBJECTS; i++) {
        if (g_creator[i])
            kobject_put(&g_objects[i].obj);
        g_creator[i] = false;
    }
    for (unsigned i = 0; i < OBJECTS; i++)
        EXPECT(!g_objects[i].alive && g_objects[i].released == 1);
    EXPECT(kobject_census() == census);   /* the trace leaves no object behind */
    snprintf(line, sizeof(line), "end releases %u epoll %u", g_releases, g_epoll_notices);
    emit(line);
    printf("handles trace: %u operations, digest %#018llx (recorded %#018llx)\n", TRACE_OPS,
           (unsigned long long)g_digest, (unsigned long long)TRACE_DIGEST);
    EXPECT(g_digest == TRACE_DIGEST);
}

static const struct host_test tests[] = {
    { "object-refcount", test_refcount },
    { "object-discard", test_discard },
    { "object-put-and-lock", test_put_and_lock },
    { "handle-table", test_table },
    { "handle-trace", test_trace },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
