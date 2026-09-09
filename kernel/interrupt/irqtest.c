/*
 * irqtest.c - Self-tests for interrupt routing itself, as opposed to the
 * subsystems that use it (kernel/interrupt/irq.c).
 *
 * `irq-route` in schedtest.c proves a line can be requested, delivered,
 * masked and released. What it cannot prove is *where* the interrupt
 * arrives: every driver in this tree asks for CPU 0, so a controller
 * that ignored the CPU argument entirely would pass every test. That is
 * exactly the code GICv3 replaces -- GICv2's eight-bit target mask
 * becomes a 64-bit affinity in GICD_IROUTER -- so the property needs a
 * test that names a CPU and checks the handler ran there.
 */

#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <kernel/selftest.h>
#include <kernel/smp.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/testhooks.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            release();                                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

static unsigned g_aff_hits;          /* handler and test: atomics, not volatile */
static volatile unsigned g_aff_cpu;
static irq_t g_aff_gsi;
static bool g_aff_held;

static void aff_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
    g_aff_cpu = arch_cpu_id();
    __atomic_fetch_add(&g_aff_hits, 1u, __ATOMIC_RELEASE);
}

/* A CHECK anywhere in the loop leaves the line requested; give the
 * failure path one place to undo it. */
static void release(void)
{
    if (g_aff_held) {
        irq_disable(g_aff_gsi);
        irq_release(g_aff_gsi);
        g_aff_held = false;
    }
}

bool selftest_irq_affinity(const char **reason)
{
    int spare = arch_test_irq_spare_gsi();
    if (spare < 0) {
        kinfo("selftest: irq-affinity: no line this controller can raise by hand; skipping");
        return true;
    }
    g_aff_gsi = (irq_t)spare;

    unsigned delivered = 0;
    for (unsigned c = 0; c < cpu_count(); c++) {
        if (!cpu_online(c))
            continue;
        int rc = irq_request(g_aff_gsi, aff_handler, NULL, "selftest-affinity", IRQ_TRIGGER_EDGE, c);
        CHECK(rc == 0);
        g_aff_held = true;
        __atomic_store_n(&g_aff_hits, 0u, __ATOMIC_RELEASE);
        g_aff_cpu = ~0u;
        CHECK(irq_enable(g_aff_gsi) == 0);

        arch_test_irq_raise(g_aff_gsi);
        uint64_t deadline = clock_now_ns() + 200000000ULL;
        while (__atomic_load_n(&g_aff_hits, __ATOMIC_ACQUIRE) == 0) {
            CHECK(clock_now_ns() < deadline);
            thread_sleep_ms(1);
        }
        /* The handler ran on the CPU the route named, not merely on some
         * CPU: this is the whole test. */
        CHECK(g_aff_cpu == c);
        delivered++;

        CHECK(irq_disable(g_aff_gsi) == 0);
        CHECK(irq_release(g_aff_gsi) == 0);
        g_aff_held = false;
    }
    CHECK(delivered > 0);
    kinfo("selftest: irq-affinity: GSI %u delivered to each of %u CPUs in turn", (unsigned)g_aff_gsi,
          delivered);
    return true;
}

/* --- an MSI must not take a line a device is already wired to ---
 *
 * A GICv2m frame advertises a range of SPIs, and firmware is free to
 * wire a device to a line inside it: on QEMU's `virt` the SMMU's event
 * and global-error interrupts sit in the frame's range. The frame's
 * allocator sees only its own bitmap, so without a check it hands the
 * SMMU's line to the next device that asks for an MSI, and the SMMU
 * stops being interrupted -- silently, because the MSI works.
 *
 * It takes about twenty-seven MSIs on QEMU's virt to reach the first
 * such line, which is why nothing saw this until sixteen CPUs made
 * that many queues. The test does not wait for the machine to get
 * there: it binds the line the allocator would hand out *next*.
 */

static unsigned g_wired_hits;

static void wired_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
    __atomic_fetch_add(&g_wired_hits, 1u, __ATOMIC_RELEASE);
}

static void msi_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
}

bool selftest_irq_msi_overlap(const char **reason)
{
    int wired = arch_test_msi_overlap_gsi();
    if (wired < 0) {
        kinfo("selftest: irq-msi-overlap: MSIs do not come out of the GSI space here; skipping");
        return true;
    }
    int rc = irq_request((irq_t)wired, wired_handler, NULL, "selftest-wired", IRQ_TRIGGER_EDGE,
                         arch_cpu_id());
    if (rc != 0) {
        *reason = "the line the MSI allocator would offer next could not be requested";
        return false;
    }

    struct irq_msi_msg msg = { 0, 0 };
    int vector = irq_request_msi(msi_handler, NULL, "selftest-msi", arch_cpu_id(), 0, &msg);
    bool took_it = vector >= 0 && msg.data == (uint32_t)wired;

    /* The wired line still belongs to its handler: raise it and see. */
    __atomic_store_n(&g_wired_hits, 0u, __ATOMIC_RELEASE);
    bool delivered = false;
    if (irq_enable((irq_t)wired) == 0) {
        arch_test_irq_raise((irq_t)wired);
        uint64_t deadline = clock_now_ns() + 200000000ULL;
        while (clock_now_ns() < deadline) {
            if (__atomic_load_n(&g_wired_hits, __ATOMIC_ACQUIRE) != 0) {
                delivered = true;
                break;
            }
            thread_sleep_ms(1);
        }
        irq_disable((irq_t)wired);
    }

    if (vector >= 0)
        irq_release_msi(vector);
    irq_release((irq_t)wired);

    if (vector < 0) {
        kinfo("selftest: irq-msi-overlap: no MSI left to allocate; the wired line was %s", 
              delivered ? "delivered" : "lost");
        return delivered;
    }
    if (took_it) {
        *reason = "the MSI allocator handed out a line a device is wired to";
        return false;
    }
    if (!delivered) {
        *reason = "the wired line stopped being delivered after an MSI was allocated";
        return false;
    }
    kinfo("selftest: irq-msi-overlap: GSI %d stayed wired; the MSI took %u instead", wired, msg.data);
    return true;
}
