/*
 * quiesce_floor.c - The C under kernel/core/quiesce.nova (roadmap 2.0 SA3).
 *
 * Quiescence is Nova (quiesce.nova, generated into quiesce.c and
 * kernel/include/kernel/quiesce_types.h). What stays here is C that Nova
 * cannot express yet or that exists only in a debug build:
 *   - the statics of C types: the epoch state (quiesce_core.h's) and the two
 *     wait queues, which WAITQUEUE_INIT builds from their own names, with an
 *     accessor each;
 *   - the formatted messages of panic, kwarn and kinfo;
 *   - what exists only under CONFIG_DEBUG: the read-section depth
 *     counting, the ten-second stall panic, and the test hooks that hand
 *     back one grace period's kicks and timeouts;
 *   - the module exports.
 * It also checks the constants quiesce.nova copies.
 *
 * First drafted by Nova's c2nova (docs/tooling.md §9.4), then finished by
 * hand.
 */

#include <arch/cpu.h>
#include <kernel/ipi.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/quiesce.h>
#include <kernel/quiesce_core.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

STATIC_ASSERT(QUIESCE_MAX_CPUS >= CONFIG_MAX_CPUS, "quiesce state covers every CPU");
_Static_assert(IPI_QUIESCE_KICK == 5 && NS_PER_SEC == 1000000000ull && TICK_NS == 4000000ull && CONFIG_MAX_CPUS == 64u &&
               SCHED_PRIO_DEFAULT == 32, "quiesce.nova's constants");

/* Declared for quiesce.nova only; not part of quiesce.h's interface. */
struct quiesce_state *quiesce_floor_g_state(void);
struct waitqueue *quiesce_floor_g_worker_wq(void);
struct waitqueue *quiesce_floor_g_gp_wq(void);
void quiesce_floor_panic_3(void);
void quiesce_floor_kwarn_4(unsigned long long a0, unsigned long long a1, unsigned long long a2);
void quiesce_floor_stall_check(uint64_t waited, uint64_t pending);
void quiesce_floor_assert_7(void);
void quiesce_floor_panic_8(void *a0);
void quiesce_floor_assert_17(void);
void quiesce_floor_panic_18(void);
void quiesce_floor_kinfo_19(unsigned int a0);

static struct quiesce_state g_state;
static struct waitqueue g_worker_wq = WAITQUEUE_INIT(g_worker_wq);
/* Waiters for a grace period. Woken by a quiescent point taken in a
 * context that holds nothing -- the trap returns and the idle loop, not
 * the scheduler's own publishes (invariant Q-W). */
static struct waitqueue g_gp_wq = WAITQUEUE_INIT(g_gp_wq);

struct quiesce_state *quiesce_floor_g_state(void)
{
    return &g_state;
}

struct waitqueue *quiesce_floor_g_worker_wq(void)
{
    return &g_worker_wq;
}

struct waitqueue *quiesce_floor_g_gp_wq(void)
{
    return &g_gp_wq;
}

void quiesce_floor_panic_3(void)
{
    panic("synchronize_quiesce in interrupt context");
}

void quiesce_floor_kwarn_4(unsigned long long a0, unsigned long long a1, unsigned long long a2)
{
    kwarn("quiesce: grace period %llu waiting %llu ms for CPU mask 0x%llx", a0, a1, a2);
}

/* A debug build panics when a grace period has waited ten seconds; a
 * release build waits on. */
void quiesce_floor_stall_check(uint64_t waited, uint64_t pending)
{
#if CONFIG_DEBUG
    if (waited > 10 * NS_PER_SEC)
        panic("quiesce: CPU mask 0x%llx has not reached a quiescent state in 10 s", (unsigned long long)pending);
#else
    (void)waited;
    (void)pending;
#endif
}

void quiesce_floor_assert_7(void)
{
    panic("assertion failed: %s at %s:%d (%s)", "fn != NULL", "quiesce.nova", 0, "call_quiesce");
}

void quiesce_floor_panic_8(void *a0)
{
    panic("call_quiesce: head %p submitted twice before its callback ran", a0);
}

void quiesce_floor_assert_17(void)
{
    panic("assertion failed: %s at %s:%d (%s)", "!g_ready", "quiesce.nova", 0, "quiesce_init");
}

void quiesce_floor_panic_18(void)
{
    panic("quiesce: cannot create the worker thread");
}

void quiesce_floor_kinfo_19(unsigned int a0)
{
    kinfo("quiesce: epoch-based reclamation ready (%u CPU slots)", a0);
}

/* --- debug builds only ----------------------------------------------------- */

void quiesce_read_lock_debug(void)
{
#if CONFIG_DEBUG
    g_state.cpus[arch_cpu_id()].depth++;
#endif
}

void quiesce_read_unlock_debug(void)
{
#if CONFIG_DEBUG
    struct quiesce_cpu *c = &g_state.cpus[arch_cpu_id()];
    if (c->depth == 0)
        panic("quiesce_read_unlock without a matching lock on CPU %u", arch_cpu_id());
    c->depth--;
#endif
}

#if CONFIG_DEBUG
/*
 * The same wait, handing back the kicks *this call* sent. Returned rather
 * than stored anywhere: a value on the caller's stack belongs to the caller
 * (docs/audit/next-subsystem-lifetime-windows.md).
 */
unsigned quiesce_test_sync_kicks(void)
{
    return quiesce_sync_counting(NULL, (uintptr_t)__builtin_return_address(0));
}

/* One grace period, reporting how many of ITS blocks reached a deadline.
 * Zero is the claim invariant Q-W makes. */
unsigned quiesce_test_sync_timeouts(void)
{
    unsigned timeouts = 0;
    (void)quiesce_sync_counting(&timeouts, (uintptr_t)__builtin_return_address(0));
    return timeouts;
}
#endif

/* Module ABI exports (docs/kernel/module/api.md). */
EXPORT_SYMBOL(synchronize_quiesce);
EXPORT_SYMBOL(call_quiesce);
EXPORT_SYMBOL(quiesce_read_lock_debug);
EXPORT_SYMBOL(quiesce_read_unlock_debug);
