/*
 * test_lockdep.c - Host test of the lock-order checker's core
 * (kernel/include/kernel/lockdep_core.h, docs/kernel/lockdep/testing.md).
 * ASan/UBSan.
 */

#include "harness.h"

#include <kernel/lockdep_core.h>

#include <stdlib.h>
#include <string.h>

static const char *const k_a = "a", *const k_b = "b";

static void test_classes(void)
{
    struct lockdep_graph *g = calloc(1, sizeof(*g));
    EXPECT(g != NULL);
    int a = lockdep_core_class(g, k_a, LOCKDEP_KIND_SPIN);
    int b = lockdep_core_class(g, k_b, LOCKDEP_KIND_SPIN);
    EXPECT(a == 0 && b == 1);
    EXPECT(lockdep_core_class(g, k_a, LOCKDEP_KIND_SPIN) == a);            /* same name, same class */
    EXPECT(lockdep_core_class(g, k_a, LOCKDEP_KIND_MUTEX) == 2);            /* same name, other kind: new class */
    EXPECT(g->nr_classes == 3);
    /* Exhaustion is reported, not overrun. */
    char names[LOCKDEP_MAX_CLASSES][4];
    int rc = 0;
    for (unsigned i = 0; i < LOCKDEP_MAX_CLASSES && rc >= 0; i++) {
        names[i][0] = 'x';
        names[i][1] = (char)('0' + i % 10);
        names[i][2] = (char)('0' + (i / 10) % 10);
        names[i][3] = '\0';
        rc = lockdep_core_class(g, names[i], LOCKDEP_KIND_SPIN);
    }
    EXPECT(rc == -1);
    EXPECT(g->nr_classes == LOCKDEP_MAX_CLASSES);
    /* Node arithmetic. */
    EXPECT(lockdep_node(5, 3) == 23 && lockdep_node_class(23) == 5 && lockdep_node_subclass(23) == 3);
    EXPECT(lockdep_node(LOCKDEP_MAX_CLASSES - 1, LOCKDEP_SUBCLASSES - 1) == LOCKDEP_MAX_NODES - 1);
    free(g);
}

static void test_edges_and_cycles(void)
{
    struct lockdep_graph *g = calloc(1, sizeof(*g));
    struct lockdep_scratch *s = calloc(1, sizeof(*s));
    EXPECT(g != NULL && s != NULL);
    uint16_t a = lockdep_node(0, 0), b = lockdep_node(1, 0), c = lockdep_node(2, 0), d = lockdep_node(3, 1);
    uint16_t path[8];
    unsigned len = 0;

    EXPECT(!lockdep_core_has_edge(g, a, b));
    EXPECT(lockdep_core_add_edge(g, a, b));
    EXPECT(!lockdep_core_add_edge(g, a, b));   /* duplicate is not new */
    EXPECT(g->nr_edges == 1);
    EXPECT(lockdep_core_has_edge(g, a, b) && !lockdep_core_has_edge(g, b, a));

    /* a -> b -> c: c reaches nothing; a reaches c through b. */
    EXPECT(lockdep_core_add_edge(g, b, c));
    EXPECT(!lockdep_core_reaches(g, s, c, a, path, 8, &len) && len == 0);
    EXPECT(lockdep_core_reaches(g, s, a, c, path, 8, &len));
    EXPECT(len == 3 && path[0] == a && path[1] == b && path[2] == c);

    /* Would c -> a close a cycle? The check the kernel makes before adding:
     * does the new lock (a) reach a held one (c)? Yes. */
    EXPECT(lockdep_core_reaches(g, s, a, c, path, 8, &len));
    /* Would a -> d? d is unconnected: no. */
    EXPECT(!lockdep_core_reaches(g, s, d, a, path, 8, &len));

    /* A long chain truncates from the start and keeps the end. */
    for (unsigned i = 10; i < 30; i++)
        EXPECT(lockdep_core_add_edge(g, (uint16_t)i, (uint16_t)(i + 1)));
    EXPECT(lockdep_core_reaches(g, s, 10, 30, path, 8, &len));
    EXPECT(len == 21 && path[7] == 30 && path[0] == 23);

    /* Highest node works. */
    uint16_t hi = LOCKDEP_MAX_NODES - 1;
    EXPECT(lockdep_core_add_edge(g, c, hi));
    EXPECT(lockdep_core_reaches(g, s, a, hi, path, 8, &len) && len == 4 && path[3] == hi);
    EXPECT(!lockdep_core_reaches(g, s, hi, a, path, 8, &len));
    free(s);
    free(g);
}

/* The kernel's decision procedure, modelled: acquiring `node` with a held
 * set is an inversion iff node reaches a held node; otherwise edges from
 * every held node are added. Replays the ABBA and the three-lock cycle. */
static bool acquire(struct lockdep_graph *g, struct lockdep_scratch *s, const uint16_t *held, unsigned n,
                    uint16_t node)
{
    uint16_t path[8];
    unsigned len;
    for (unsigned i = 0; i < n; i++)
        if (lockdep_core_reaches(g, s, node, held[i], path, 8, &len))
            return false;   /* inversion */
    for (unsigned i = 0; i < n; i++)
        lockdep_core_add_edge(g, held[i], node);
    return true;
}

static void test_decision(void)
{
    struct lockdep_graph *g = calloc(1, sizeof(*g));
    struct lockdep_scratch *s = calloc(1, sizeof(*s));
    EXPECT(g != NULL && s != NULL);
    uint16_t a = 0, b = 4, c = 8;
    /* ABBA. */
    EXPECT(acquire(g, s, &a, 1, b));
    EXPECT(!acquire(g, s, &b, 1, a));
    /* Three locks: a -> b, b -> c, then c -> a is caught through the chain. */
    EXPECT(acquire(g, s, &b, 1, c));
    EXPECT(!acquire(g, s, &c, 1, a));
    /* Edges are added from every held lock, so a nested chain seen once
     * records a -> c directly too. */
    uint16_t ab[2] = { a, b };
    EXPECT(acquire(g, s, ab, 2, c));
    EXPECT(lockdep_core_has_edge(g, a, c));
    /* Subclasses are distinct nodes: a#1 under a#0 is an ordinary edge. */
    EXPECT(acquire(g, s, &a, 1, lockdep_node(0, 1)));
    EXPECT(!acquire(g, s, (uint16_t[]){ lockdep_node(0, 1) }, 1, a));
    free(s);
    free(g);
}

static const struct host_test tests[] = {
    { "classes", test_classes },
    { "edges-and-cycles", test_edges_and_cycles },
    { "decision", test_decision },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
