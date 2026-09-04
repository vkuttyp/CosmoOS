/*
 * cosmotest_dep.c - Self-test fixture that depends on cosmotest: resolves
 * a function, a rodata table and a data variable from another module.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/module.h>

extern const int cosmotest_table[4];
extern int cosmotest_counter;
int cosmotest_answer(void);

int cosmotest_dep_sum(void);

int cosmotest_dep_sum(void)
{
    return cosmotest_answer() + cosmotest_table[3] + cosmotest_counter;
}
EXPORT_SYMBOL(cosmotest_dep_sum);

static int cosmotest_dep_init(void)
{
    if (cosmotest_answer() != 42) {
        kerror("cosmotest_dep: cosmotest_answer() != 42");
        return -EINVAL;
    }
    cosmotest_counter += 10;
    kinfo("cosmotest_dep: init ok, sum %d", cosmotest_dep_sum());
    return 0;
}

static void cosmotest_dep_shutdown(void)
{
    cosmotest_counter -= 10;
    kinfo("cosmotest_dep: shutdown");
}

COSMO_MODULE("cosmotest_dep", "1.0", cosmotest_dep_init, cosmotest_dep_shutdown, "cosmotest", MODULE_CAP_TEST);
