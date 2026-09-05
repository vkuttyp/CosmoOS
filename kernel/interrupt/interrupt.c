/*
 * interrupt.c - Vector-to-handler dispatch table.
 *
 * One slot per vector. A slot publishes a pointer to an immutable
 * {fn, arg, name} record: dispatch loads that pointer once (acquire) and
 * uses the record, so a handler never runs with another registration's
 * argument. Registration writes a record and publishes it (release).
 *
 * Unregistration clears the pointer; a CPU already inside the handler
 * keeps its record until it returns. That is the read-side section of
 * docs/kernel/quiesce/: interrupt handlers run with preemption disabled,
 * so a grace period (synchronize_quiesce) after the clear proves no CPU
 * is still executing the handler. The _sync variants wait for it and
 * are the only way to free `arg` safely; the plain variants exist for
 * callers holding a spinlock, who must call synchronize_irq() before
 * touching `arg`.
 *
 * Each slot has two records. Registration uses the one not published
 * last, so the record a racing dispatcher may still hold is rewritten
 * only by the second re-registration after its unregister. The _sync
 * variants make that ordering an invariant (the record is idle before
 * they return); a plain unregister followed by a plain register on the
 * same vector without a grace period is a caller bug the design.md
 * lists, and cannot happen through the IRQ layer, which always waits.
 */

#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/quiesce.h>
#include <kernel/string.h>

#include <arch/irq.h>
#include <arch/trap.h>

/* Upper bound on vectors any architecture we target has. x86-64 has 256;
 * AArch64 GIC INTIDs are mapped into this space by its arch layer. */
#define INTERRUPT_MAX_VECTORS 1344   /* x86-64 uses 256; AArch64 1020 INTIDs + software vectors */

struct interrupt_record {
    interrupt_handler_fn fn;
    void *arg;
    const char *name;
};

struct interrupt_slot {
    struct interrupt_record *cur;      /* published registration, NULL when none */
    struct interrupt_record recs[2];
    unsigned next_rec;                 /* index the next registration writes */
    uint64_t count;
};

static struct interrupt_slot g_slots[INTERRUPT_MAX_VECTORS];
static unsigned g_vector_count;

void quiesce_count_irq_sync(void);   /* quiesce.c statistics */

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

    if (slot->cur != NULL) {
        rc = -EBUSY;
    } else {
        struct interrupt_record *r = &slot->recs[slot->next_rec & 1u];
        slot->next_rec++;
        r->fn = fn;
        r->arg = arg;
        r->name = name ? name : "?";
        /* Release: the record's fields are complete before any CPU can
         * load the pointer (dispatch pairs with an acquire load). */
        __atomic_store_n(&slot->cur, r, __ATOMIC_RELEASE);
        rc = 0;
    }

    arch_irq_restore(s);
    return rc;
}

/* Clear the publication. With `fn` NULL any handler qualifies. */
static int unpublish(unsigned vector, interrupt_handler_fn fn)
{
    if (vector >= g_vector_count)
        return -EINVAL;

    arch_irq_state_t s = arch_irq_save();
    struct interrupt_slot *slot = &g_slots[vector];
    struct interrupt_record *r = slot->cur;
    int rc;
    if (r == NULL || (fn != NULL && r->fn != fn)) {
        rc = -ENOENT;
    } else {
        /* Release is not needed for correctness here (nothing is
         * published); relaxed would do. Release keeps every store to
         * the table monotone for tools that check ordering. */
        __atomic_store_n(&slot->cur, NULL, __ATOMIC_RELEASE);
        rc = 0;
    }
    arch_irq_restore(s);
    return rc;
}

int interrupt_unregister(unsigned vector, interrupt_handler_fn fn)
{
    if (fn == NULL)
        return -EINVAL;
    return unpublish(vector, fn);
}

int interrupt_unregister_vector(unsigned vector)
{
    return unpublish(vector, NULL);
}

void synchronize_irq(unsigned vector)
{
    (void)vector;   /* one grace period covers every vector; the argument documents intent */
    synchronize_quiesce();
    quiesce_count_irq_sync();
}

int interrupt_unregister_sync(unsigned vector, interrupt_handler_fn fn)
{
    int rc = interrupt_unregister(vector, fn);
    if (rc == 0)
        synchronize_irq(vector);
    return rc;
}

int interrupt_unregister_vector_sync(unsigned vector)
{
    int rc = interrupt_unregister_vector(vector);
    if (rc == 0)
        synchronize_irq(vector);
    return rc;
}

void interrupt_dispatch(unsigned vector, struct arch_trap_frame *frame)
{
    if (vector >= g_vector_count) {
        panic_frame(frame, "interrupt: vector %u out of range", vector);
    }

    struct interrupt_slot *slot = &g_slots[vector];
    __atomic_fetch_add(&slot->count, 1u, __ATOMIC_RELAXED);

    /* Acquire pairs with the release in interrupt_register: the record's
     * fields are visible. One load: the record is used as a unit. */
    struct interrupt_record *r = __atomic_load_n(&slot->cur, __ATOMIC_ACQUIRE);
    if (r == NULL) {
        arch_trap_unhandled(vector, frame);
        return;
    }
    r->fn(vector, frame, r->arg);
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
    struct interrupt_record *r = __atomic_load_n(&g_slots[vector].cur, __ATOMIC_ACQUIRE);
    return r ? r->name : NULL;
}

/* Module ABI exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(synchronize_irq);
EXPORT_SYMBOL(interrupt_unregister_sync);
