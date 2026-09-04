/*
 * cosmotest_fail.c - Self-test fixture whose init() fails. The loader
 * must return its error and leave nothing behind.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/module.h>

static int cosmotest_fail_state = 1;

static int cosmotest_fail_init(void)
{
    kinfo("cosmotest_fail: init refusing on purpose (state %d)", cosmotest_fail_state);
    return -EIO;
}

static void cosmotest_fail_shutdown(void)
{
    kerror("cosmotest_fail: shutdown must never run");
}

COSMO_MODULE("cosmotest_fail", "0.1", cosmotest_fail_init, cosmotest_fail_shutdown, "", MODULE_CAP_TEST);
