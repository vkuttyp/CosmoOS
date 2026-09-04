/*
 * hello.c - The smallest possible kernel module: proves the load
 * pipeline end to end at every boot. Loaded from the boot archive as
 * modules/hello.ko; the boot test requires its log line.
 */

#include <kernel/log.h>
#include <kernel/module.h>

static unsigned hello_loads;

static int hello_init(void)
{
    hello_loads++;
    kinfo("hello: module init (ABI v%u, load %u)", COSMO_MODULE_ABI_VERSION, hello_loads);
    return 0;
}

static void hello_shutdown(void)
{
    kinfo("hello: module shutdown");
}

COSMO_MODULE("hello", "1.0", hello_init, hello_shutdown, "", MODULE_CAP_NONE);
