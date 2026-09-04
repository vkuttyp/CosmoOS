/*
 * interrupt.c - Vector-to-handler dispatch table.
 *
 * One slot per vector. Registration is a single pointer publish under
 * disabled interrupts; dispatch reads the slot without locking. That is
 * correct on one CPU and becomes correct on many once registration uses
 * a release store and dispatch an acquire load plus a grace period before
 * a handler's memory may be reused. The stores below are already ordered
 * that way so the SMP change is confined to adding the grace period.
 */

#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include <arch/irq.h>
#include <arch/trap.h>

/* Upper bound on vectors any architecture we target has. x86-64 has 256;
 * AArch64 GIC INTIDs are mapped into this space by its arch layer. */
#define INTERRUPT_MAX_VECTORS 256

struct interrupt_slot {
    interrupt_handler_fn fn;
    void *arg;
    const char *name;
    uint64_t count;
};

static struct interrupt_slot g_slots[INTERRUPT_MAX_VECTORS];
static unsigned g_vector_count;

void interrupt_init(void)
{
    g_vector_count = arch_trap_vector_count();
    if (g_vector_count > INTERRUPT_MAX_VECTORS)
        panic("interrupt: architecture reports %u vectors, table holds %u",
              g_vector_count, INTERRUPT_MAX_VECTORS);
    memset(g_slots, 0, sizeof(g_slots));
    kdebug("interrupt: %u vectors", g_vector_count);
}

int interrupt_register(unsigned vector, interrupt_handler_fn fn, void *arg, const char *name)
{
    if (vector >= g_vector_count || fn == NULL)
        return -EINVAL;

    arch_irq_state_t s = arch_irq_save();
    int rc;
    struct interrupt_slot *slot = &g_slots[vector];

    if (slot->fn != NULL) {
        rc = -EBUSY;
    } else {
        slot->arg = arg;
        slot->name = name ? name : "?";
        barrier();
        __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
        rc = 0;
    }

    arch_irq_restore(s);
    return rc;
}

int interrupt_unregister(unsigned vector, interrupt_handler_fn fn)
{
    if (vector >= g_vector_count || fn == NULL)
        return -EINVAL;

    arch_irq_state_t s = arch_irq_save();
    int rc;
    struct interrupt_slot *slot = &g_slots[vector];

    if (slot->fn != fn) {
        rc = -ENOENT;
    } else {
        __atomic_store_n(&slot->fn, NULL, __ATOMIC_RELEASE);
        barrier();
        slot->arg = NULL;
        slot->name = NULL;
        rc = 0;
    }

    arch_irq_restore(s);
    return rc;
}

int interrupt_unregister_vector(unsigned vector)
{
    if (vector >= g_vector_count)
        return -EINVAL;

    arch_irq_state_t s = arch_irq_save();
    struct interrupt_slot *slot = &g_slots[vector];
    int rc;
    if (slot->fn == NULL) {
        rc = -ENOENT;
    } else {
        __atomic_store_n(&slot->fn, NULL, __ATOMIC_RELEASE);
        barrier();
        slot->arg = NULL;
        slot->name = NULL;
        rc = 0;
    }
    arch_irq_restore(s);
    return rc;
}

void interrupt_dispatch(unsigned vector, struct arch_trap_frame *frame)
{
    if (vector >= g_vector_count) {
        panic_frame(frame, "interrupt: vector %u out of range", vector);
    }

    struct interrupt_slot *slot = &g_slots[vector];
    __atomic_fetch_add(&slot->count, 1u, __ATOMIC_RELAXED);

    interrupt_handler_fn fn = __atomic_load_n(&slot->fn, __ATOMIC_ACQUIRE);
    if (fn == NULL) {
        arch_trap_unhandled(vector, frame);
        return;
    }
    fn(vector, frame, slot->arg);
}

uint64_t interrupt_count(unsigned vector)
{
    if (vector >= g_vector_count)
        return 0;
    return g_slots[vector].count;
}

const char *interrupt_handler_name(unsigned vector)
{
    if (vector >= g_vector_count)
        return NULL;
    return g_slots[vector].name;
}
