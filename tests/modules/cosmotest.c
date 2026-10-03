/*
 * cosmotest.c - Self-test fixture: exercises every section group
 * (text, rodata, data, bss) and exports symbols for a dependant module
 * and for the kernel's module self-tests. Loaded by the self-tests
 * only (tests/cosmotest.ko in the boot archive).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* rodata: a table the dependant reads through a relocated pointer. */
const int cosmotest_table[4] = { 1, 2, 3, 36 };
EXPORT_SYMBOL(cosmotest_table);

/* data: initialised, mutated by init and by the dependant. */
int cosmotest_counter = 100;
EXPORT_SYMBOL(cosmotest_counter);

/* bss: zeroed by the loader, checked by init. */
static char cosmotest_scratch[256];

/* A function pointer table in data, relocated with R_X86_64_64. */
static int (*const cosmotest_ops[])(void) = { NULL };

int cosmotest_answer(void);
/* Both object and class name live in the unloadable image. The graph
 * must keep its own metadata after module-unload frees this rodata. */
static spinlock_t cosmotest_lock = SPINLOCK_INIT("cosmotest-unloadable-lock");

int cosmotest_answer(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&cosmotest_lock);
    int sum = 0;
    for (unsigned i = 0; i < 4; i++)
        sum += cosmotest_table[i];
    spin_unlock_irqrestore(&cosmotest_lock, s);
    return sum;   /* 42 */
}
EXPORT_SYMBOL(cosmotest_answer);

/* A kernel object whose release code lives in this module: the unload
 * protocol must keep the module's text until the last reference drops
 * (self-test module-unload-busy). */
int cosmotest_released;
EXPORT_SYMBOL(cosmotest_released);

static void cosmotest_obj_release(struct kobject *obj)
{
    (void)obj;
    cosmotest_released++;
}

static const struct kobject_type cosmotest_obj_type = { .name = "cosmotest", .release = cosmotest_obj_release };
static struct kobject cosmotest_obj;

struct kobject *cosmotest_object_take(void);
struct kobject *cosmotest_object_take(void)
{
    kobject_init(&cosmotest_obj, &cosmotest_obj_type);   /* reference 1 goes to the caller */
    return &cosmotest_obj;
}
EXPORT_SYMBOL(cosmotest_object_take);

/* Uses kernel exports across the module ABI: kmalloc, memset, strlen. */
static int cosmotest_init(void)
{
    for (unsigned i = 0; i < sizeof(cosmotest_scratch); i++) {
        if (cosmotest_scratch[i] != 0) {
            kerror("cosmotest: bss not zeroed");
            return -EINVAL;
        }
    }
    char *buf = kmalloc(64, 0);
    if (buf == NULL)
        return -ENOMEM;
    memset(buf, 'x', 63);
    buf[63] = '\0';
    size_t n = strlen(buf);
    kfree(buf);
    if (n != 63)
        return -EINVAL;
    if (cosmotest_ops[0] != NULL)
        return -EINVAL;
    cosmotest_counter++;
    kinfo("cosmotest: init ok, answer %d, counter %d", cosmotest_answer(), cosmotest_counter);
    return 0;
}

static void cosmotest_shutdown(void)
{
    kinfo("cosmotest: shutdown, counter %d", cosmotest_counter);
}

COSMO_MODULE("cosmotest", "1.0", cosmotest_init, cosmotest_shutdown, "", MODULE_CAP_TEST);
