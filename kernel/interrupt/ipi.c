/*
 * ipi.c - Inter-processor interrupts: kinds to vectors, handlers, and
 * the synchronous cross-CPU function call.
 */

#include <kernel/ipi.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/irqc.h>
#include <arch/mmu.h>

static int g_vectors[IPI_KIND_COUNT];
static uint64_t g_counts[CONFIG_MAX_CPUS][IPI_KIND_COUNT];
static bool g_initialized;

/* One outstanding cross-CPU call at a time. */
static spinlock_t g_call_lock = SPINLOCK_INIT("smp_call");
static smp_call_fn g_call_fn;
static void *g_call_arg;
static volatile uint32_t g_call_done;

static void count(enum ipi_kind kind)
{
    g_counts[arch_cpu_id()][kind]++;
}

static void ipi_reschedule(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector; (void)frame; (void)arg;
    /* Nothing to do: the sender set need_resched under our run-queue
     * lock and the interrupt-return path acts on it. */
    count(IPI_RESCHEDULE);
}

static void ipi_call(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector; (void)frame; (void)arg;
    count(IPI_CALL);
    smp_call_fn fn = g_call_fn;
    void *a = g_call_arg;
    barrier();
    fn(a);
    __atomic_store_n(&g_call_done, 1u, __ATOMIC_RELEASE);
}

static void ipi_tlb_flush(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector; (void)frame; (void)arg;
    count(IPI_TLB_FLUSH);
    arch_mmu_shootdown_ipi_handler();
}

static void ipi_halt(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector; (void)frame; (void)arg;
    count(IPI_HALT);
    __atomic_store_n(&this_cpu()->online, false, __ATOMIC_RELEASE);
    arch_cpu_halt_forever();
}

void ipi_init(void)
{
    static const struct {
        interrupt_handler_fn fn;
        const char *name;
    } handlers[IPI_KIND_COUNT] = {
        [IPI_RESCHEDULE] = { ipi_reschedule, "ipi-reschedule" },
        [IPI_CALL] = { ipi_call, "ipi-call" },
        [IPI_TLB_FLUSH] = { ipi_tlb_flush, "ipi-tlb-flush" },
        [IPI_HALT] = { ipi_halt, "ipi-halt" },
    };

    for (unsigned k = 0; k < IPI_KIND_COUNT; k++) {
        int v = arch_vector_alloc();
        if (v < 0)
            panic("ipi: no vector for kind %u", k);
        int rc = interrupt_register((unsigned)v, handlers[k].fn, NULL, handlers[k].name);
        if (rc)
            panic("ipi: cannot register %s (%d)", handlers[k].name, rc);
        arch_ipi_bind((unsigned)v);   /* so ipi_send takes no lock under the run-queue lock */
        g_vectors[k] = v;
    }
    g_initialized = true;
    kdebug("ipi: vectors %d..%d", g_vectors[0], g_vectors[IPI_KIND_COUNT - 1]);
}

void ipi_send(unsigned cpu, enum ipi_kind kind)
{
    KASSERT(g_initialized && kind < IPI_KIND_COUNT);
    arch_ipi_send(cpu, (unsigned)g_vectors[kind]);
}

void ipi_broadcast_others(enum ipi_kind kind)
{
    KASSERT(g_initialized && kind < IPI_KIND_COUNT);
    arch_ipi_broadcast_others((unsigned)g_vectors[kind]);
}

uint64_t ipi_count(enum ipi_kind kind)
{
    return kind < IPI_KIND_COUNT ? g_counts[arch_cpu_id()][kind] : 0;
}

void smp_call_function_single(unsigned cpu, smp_call_fn fn, void *arg)
{
    KASSERT(fn != NULL);

    if (cpu == arch_cpu_id()) {
        arch_irq_state_t s = arch_irq_save();
        fn(arg);
        arch_irq_restore(s);
        return;
    }
    if (!cpu_online(cpu))
        panic("smp_call_function_single: CPU %u is not online", cpu);
    KASSERT(arch_irq_enabled());

    spin_lock(&g_call_lock); /* preemption off, interrupts on */
    g_call_fn = fn;
    g_call_arg = arg;
    __atomic_store_n(&g_call_done, 0u, __ATOMIC_RELEASE);
    ipi_send(cpu, IPI_CALL);

    uint64_t deadline = clock_now_ns() + 1000000000ULL;
    while (!__atomic_load_n(&g_call_done, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() > deadline) {
            spin_unlock(&g_call_lock);
            panic("smp_call_function_single: CPU %u did not respond", cpu);
        }
        arch_cpu_relax();
    }
    spin_unlock(&g_call_lock);
}
