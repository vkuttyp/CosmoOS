/*
 * selftest.c - Boot-time self-tests for the Phase 0/1 kernel.
 *
 * These exercise exactly the subsystems that exist: the formatter, string
 * primitives, boot data validation, interrupt enable state, and the trap
 * path end to end (a real #BP taken through the IDT, the stub, the
 * dispatcher, and a registered handler). Every test restores the state
 * it changed.
 */

#include <kernel/bootinfo.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/kernel.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/timer.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <arch/cpu.h>
#include <kernel/string.h>

#include <arch/irq.h>
#include <arch/testhooks.h>
#include <arch/trap.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define STR_(x) #x
#define STR(x)  STR_(x)

typedef bool (*selftest_fn)(const char **reason);

struct selftest {
    const char *name;
    selftest_fn fn;
};

/*
 * A test's budget is how long it may take, and the hang watchdog's period
 * while it runs: the runner arms the watchdog before each test at that
 * test's budget, and prints every budget in one line the boot harness reads
 * (docs/verification/design.md, "Per-test timing"). One number, one place.
 * A test not named below has the default; a name below that no test has is
 * refused when the run starts, so a typo cannot quietly mean the default.
 */
#define SELFTEST_BUDGET_DEFAULT_MS 8000u

static const struct selftest_budget {
    const char *name;
    unsigned ms;
} budgets[] = {
    /* The entire user-mode suite behind one line (every fs, net, proc,
     * fpu, trap, priv and svc check init makes, and a spawn per tool), so
     * it grows whenever userland gains a test. It stood at 7129 ms of 8000
     * on CI before docs/audit/next-subsystem-fsctl.md gave it its own
     * budget; the harness names its slowest section. */
    { "process-user",   20000 },
    /* net-nicbench had 20 s here from 2026-10-04 to 2026-10-06, when its
     * UDP phase was a count (10,000 sends) whose time was the host's
     * per-datagram cost times 10,000 -- 21.5 s on a slow afternoon. The
     * phase is a 500 ms window now and every other phase of the test is
     * bounded by its own wait, so it is back on the default
     * (docs/audit/2026-10-06-flake-triage-report.md). */
    /* One line is not one test: it mounts and structurally checks every
     * prefix of a recorded write stream, 410 filesystem images, and grows
     * whenever a transaction writes another block -- by design, twice so
     * far (docs/audit/next-subsystem-snap-deadlist.md, -orphan.md). About
     * 13 s here and 20 s on CI. 40 s is twice CI's figure and still far
     * under the 240 s boot timeout: a budget notices a test that stopped
     * terminating, it does not ration one that got more thorough. Under
     * the old single 8 s arming this test fired the watchdog in every
     * debug boot and left it spent for every test after it
     * (docs/audit/next-subsystem-watchdog-spent.md). */
    { "cosmofs-replay", 40000 },
};

/* --- formatter --- */

static bool fmt_eq(const char *expect, const char *fmt, ...) __printf(2, 3);

static bool fmt_eq(const char *expect, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return n == (int)strlen(expect) && strcmp(buf, expect) == 0;
}

static bool test_printf(const char **reason)
{
    CHECK(fmt_eq("hello", "%s", "hello"));
    CHECK(fmt_eq("(null)", "%s", (const char *)NULL));
    CHECK(fmt_eq("-42", "%d", -42));
    CHECK(fmt_eq("42", "%u", 42u));
    CHECK(fmt_eq("ff", "%x", 255u));
    CHECK(fmt_eq("FF", "%X", 255u));
    CHECK(fmt_eq("0xff", "%#x", 255u));
    CHECK(fmt_eq("00ff", "%04x", 255u));
    CHECK(fmt_eq("  42", "%4d", 42));
    CHECK(fmt_eq("42  ", "%-4d", 42));
    CHECK(fmt_eq("+42", "%+d", 42));
    CHECK(fmt_eq("0042", "%.4d", 42));
    CHECK(fmt_eq("", "%.0d", 0));
    CHECK(fmt_eq("18446744073709551615", "%llu", 18446744073709551615ULL));
    CHECK(fmt_eq("-9223372036854775808", "%lld", (long long)(-9223372036854775807LL - 1)));
    CHECK(fmt_eq("0x0000000000001000", "%p", (void *)0x1000));
    CHECK(fmt_eq("abc", "%.3s", "abcdef"));
    CHECK(fmt_eq("  x", "%3c", 'x'));
    CHECK(fmt_eq("100%", "%d%%", 100));
    CHECK(fmt_eq("777", "%o", 0777u));
    CHECK(fmt_eq("12", "%zu", (size_t)12));

    /* Truncation: return value is the full length, buffer is cut. */
    char small[4];
    int n = ksnprintf(small, sizeof(small), "%s", "toolong");
    CHECK(n == 7);
    CHECK(strcmp(small, "too") == 0);

    /* Zero-size buffer must not write. */
    n = ksnprintf(NULL, 0, "%d", 12345);
    CHECK(n == 5);
    return true;
}

/* --- strings --- */

static bool test_string(const char **reason)
{
    char buf[16];

    memset(buf, 'x', sizeof(buf));
    CHECK(buf[0] == 'x' && buf[15] == 'x');

    memcpy(buf, "abcdef", 7);
    CHECK(strcmp(buf, "abcdef") == 0);
    CHECK(strlen(buf) == 6);
    CHECK(strnlen(buf, 3) == 3);

    /* Overlapping move both directions. */
    memmove(buf + 2, buf, 4);
    CHECK(memcmp(buf, "ababcd", 6) == 0);
    memcpy(buf, "abcdef", 7);
    memmove(buf, buf + 2, 4);
    CHECK(memcmp(buf, "cdef", 4) == 0);

    CHECK(strcmp("a", "b") < 0);
    CHECK(strcmp("b", "a") > 0);
    CHECK(strncmp("abc", "abd", 2) == 0);
    CHECK(strncmp("abc", "abd", 3) < 0);

    CHECK(strlcpy(buf, "0123456789ABCDEFGHIJ", sizeof(buf)) == 20);
    CHECK(strlen(buf) == 15);
    CHECK(strlcpy(buf, "hi", sizeof(buf)) == 2);
    CHECK(strcmp(buf, "hi") == 0);
    return true;
}

/* --- boot information --- */

static bool test_bootinfo(const char **reason)
{
    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);
    const struct cosmoboot_info *info = bootinfo_get();

    CHECK(n > 0);
    CHECK(bootinfo_usable_bytes() > 0);
    CHECK(info->kernel_size > 0);
    CHECK(info->kernel_virt_base == (uint64_t)(uintptr_t)__kernel_start);

    /* No two entries overlap. O(n^2) is fine for a few hundred entries. */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            bool overlap = map[i].base < map[j].base + map[j].length &&
                           map[j].base < map[i].base + map[i].length;
            CHECK(!overlap);
        }
    }

    /* The kernel image must be described as such (or as loader memory
     * on firmware that rejected custom types) and never as usable. */
    bool kernel_seen = false;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t end = map[i].base + map[i].length;
        if (info->kernel_phys_base >= map[i].base && info->kernel_phys_base < end) {
            kernel_seen = true;
            CHECK(map[i].type != COSMOBOOT_MEM_USABLE);
        }
    }
    CHECK(kernel_seen);
    return true;
}

/* --- interrupt enable state --- */

static bool test_irq_state(const char **reason)
{
    bool was = arch_irq_enabled();

    arch_irq_state_t s = arch_irq_save();
    CHECK(!arch_irq_enabled());
    arch_irq_state_t s2 = arch_irq_save();
    CHECK(!arch_irq_enabled());
    arch_irq_restore(s2);
    CHECK(!arch_irq_enabled());
    arch_irq_restore(s);
    CHECK(arch_irq_enabled() == was);
    return true;
}

/* --- trap path --- */

struct bp_state {
    unsigned hits;
    uintptr_t pc;
    unsigned vector;
};

static void bp_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    struct bp_state *st = arg;
    st->hits++;
    st->pc = arch_trap_frame_pc(frame);
    st->vector = vector;
}

static void other_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
}

static bool test_breakpoint_trap(const char **reason)
{
    int vec = arch_trap_vector(ARCH_TRAP_BREAKPOINT);
    CHECK(vec >= 0);
    CHECK(arch_trap_is_exception((unsigned)vec));

    struct bp_state st = { 0 };
    uint64_t before = interrupt_count((unsigned)vec);

    CHECK(interrupt_register((unsigned)vec, bp_handler, &st, "selftest-bp") == 0);
    CHECK(interrupt_register((unsigned)vec, other_handler, NULL, "dup") == -EBUSY);
    CHECK(interrupt_unregister((unsigned)vec, other_handler) == -ENOENT);
    CHECK(strcmp(interrupt_handler_name((unsigned)vec), "selftest-bp") == 0);

    arch_debug_break();

    CHECK(st.hits == 1);
    CHECK(st.vector == (unsigned)vec);
    CHECK(kernel_text_contains(st.pc));
    CHECK(interrupt_count((unsigned)vec) == before + 1);

    /* Interrupt state must survive the trap unchanged. */
    CHECK(arch_irq_enabled());

    CHECK(interrupt_unregister((unsigned)vec, bp_handler) == 0);
    CHECK(interrupt_handler_name((unsigned)vec) == NULL);

    /* Bad arguments. */
    CHECK(interrupt_register(arch_trap_vector_count(), bp_handler, NULL, "x") == -EINVAL);
    CHECK(interrupt_register(0, NULL, NULL, "x") == -EINVAL);
    return true;
}

/* The exception paths that must be safe at any instruction (x86-64: NMI-class
 * vectors on their own stacks, recovering the per-CPU pointer from the MSR). */
static bool test_trap_paranoid(const char **reason)
{
    const char *why = NULL;
    if (!arch_test_paranoid_entry(&why)) {
        *reason = why ? why : "paranoid entry check failed";
        return false;
    }
    return true;
}

/* The asynchronous-error classifier, over encodings no CI CPU produces
 * (invariant I-ARCH-16). The table is architecture-shaped and lives with the
 * classifier; the row that fails names itself. */
static bool test_trap_async_class(const char **reason)
{
    const char *why = NULL;
    if (!arch_test_async_class(&why)) {
        *reason = why ? why : "async-error classifier check failed";
        return false;
    }
    return true;
}

/* And one real asynchronous error of the only survivable class, where the
 * hardware can produce one (invariant I-ARCH-16). */
static bool test_trap_async_inject(const char **reason)
{
    const char *why = NULL;
    if (!arch_test_async_inject(&why)) {
        *reason = why ? why : "async-error injection check failed";
        return false;
    }
    return true;
}

/* Vector/x87 register state stays with the thread that owns it across switches. */
static bool test_fpu_switch(const char **reason)
{
    const char *why = NULL;
    if (!arch_test_fpu_switch(&why)) {
        *reason = why ? why : "register state leaked between threads";
        return false;
    }
    return true;
}

/*
 * What owning FP/SIMD state costs a context switch (reports only;
 * docs/kernel/arch/design.md, "FPU and SIMD state"). Two threads pinned
 * to this CPU hand the processor back and forth; the pair is timed with
 * state and without, and the difference is the save and the restore.
 *
 * The number decides a policy: eager switching pays it on every switch
 * between owners, which after this unit is every user thread. Lazy
 * switching would pay a trap instead, on the first FP instruction a
 * thread executes -- worth having only if this figure is large.
 */
#define FPU_BENCH_SWITCHES 2000u

struct fpu_bench {
    bool own;
    unsigned rounds;
};

static void fpu_bench_thread(void *arg)
{
    struct fpu_bench *b = arg;
    if (b->own)
        (void)arch_fpu_alloc(thread_current());
    for (unsigned i = 0; i < b->rounds; i++)
        sched_yield();
    thread_exit(0);
}

static uint64_t fpu_bench_pair(bool own)
{
    struct fpu_bench a = { .own = own, .rounds = FPU_BENCH_SWITCHES };
    struct fpu_bench b = { .own = own, .rounds = FPU_BENCH_SWITCHES };
    cpumask_t here = CPUMASK_OF(raw_cpu_id());   /* one CPU for the pair to share; the caller need not stay */
    uint64_t t0 = clock_now_ns();
    struct thread *ta = thread_create_on(fpu_bench_thread, &a, "fpu-bench-a", SCHED_PRIO_DEFAULT, here);
    struct thread *tb = thread_create_on(fpu_bench_thread, &b, "fpu-bench-b", SCHED_PRIO_DEFAULT, here);
    if (ta == NULL || tb == NULL) {
        if (ta)
            thread_join(ta);
        if (tb)
            thread_join(tb);
        return 0;
    }
    thread_join(ta);
    thread_join(tb);
    return clock_since_ns(t0);
}

static bool test_fpu_bench(const char **reason)
{
    (void)reason;
    if (arch_fpu_state_size() == 0) {
        kinfo("selftest: fpu-bench: no FP/SIMD state on this architecture; skipped");
        return true;
    }
    uint64_t bare = fpu_bench_pair(false);
    uint64_t owned = fpu_bench_pair(true);
    if (bare == 0 || owned == 0) {
        kinfo("selftest: fpu-bench: could not create the pair; skipped");
        return true;
    }
    uint64_t switches = 2ull * FPU_BENCH_SWITCHES;
    kinfo("selftest: fpu-bench: %llu-byte state; %llu ns a switch with it, %llu without, "
          "%lld ns of save and restore",
          (unsigned long long)arch_fpu_state_size(), (unsigned long long)(owned / switches),
          (unsigned long long)(bare / switches),
          (long long)((int64_t)owned - (int64_t)bare) / (int64_t)switches);
    return true;
}

/* --- the runner's hang watchdog (docs/audit/next-subsystem-watchdog-spent.md) --- */

static unsigned selftest_budget_ms(const char *name);

/* Fires a quiet arming once and returns with it still fired. The next test
 * checks that the runner, not this test, cleared that. */
static bool selftest_watchdog_spend(const char **reason)
{
    uint64_t before = sched_watchdog_fire_count();
    sched_watchdog_arm_quiet(50ull * 1000 * 1000);
    thread_sleep_ms(150);   /* three periods: one firing, not three */
    uint64_t fired = sched_watchdog_fire_count() - before;
    uint64_t period;
    bool is_fired;
    sched_watchdog_state(&period, &is_fired);
    if (fired != 1 || !is_fired) {
        kerror("selftest: watchdog-spend: %llu firings in three periods, fired %d", (unsigned long long)fired,
               (int)is_fired);
        *reason = fired == 0 ? "a quiet arming did not fire" : "one arming fired more than once";
        return false;
    }
    return true;   /* left fired, on purpose */
}

/* The first thing it does: read the arming the runner gave it. */
static bool selftest_watchdog_rearm(const char **reason)
{
    uint64_t period;
    bool fired;
    sched_watchdog_state(&period, &fired);
    if (fired || period != (uint64_t)selftest_budget_ms("watchdog-rearm") * 1000 * 1000) {
        kerror("selftest: watchdog-rearm: entered with the watchdog %s, period %llu ms", fired ? "fired" : "armed",
               (unsigned long long)(period / 1000000));
        *reason = fired ? "the runner did not re-arm the watchdog: the previous test's firing carried over"
                        : "the runner armed the watchdog at another period than this test's budget";
        return false;
    }
    kinfo("selftest: watchdog-rearm: armed afresh at %llu ms after the previous test fired it",
          (unsigned long long)(period / 1000000));
    return true;
}

static const struct selftest tests[] = {
    { "printf",          test_printf },
    { "string",          test_string },
    { "bootinfo",        test_bootinfo },
    { "irq-state",       test_irq_state },
    { "breakpoint-trap", test_breakpoint_trap },
    { "trap-paranoid",   test_trap_paranoid },
    { "trap-async-class", test_trap_async_class },
    { "trap-async-inject", test_trap_async_inject },
    { "pmm",             selftest_pmm },
    { "vmm",             selftest_vmm },
    { "user-vmm",        selftest_user_vmm },
    { "vm-teardown-absent", selftest_vm_teardown_absent },
    { "vm-replace",      selftest_vm_replace },
    { "rlimit",          selftest_rlimit },
    { "uaccess",         selftest_uaccess },
    { "uaccess-guard",   selftest_uaccess_guard },
    { "kmalloc",         selftest_kmalloc },
    { "asid-alloc",      selftest_asid_alloc },
    { "asid-isolation",  selftest_asid_isolation },
    { "asid-rollover",   selftest_asid_rollover },
    { "asid-paranoid",   selftest_asid_paranoid },
    { "asid-destroy-reuse", selftest_asid_destroy_reuse },
    { "asid-race",       selftest_asid_race },
    { "asid-quiet",      selftest_asid_quiet },
    { "acpi",            selftest_acpi },
    { "timer",           selftest_timer },
    { "irq-route",       selftest_irq_route },
    { "irq-affinity",    selftest_irq_affinity },
    { "irq-msi-overlap", selftest_irq_msi_overlap },
    { "irq-msi-devid",   selftest_irq_msi_devid },
    { "thread",          selftest_thread },
    { "yield",           selftest_yield },
    { "preempt",         selftest_preempt },
    { "preempt-wake",    selftest_preempt_wake },
    { "preempt-wake-direct", selftest_preempt_wake_direct },
    { "preempt-wake-locked", selftest_preempt_wake_locked },
    { "prio-inversion",  selftest_prio_inversion },
    { "irqrestore-bench", selftest_irqrestore_bench },
    { "sched-restore-loop", selftest_sched_restore_loop },
    { "sleep",           selftest_sleep },
    { "wait-timeout",    selftest_wait_timeout },
    { "mutex",           selftest_mutex },
    { "mutex-wake-bench", selftest_mutex_wake_bench },
    { "semaphore",       selftest_semaphore },
    { "completion",      selftest_completion },
    { "completion-race", selftest_completion_race },
    { "completion-timeout", selftest_completion_timeout },
    { "waitqueue",       selftest_waitqueue },
    { "smp-online",      selftest_smp_online },
    { "smp-affinity",    selftest_smp_affinity },
    { "smp-parallel",    selftest_smp_parallel },
    { "smp-call",        selftest_smp_call },
    { "smp-shootdown",   selftest_smp_shootdown },
    { "smp-wake",        selftest_smp_wake },
    { "smp-ticks",       selftest_smp_ticks },
    { "smp-mutex",       selftest_smp_mutex },
    { "smp-ipi-storm",   selftest_smp_ipi_storm },
    { "lockup-sample",   selftest_lockup_sample },
    { "lockup-sample-irqoff", selftest_lockup_sample_irqoff },
    { "lockup-sample-busy", selftest_lockup_sample_busy },
    { "lockup-soft",     selftest_lockup_soft },
    { "lockup-hard",     selftest_lockup_hard },
    { "lockup-quiet",    selftest_lockup_quiet },
    { "lockup-tick-bench", selftest_lockup_tick_bench },
    { "quiesce-straggler", selftest_quiesce_straggler },
    { "quiesce-wake",    selftest_quiesce_wake },
    { "quiesce-straggler-system", selftest_quiesce_straggler_system },
    { "quiesce-straggler-idle", selftest_quiesce_straggler_idle },
    { "quiesce-kick-population", selftest_quiesce_kick_population },
    { "quiesce-kick-spinner", selftest_quiesce_kick_spinner },
    { "quiesce-grace",   selftest_quiesce_grace },
    { "quiesce-call",    selftest_quiesce_call },
    { "irq-sync",        selftest_irq_sync },
    { "irq-writers",     selftest_irq_writers },
    { "irq-unhandled",   selftest_irq_unhandled },
    { "timer-cancel-sync", selftest_timer_cancel_sync },
    { "quiesce-stress",  selftest_quiesce_stress },
    /*
     * After the quiesce block, deliberately. This test creates and
     * joins kernel threads, and ANY test that does so before
     * quiesce-kick-spinner makes it fail its straggler-IPI check --
     * six no-op threads with no VM work at all reproduce it, so the
     * sensitivity is that test's and not this one's. Moving this one
     * keeps the suite green without pretending the fragility is
     * fixed; it is written up in docs/testing/flakes.md.
     */
    { "vm-replace-race", selftest_vm_replace_race },
    /* With the memory tests, not with the process tests: its claims
     * are counts of faults and of free pages, so it wants the machine
     * as quiet as the rest of this group has it. */
    { "vm-anon-fault-race", selftest_vm_anon_fault_race },
    { "lockdep-order",   selftest_lockdep_order },
    { "lockdep-recursion", selftest_lockdep_recursion },
    { "lockdep-irq",     selftest_lockdep_irq },
    { "lockdep-sleep",   selftest_lockdep_sleep },
    { "lockdep-mutex",   selftest_lockdep_mutex },
    { "lockdep-contention", selftest_lockdep_contention },
    { "lockdep-callback", selftest_lockdep_callback },
    { "lockdep-irq-pairing", selftest_lockdep_irq_pairing },
    { "lockdep-completion", selftest_lockdep_completion },
    { "lockdep-bench", selftest_lockdep_bench },
    { "lockdep-first-bench", selftest_lockdep_first_bench },
    { "lockdep-mutex-bench", selftest_lockdep_mutex_bench },
    { "lockdep-spin-bench", selftest_lockdep_spin_bench },
    { "lockdep-graph-bench", selftest_lockdep_graph_bench },
    { "fpu-switch",      test_fpu_switch },
    { "fpu-bench",       test_fpu_bench },
    { "objects",         selftest_objects },
    { "elf",             selftest_elf },
    { "bootarchive",     selftest_bootarchive },
    { "ksym",            selftest_ksym },
    { "modsig",          selftest_modsig },
    { "module-reject",   selftest_module_reject },
    { "module-load",     selftest_module_load },
    { "module-fail",     selftest_module_fail },
    { "module-unload-busy", selftest_module_unload_busy },
    { "module-zombie-swept", selftest_module_zombie_swept },
    { "module-zombie-swept-on-every-exit", selftest_module_zombie_swept_on_every_exit },
    { "module-zombie-name-reused", selftest_module_zombie_name_reused },
    { "module-zombie-two-of-a-name", selftest_module_zombie_two_of_a_name },
    { "module-slots-enospc", selftest_module_slots_enospc },
    { "device",          selftest_device },
    { "device-remove-busy", selftest_device_remove_busy },
    { "pci",             selftest_pci },
    { "dma",             selftest_dma },
    { "iommu",           selftest_iommu },
    { "blk-submit-unregister", selftest_blk_submit_unregister },
    { "blk-unregister-drain", selftest_blk_unregister_drain },
    { "virtio-remove-inflight", selftest_virtio_remove_inflight },
    { "device-reset", selftest_device_reset },
    { "blk-lifetime",    selftest_blk_lifetime },
    { "fault-kmalloc",   selftest_fault_kmalloc },
    { "fault-blk",       selftest_fault_blk },
    { "blk-queue",       selftest_blk_queue },
    { "blk-segments",    selftest_blk_segments },
    { "blk-timeout",     selftest_blk_timeout },
    { "blk-timeout-skew", selftest_blk_timeout_skew },
    { "clock-since-saturates", selftest_clock_since_saturates },
    { "clock-cross-cpu", selftest_clock_cross_cpu },
    { "clock-scope-aarch64", selftest_clock_scope_aarch64 },
    { "clock-skew-detected", selftest_clock_skew_detected },
    { "clock-invariant-gate", selftest_clock_invariant_gate },
    { "clock-offset-bound", selftest_clock_offset_bound },
    { "lockup-report-skew", selftest_lockup_report_skew },
    { "clock-cost", selftest_clock_cost },
    { "clock-tick-owner", selftest_clock_tick_owner },
    { "sched-spread",    selftest_sched_spread },
    { "sched-load",      selftest_sched_load },
    { "sched-balance-pull", selftest_sched_balance_pull },
    { "sched-balance-pair", selftest_sched_balance_pair },
    { "sched-balance-hysteresis", selftest_sched_balance_hysteresis },
    { "sched-balance-affinity", selftest_sched_balance_affinity },
    { "bench-balance",   selftest_bench_balance },
    { "percpu-claim",    selftest_percpu_claim },
    { "lockdep-rq-order", selftest_lockdep_rq_order },
    { "sched-migrate",   selftest_sched_migrate },
    { "sched-migrate-refuses", selftest_sched_migrate_refuses },
    { "sched-migrate-stress", selftest_sched_migrate_stress },
    { "nvme",            selftest_nvme },
    { "nvme-admin-poll", selftest_nvme_admin_poll },
    { "usb-enum",        selftest_usb_enum },
    { "usb-storage",     selftest_usb_storage },
    { "usb-storage-timeout", selftest_usb_storage_timeout },
    { "usb-unplug",      selftest_usb_unplug },
    { "ahci-identify",   selftest_ahci_identify },
    { "ahci-io",         selftest_ahci_io },
    { "ahci-timeout",    selftest_ahci_timeout },
    { "ahci-unplug",     selftest_ahci_unplug },
    { "ahci-reset",      selftest_ahci_reset },
    { "random",          selftest_random },
    { "blk",             selftest_blk },
    { "virtio-console",  selftest_virtio_console },
    { "crc32c",          selftest_crc32c },
    { "pagecache",       selftest_pagecache },
    { "pagecache-pinned", selftest_pagecache_pinned },
    { "vfs-ramfs",       selftest_vfs_ramfs },
    { "vfs-rename2",     selftest_vfs_rename2 },
    { "vfs-lookup-named", selftest_vfs_lookup_named },
    { "vfs-umount-once", selftest_vfs_umount_once },
    { "vfs-symlink",     selftest_vfs_symlink },
    { "vfs-symlink-walk", selftest_vfs_symlink_walk },
    { "vfs-symlink-loop", selftest_vfs_symlink_loop },
    { "vfs-symlink-nofollow", selftest_vfs_symlink_nofollow },
    { "vfs-fsync",       selftest_fsync_handle },
    { "read-bounce",     selftest_read_bounce },
    { "wb-error-fsync",  selftest_wb_error_fsync },
    { "wb-error-once",   selftest_wb_error_once },
    { "wb-error-close",  selftest_wb_error_close },
    { "wb-error-lost",   selftest_wb_error_lost },
    { "read-bench",      selftest_read_bench },
    { "write-bench",     selftest_write_bench },
    { "vfs-concurrency", selftest_vfs_concurrency },
    { "vfs-put-race",    selftest_vfs_put_race },
    { "vfs-chrdev-open", selftest_vfs_chrdev_open },
    { "vfs-chr-write-during-blocked-read", selftest_vfs_chr_write_during_blocked_read },
    { "vfs-mount-id",    selftest_vfs_mount_id },
    { "vfs-mount-pin",   selftest_vfs_mount_pin },
    { "fsctl-list",      selftest_fsctl_list },
    { "fsctl-check",     selftest_fsctl_check },
    { "fsctl-result-per-open", selftest_fsctl_result_per_open },
    { "mountns",         selftest_mountns },
    { "utsns",           selftest_utsns },
    { "pool",            selftest_pool },
    { "cosmofs-format",  selftest_cosmofs_format },
    { "cosmofs-ops",     selftest_cosmofs_ops },
    { "cosmofs-crash",   selftest_cosmofs_crash },
    { "cosmofs-replay",  selftest_cosmofs_replay },
    /* The watchdog is armed per test: the first leaves it fired, the
     * second finds it re-armed. Adjacent, in this order. */
    { "watchdog-spend",  selftest_watchdog_spend },
    { "watchdog-rearm",  selftest_watchdog_rearm },
    { "cosmofs-holes",   selftest_cosmofs_holes },
    { "cosmofs-csum",    selftest_cosmofs_csum },
    { "cosmofs-metadata-csum-id", selftest_cosmofs_metadata_csum_id },
    { "cosmofs-fsync",   selftest_cosmofs_fsync },
    { "cosmofs-snapshot", selftest_cosmofs_snapshot },
    { "cosmofs-snapshot-remount", selftest_cosmofs_snapshot_remount },
    { "cosmofs-pool2", selftest_cosmofs_pool2 },
    { "cosmofs-v3", selftest_cosmofs_v3 },
    { "cosmofs-freelog-format", selftest_cosmofs_freelog_format },
    { "cosmofs-freelog-accounted", selftest_cosmofs_freelog_accounted },
    { "cosmofs-freelog-supersede", selftest_cosmofs_freelog_supersede },
    { "cosmofs-freelog-not-held", selftest_cosmofs_freelog_not_held },
    { "cosmofs-unmount-leak", selftest_cosmofs_unmount_leak },
    { "cosmofs-freelog-snapshot", selftest_cosmofs_freelog_snapshot },
    { "cosmofs-snap-cow", selftest_cosmofs_snap_cow },
    { "cosmofs-snap-unmount", selftest_cosmofs_snap_unmount },
    { "cosmofs-snap-nogrow", selftest_cosmofs_snap_nogrow },
    { "cosmofs-snap-reserved", selftest_cosmofs_snap_reserved },
    { "cosmofs-snap-rollback", selftest_cosmofs_snap_rollback },
    { "cosmofs-snap-onewalk", selftest_cosmofs_snap_onewalk },
    { "cosmofs-orphan-crash", selftest_cosmofs_orphan_crash },
    { "cosmofs-orphan-cancels", selftest_cosmofs_orphan_cancels },
    { "cosmofs-orphan-dir", selftest_cosmofs_orphan_dir },
    { "cosmofs-orphan-rename", selftest_cosmofs_orphan_rename },
    { "cosmofs-orphan-idempotent", selftest_cosmofs_orphan_idempotent },
    { "cosmofs-orphan-reserved", selftest_cosmofs_orphan_reserved },
    { "cosmofs-mount-no-early-writeback", selftest_cosmofs_mount_no_early_wb },
    { "cosmofs-orphan-supersede", selftest_cosmofs_orphan_supersede },
    { "cosmofs-orphan-rollback", selftest_cosmofs_orphan_rollback },
    { "cosmofs-orphan-suspect", selftest_cosmofs_orphan_suspect },
    { "cosmofs-freelog-idempotent", selftest_cosmofs_freelog_idempotent },
    { "cosmofs-freelog-reuse", selftest_cosmofs_freelog_reuse },
    { "cosmofs-freelog-chain", selftest_cosmofs_freelog_chain },
    { "cosmofs-freelog-rollback", selftest_cosmofs_freelog_rollback },
    { "cosmofs-freelog-malformed", selftest_cosmofs_freelog_malformed },
    { "cosmofs-check-clean", selftest_cosmofs_check_clean },
    { "cosmofs-check-leak", selftest_cosmofs_check_leak },
    { "cosmofs-check-faults", selftest_cosmofs_check_faults },
    { "cosmofs-check-dup-name", selftest_cosmofs_check_dup_name },
    { "cosmofs-check-snap-members", selftest_cosmofs_check_snap_members },
    { "cosmofs-check-two-parents", selftest_cosmofs_check_two_parents },
    { "cosmofs-check-chain-cycle", selftest_cosmofs_check_chain_cycle },
    { "cosmofs-check-bad-ptr", selftest_cosmofs_check_bad_ptr },
    { "cosmofs-check-namelen", selftest_cosmofs_check_namelen },
    { "cosmofs-check-extent-order", selftest_cosmofs_check_extent_order },
    { "cosmofs-check-extent-overlap", selftest_cosmofs_check_extent_overlap },
    { "cosmofs-check-snapshot", selftest_cosmofs_check_snapshot },
    { "cosmofs-check-orphan-crash", selftest_cosmofs_check_orphan_crash },
    { "cosmofs-check-partial", selftest_cosmofs_check_partial },
    { "cosmofs-check-many-orphans", selftest_cosmofs_check_many_orphans },
    { "cosmofs-check-slot-identity", selftest_cosmofs_check_slot_identity },
    { "cosmofs-symlink", selftest_cosmofs_symlink },
    { "cosmofs-symlink-version", selftest_cosmofs_symlink_version },
    { "cosmofs-badmembers", selftest_cosmofs_badmembers },
    { "cosmofs-mirror", selftest_cosmofs_mirror },
    { "cosmofs-mirror-stale", selftest_cosmofs_mirror_stale },
    { "cosmofs-compress", selftest_cosmofs_compress },
    { "cosmofs-crypt", selftest_cosmofs_crypt },
    { "cosmofs-reserve", selftest_cosmofs_reserve },
    { "cosmofs-fallback", selftest_cosmofs_fallback },
    { "cosmofs-writeback", selftest_cosmofs_writeback },
    { "cosmofs-badmap",  selftest_cosmofs_badmap },
    { "cache-limits",    selftest_cache_limits },
    { "cache-budget-race", selftest_cache_budget_race },
    { "net-mbuf",        selftest_net_mbuf },
    { "net-mbufq-double", selftest_net_mbufq_double },
    { "net-cksum",       selftest_net_cksum },
    { "net-arp",         selftest_net_arp },
    { "net-route",       selftest_net_route },
    { "net-forward",     selftest_net_forward },
    { "net-nat",         selftest_net_nat },
    { "net-netif-flags", selftest_net_netif_flags },
    { "tap",             selftest_tap },
    { "tap-filter",      selftest_tap_filter },
    { "tap-ready",       selftest_tap_ready },
    { "net-dhcp",        selftest_net_dhcp },
    { "net-dns",         selftest_net_dns },
    { "net-dnat",        selftest_net_dnat },
    { "net-pf-clear",    selftest_net_pf_clear },
    { "net-tapctl",      selftest_net_tapctl },
    { "net-multiguest",  selftest_net_multiguest },
    { "net-firewall",    selftest_net_firewall },
    { "net-input",       selftest_net_input },
    { "net-hostinput",   selftest_net_hostinput },
    { "net-zero-window-probe", selftest_net_zero_window_probe },
    { "net-fin-acks-last-data", selftest_net_fin_acks_last_data },
    { "net-hoststate",   selftest_net_hoststate },
    { "net-flows-nat",   selftest_net_flows_nat },
    { "net-flows-fw",    selftest_net_flows_fw },
    { "net-output",      selftest_net_output },
    { "net-tcpverdict",  selftest_net_tcpverdict },
    { "net-second-nic",  selftest_net_second_nic },
    { "net-lo-udp",      selftest_net_lo_udp },
    { "tcp-pcb-timer-free", selftest_tcp_pcb_timer_free },
    { "net-lo-tcp",      selftest_net_lo_tcp },
    { "net-tcp-delack",  selftest_net_tcp_delack },
    { "net-tcp-nagle-peer", selftest_net_tcp_nagle_peer },
    { "net-rx-dup",      selftest_net_rx_dup },
    { "net-accept-order", selftest_net_accept_order },
    { "net-lo-tcp-loss", selftest_net_lo_tcp_loss },
    { "net-tcp-mss",     selftest_net_tcp_mss },
    { "net-netif-lifetime", selftest_net_netif_lifetime },
    { "net-arp-retry-unregister", selftest_net_arp_retry_unregister },
    { "net-arp-flush-counts", selftest_net_arp_flush_counts },
    { "net-nd-flush-counts", selftest_net_nd_flush_counts },
    { "net-nd-retry-unregister", selftest_net_nd_retry_unregister },
    { "net-arp-per-interface", selftest_net_arp_per_interface },
    { "net-nd-per-interface", selftest_net_nd_per_interface },
    { "net-neigh-down-race", selftest_net_neigh_down_race },
    { "net-accept-race", selftest_net_accept_race },
    { "net-census-wake-ref", selftest_net_census_wake_ref },
    { "net-tcp-syncache", selftest_net_tcp_syncache },
    { "net-tcp-rfc5961", selftest_net_tcp_rfc5961 },
    { "net-tcp-reorder", selftest_net_tcp_reorder },
    { "net-tcp-keepalive", selftest_net_tcp_keepalive },
    { "net-icmp-limit",  selftest_net_icmp_limit },
    { "net-nonblock",    selftest_net_nonblock },
    { "net-sockerr-udp", selftest_net_sockerr_udp },
    { "net-sockerr-spoof", selftest_net_sockerr_spoof },
    { "net-sockerr-accept", selftest_net_sockerr_accept },
    { "net-sockerr-locking", selftest_net_sockerr_locking },
    { "net-steer",       selftest_net_steer },
    { "net-rxhook-grace", selftest_net_rxhook_grace },
    { "net-csum-offload", selftest_net_csum_offload },
    { "net-bench",       selftest_net_bench },
    { "blk-bench",       selftest_blk_bench },
    { "net-nicbench",    selftest_net_nicbench },
    { "net-harness",     selftest_net_harness },
    /* Last: init's user-mode self-test mounts the cosmofs the tests above
     * leave on the scratch disk. */
    { "tty-ldisc",       selftest_tty_ldisc },
    { "console-rx-clear", selftest_console_rx_clear },
    { "fb-console",      selftest_fb_console },
    { "fb-bench",        selftest_fb_bench },
    /* After the hotplug tests: unplugging the hub takes the keyboard
     * with it, and keys typed while it is gone are gone too. */
    { "usb-hub-unplug",  selftest_usb_hub_unplug },
    /* And before the keyboard is armed: these three drive the console
     * themselves -- they claim it, type control characters at it, and
     * `^C` throws away whatever line is under edit. Run after the
     * harness has started typing, they eat its line. */
    { "signal-stop-restart", selftest_signal_stop_restart },
    { "tty-raw",         selftest_tty_raw },
    { "tty-nosig",       selftest_tty_nosig },
    { "tty-isatty",      selftest_tty_isatty },
    { "tty-pollraw",     selftest_tty_pollraw },
    { "tty-devready",    selftest_tty_devready },
    { "dev-tty",         selftest_dev_tty },
    { "dev-tty-none",    selftest_dev_tty_none },
    { "tty-intr",        selftest_tty_intr },
    { "tty-stop",        selftest_tty_stop },
    { "tty-ttin",        selftest_tty_ttin },
    { "hid-arm",         selftest_hid_arm },
    { "ipc-pipe",        selftest_ipc_pipe },
    { "ipc-fifo",        selftest_ipc_fifo },
    { "unix-stream",     selftest_unix_stream },
    { "unix-dgram",      selftest_unix_dgram },
    { "unix-name",       selftest_unix_name },
    { "unix-handles",    selftest_unix_handles },
    { "unix-close-race", selftest_unix_close_race },
    { "unix-poll",       selftest_unix_poll },
    { "io-poll",         selftest_io_poll },
    { "epoll-close",     selftest_epoll_close },
    { "epoll-scale",     selftest_epoll_scale },
    { "epoll-wake-race", selftest_epoll_wake_race },
    { "epoll-nest",      selftest_epoll_nest },
    { "epoll-close-bench", selftest_epoll_close_bench },
    { "realtime",        selftest_realtime },
    { "process-reject",  selftest_process_reject },
    { "process-spawn",   selftest_process_spawn },
    { "linux-elf",       selftest_linux_elf },
    { "el2",             selftest_el2_stub },
    { "hv-disabled",     selftest_hv_disabled },
    { "el2-guest-wfi",   selftest_el2_guest_wfi },
    { "el2-vgic-roundtrip", selftest_el2_vgic_roundtrip },
    { "el2-guest-irq",   selftest_el2_guest_irq },
    { "el2-guest-irq-masked", selftest_el2_guest_irq_masked },
    { "el2-guest-irq-private", selftest_el2_guest_irq_private },
    { "el2-guest-irq-queue", selftest_el2_guest_irq_queue },
    { "el2-guest-timer-isolated", selftest_el2_guest_timer_isolated },
    { "el2-guest-timer-offset", selftest_el2_guest_timer_offset },
    { "el2-guest-phys-timer", selftest_el2_guest_phys_timer },
    { "el2-guest-timer",  selftest_el2_guest_timer },
    { "el2-guest-timer-ontime", selftest_el2_guest_timer_ontime },
    { "el2-guest-gicd-probe", selftest_el2_guest_gicd_probe },
    { "el2-guest-gic-config", selftest_el2_guest_gic_config },
    { "el2-guest-gic-timer", selftest_el2_guest_gic_timer },
    { "el2-guest-sgi", selftest_el2_guest_sgi },
    { "el2-guest-gicd-isolated", selftest_el2_guest_gicd_isolated },
    { "el2-mmio-device", selftest_el2_mmio_device },
    { "el2-guest-uart", selftest_el2_guest_uart },
    { "el2-guest-uart-rx", selftest_el2_guest_uart_rx },
    { "el2-guest-uart-level", selftest_el2_guest_uart_level },
    { "el2-guest-uart-race", selftest_el2_guest_uart_race },
    { "el2-guest-dtb", selftest_el2_guest_dtb },
    { "el2-guest-psci", selftest_el2_guest_psci },
    { "el2-vcpu-run-tick", selftest_el2_vcpu_run_tick },
    { "el2-guest-hvc",   selftest_el2_guest_hvc },
    { "el2-guest-mmio",  selftest_el2_guest_mmio },
    { "el2-guest-sysreg", selftest_el2_guest_sysreg },
    { "el2-guest-idreg", selftest_el2_guest_idreg },
    { "el2-vm-raise-spi", selftest_el2_vm_raise_spi },
    { "el2-virtq-device", selftest_el2_virtq_device },
    { "el2-virtq-net", selftest_el2_virtq_net },
    { "el2-tap-host", selftest_el2_tap_host },
    { "el2-guest-spin",  selftest_el2_guest_spin },
    { "hv-probe",        selftest_hv_probe },
    { "hv-vcpu-regs-roundtrip", selftest_hv_vcpu_regs_roundtrip },
    { "hv-caps",         selftest_hv_caps },
    { "hv-npt",          selftest_hv_npt },
    { "hv-guest-pio",    selftest_hv_guest_pio },
    { "hv-guest-irq",    selftest_hv_guest_irq },
    { "hv-guest-cpuid",  selftest_hv_guest_cpuid },
    { "hv-guest-pm",     selftest_hv_guest_pm },
    { "hv-guest-shutdown", selftest_hv_guest_shutdown },
    { "hv-guest-spin",   selftest_hv_guest_spin },
    { "hv-vcpu-stop",    selftest_hv_vcpu_stop },
    { "hv-guest-fpu",    selftest_hv_guest_fpu },
    { "process-user",    selftest_process_selftest },
    { "process-fault",   selftest_process_fault },
    { "signal-native",   selftest_signal_native },
    { "signal-async",    selftest_signal_async },
    { "signal-mask",     selftest_signal_mask },
    { "signal-fault",    selftest_signal_fault },
    { "process-reaped",  selftest_process_reaped },
    { "process-gone-order", selftest_process_gone_order },
    { "process-leftover-named", selftest_process_leftover_named },
    { "p33-once",        selftest_p33_once },
    { "exit-space-order", selftest_exit_space_order },
    { "signal-group",    selftest_signal_group },
    { "signal-setsid",   selftest_signal_setsid },
    { "signal-stop",     selftest_signal_stop },
    { "signal-stop-kill", selftest_signal_stop_kill },
    { "signal-stop-mask", selftest_signal_stop_mask },
    { "signal-stop-late", selftest_signal_stop_late },
    { "signal-stop-threads", selftest_signal_stop_threads },
    { "process-efault",  selftest_process_efault },
    { "process-protnone", selftest_process_protnone },
    { "process-oom",     selftest_process_oom },
    { "process-rlimit",  selftest_process_rlimit },
    { "mmap-place-race", selftest_mmap_place_race },
    { "vm-file-fault-hold", selftest_vm_file_fault_hold },
    { "vm-file-readpage-fail", selftest_vm_file_readpage_fail },
    { "cwd-hold-native", selftest_cwd_hold_native },
    { "cwd-hold-linux",  selftest_cwd_hold_linux },
    { "dirfd-rights",    selftest_dirfd_rights },
    { "process-nproc",   selftest_process_nproc },
    { "syscall-fuzz",    selftest_syscall_fuzz },
    /* Last: what the harness typed at the keyboard while everything
     * above ran (kernel/device/hidtest.c). */
    { "hid-keyboard",    selftest_hid_keyboard },
    /* After the tests that read or write the console.
     *
     * These spawn real programs, and a spawned program prints. Run
     * earlier they made `hid-keyboard` read their output instead of the
     * line it sent, and left `process-spawn` racing their teardown --
     * a test that perturbs what it shares the machine with is measuring
     * the machine (docs/audit/next-subsystem-elf-shared-text.md). */
    { "elf-shared-text", selftest_elf_shared_text },
    { "elf-share-cost",  selftest_elf_share_cost },
    { "elf-text-ro",     selftest_elf_text_ro },
    { "elf-data-private", selftest_elf_data_private },
    { "elf-txtbsy",      selftest_elf_txtbsy },
};

/*
 * P33: nothing a self-test spawns outlives it. A test waits for its
 * process to exit and drops its reference, but the reaper may hold the
 * last one for a while after; a process released during the *next* test
 * broke that test's count and was blamed on it, three times in three
 * tests (docs/audit/next-subsystem-proc-settle.md). So the runner waits
 * up to `wait_ns` for the table to empty after every test, and a test
 * that leaves a process fails, by name.
 *
 * Returns how many processes are left and fills up to `max` of their
 * pids. It logs nothing: the runner names what it finds, and the test of
 * this check (process-leftover-named) finds one on purpose.
 */
/*
 * A process already reported is blamed once. Without this a test that
 * leaked its reference -- a CHECK that returned before its put -- failed
 * every test after it for the same pid: 19 of them in one boot of the
 * exit-space probe. A reported pid is ignored until it leaves the table,
 * and forgotten then.
 */
/* One limit for both: a snapshot names up to 64 pids, and every pid a
 * check returns can be remembered. More than 64 processes in the table
 * at a test's end (never seen: it is empty after every test of a clean
 * boot) are counted but not named, and could be blamed again. */
enum { LEFTOVER_SNAPSHOT_MAX = 64, LEFTOVER_REPORTED_MAX = LEFTOVER_SNAPSHOT_MAX };
static uint32_t g_leftover_reported[LEFTOVER_REPORTED_MAX];
static unsigned g_leftover_nreported;

void selftest_leftover_reported(uint32_t pid)
{
    for (unsigned i = 0; i < g_leftover_nreported; i++)
        if (g_leftover_reported[i] == pid)
            return;
    if (g_leftover_nreported < LEFTOVER_REPORTED_MAX)
        g_leftover_reported[g_leftover_nreported++] = pid;
}

static bool leftover_was_reported(uint32_t pid)
{
    for (unsigned i = 0; i < g_leftover_nreported; i++)
        if (g_leftover_reported[i] == pid)
            return true;
    return false;
}

/* From one locked snapshot: forget reported pids no longer in it, and
 * return the others -- all of them counted, and *named of them written to
 * `pids` (never more than `max`, never a pid beyond the snapshot). */
static unsigned leftover_snapshot(uint32_t *pids, unsigned max, unsigned *named)
{
    static uint32_t all[LEFTOVER_SNAPSHOT_MAX];
    unsigned total = process_table_pids(all, LEFTOVER_SNAPSHOT_MAX), seen = total < LEFTOVER_SNAPSHOT_MAX ? total : LEFTOVER_SNAPSHOT_MAX;
    unsigned keep = 0;
    for (unsigned i = 0; i < g_leftover_nreported; i++) {
        bool present = false;
        for (unsigned k = 0; k < seen && !present; k++)
            present = all[k] == g_leftover_reported[i];
        if (present || total > LEFTOVER_SNAPSHOT_MAX)   /* a snapshot too large to search forgets nothing */
            g_leftover_reported[keep++] = g_leftover_reported[i];
    }
    g_leftover_nreported = keep;
    unsigned n = 0, w = 0;
    for (unsigned k = 0; k < seen; k++)
        if (!leftover_was_reported(all[k])) {
            if (w < max)
                pids[w++] = all[k];
            n++;
        }
    if (named)
        *named = w;
    return n + (total - seen);   /* beyond the snapshot: counted, not named */
}

/*
 * A test's releases run however it returns (the net-leftover unit,
 * docs/audit/next-subsystem-net-leftover.md). CHECK returns at once, and a
 * test that released what it made only on its last lines kept all of it on
 * a failure: one net-dns failure held a tap and a service slot, and eight
 * tests after it failed for the slot. A release is registered at the
 * acquisition; the runner runs what is left, last first, after the test.
 * selftest_release runs one early, where the test used to tear down, so
 * the success path keeps its order. Locked: a test's own threads put the
 * sockets they were handed.
 */
enum { DEFER_MAX = 64 };
static struct {
    void (*fn)(void *);
    void *arg;
} g_defers[DEFER_MAX];
static unsigned g_ndefers;
static bool g_defer_overflow;
static spinlock_t g_defer_lock = SPINLOCK_INIT("selftest-defer");

bool selftest_defer(void (*fn)(void *), void *arg)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_defer_lock);
    bool ok = g_ndefers < DEFER_MAX;
    if (ok) {
        g_defers[g_ndefers].fn = fn;
        g_defers[g_ndefers].arg = arg;
        g_ndefers++;
    } else {
        g_defer_overflow = true;
    }
    spin_unlock_irqrestore(&g_defer_lock, s);
    return ok;
}

bool selftest_release(void *arg)
{
    void (*fn)(void *) = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_defer_lock);
    for (unsigned i = g_ndefers; i-- > 0;)
        if (g_defers[i].arg == arg) {
            fn = g_defers[i].fn;
            for (unsigned k = i; k + 1 < g_ndefers; k++)
                g_defers[k] = g_defers[k + 1];
            g_ndefers--;
            break;
        }
    spin_unlock_irqrestore(&g_defer_lock, s);
    if (fn == NULL)
        return false;
    fn(arg);   /* outside the lock: a release may sleep, join, or register */
    return true;
}

bool selftest_forget(void *arg)
{
    bool found = false;
    arch_irq_state_t s = spin_lock_irqsave(&g_defer_lock);
    for (unsigned i = g_ndefers; i-- > 0;)
        if (g_defers[i].arg == arg) {
            for (unsigned k = i; k + 1 < g_ndefers; k++)
                g_defers[k] = g_defers[k + 1];
            g_ndefers--;
            found = true;
            break;
        }
    spin_unlock_irqrestore(&g_defer_lock, s);
    return found;
}

/* The runner's side: every release still registered, last first. */
static unsigned run_defers(void)
{
    unsigned ran = 0;
    for (;;) {
        void (*fn)(void *) = NULL;
        void *arg = NULL;
        arch_irq_state_t s = spin_lock_irqsave(&g_defer_lock);
        if (g_ndefers > 0) {
            g_ndefers--;
            fn = g_defers[g_ndefers].fn;
            arg = g_defers[g_ndefers].arg;
        }
        spin_unlock_irqrestore(&g_defer_lock, s);
        if (fn == NULL)
            return ran;
        fn(arg);
        ran++;
    }
}

unsigned selftest_leftover_processes(uint64_t wait_ns, uint32_t *pids, unsigned max, unsigned *named)
{
    /* One locked snapshot decides and is returned: the count it reports
     * and the pids it names are the same reading of the table. */
    uint64_t deadline = clock_deadline_ns(wait_ns);
    for (;;) {
        unsigned n = leftover_snapshot(pids, max, named);
        if (n == 0 || clock_deadline_passed(deadline))
            return n;
        thread_sleep_ms(1);
    }
}

/* A test's budget in ms: its entry in `budgets`, or the default. */
static unsigned selftest_budget_ms(const char *name)
{
    for (size_t b = 0; b < ARRAY_SIZE(budgets); b++)
        if (strcmp(budgets[b].name, name) == 0)
            return budgets[b].ms;
    return SELFTEST_BUDGET_DEFAULT_MS;
}

int selftest_run_all(void)
{
    int failed = 0;

    /* Every budget, before the first test: the harness judges durations by
     * this line and by nothing of its own. */
    {
        static char line[256];   /* one kprintf: another CPU's line must not land inside it */
        size_t n = (size_t)ksnprintf(line, sizeof(line), "SELFTEST: budgets default=%u", SELFTEST_BUDGET_DEFAULT_MS);
        for (size_t b = 0; b < ARRAY_SIZE(budgets); b++) {
            bool named = false;
            for (size_t i = 0; i < ARRAY_SIZE(tests) && !named; i++)
                named = strcmp(tests[i].name, budgets[b].name) == 0;
            if (!named)
                panic("selftest: a budget for '%s', which is no test", budgets[b].name);
            if (n < sizeof(line))
                n += (size_t)ksnprintf(line + n, sizeof(line) - n, " %s=%u", budgets[b].name, budgets[b].ms);
        }
        KASSERT(n < sizeof(line));   /* a truncated line would hold a test to the default */
        kprintf("%s\n", line);
    }

    uint64_t total_ns = 0, slowest_ns = 0;
    const char *slowest = "-";
    for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
        const char *reason = "";
        /* A test that hangs is worth more with a scheduler dump than as a
         * bare harness timeout. Armed, not kicked: an arming fires once, so
         * one armed for the whole run was spent by the first test to go
         * quiet for 8 s -- `cosmofs-replay`, in every debug boot -- and
         * the tests after it had none. */
        sched_watchdog_arm((uint64_t)selftest_budget_ms(tests[i].name) * 1000 * 1000);
        uint64_t t0 = clock_now_ns();
        /* The network before the test: a test is judged by what it
         * changed, so a leftover is blamed on the test that left it and
         * becomes the next test's starting point -- blamed once. */
        static struct nettest_census net_before, net_after;
        nettest_census(&net_before);
        g_defer_overflow = false;
        bool ok = tests[i].fn(&reason);
        unsigned released = run_defers();
        if (g_defer_overflow && ok) {
            ok = false;
            reason = "its release list overflowed";
        }
        (void)released;
        {
            /* Counted in the test's time, so the budget sees the wait. */
            enum { LEFT_NAMED = 16 };
            static uint32_t left[LEFTOVER_SNAPSHOT_MAX];
            unsigned named = 0;
            unsigned nleft = selftest_leftover_processes(2000ull * 1000 * 1000, left, LEFTOVER_SNAPSHOT_MAX, &named);
            for (unsigned k = 0; k < named; k++) {   /* only the pids this check wrote: never a stale entry */
                char what[96];
                if (k < LEFT_NAMED && process_describe(left[k], what, sizeof(what)))
                    kerror("selftest: %s left %s", tests[i].name, what);
                selftest_leftover_reported(left[k]);   /* blamed here, and only here: named or not */
            }
            if (nleft > LEFT_NAMED)
                kerror("selftest: %s left %u more processes, not named", tests[i].name, nleft - LEFT_NAMED);
            if (nleft != 0 && ok) {
                ok = false;
                reason = "a process it spawned outlived it (P33)";
            }
        }
        nettest_census(&net_after);
        if (!nettest_census_equal(&net_before, &net_after)) {
            kerror("selftest: %s left the network changed: interfaces (%u) [%s] -> (%u) [%s], services %u -> %u, sockets %u -> %u",
                   tests[i].name, net_before.nnetifs, net_before.netifs, net_after.nnetifs, net_after.netifs,
                   net_before.services, net_after.services, net_before.sockets, net_after.sockets);
            if (ok) {
                ok = false;
                reason = "it left network state behind";
            }
        }
        uint64_t dt = clock_since_ns(t0);
        total_ns += dt;
        if (dt > slowest_ns) {
            slowest_ns = dt;
            slowest = tests[i].name;
        }
        /* The duration is on every line so the boot harness can rank the
         * tests and fail one that nears the watchdog (docs/verification/). */
        if (ok) {
            kprintf("SELFTEST: %-16s ... ok (%llu ms)\n", tests[i].name, (unsigned long long)(dt / 1000000));
        } else {
            kprintf("SELFTEST: %-16s ... FAIL: %s (%llu ms)\n", tests[i].name, reason,
                    (unsigned long long)(dt / 1000000));
            failed++;
        }
    }

    sched_watchdog_disarm();
    nettest_finish();   /* sockets abandoned connects hand over from now on are theirs to put */

    /* The lock order the whole run recorded, for docs/kernel/lockdep/testing.md. */
    lockdep_dump_graph();
    /* What the run did not drive: callback functions set up and never run,
     * completion classes never signalled or never waited for (lockdep
     * design.md, "Coverage"); the paths the graph has no edges for. */
    timer_dump_callbacks();
    lockdep_dump_completion_coverage();

    kprintf("SELFTEST: timing total=%llu ms slowest=%s (%llu ms)\n", (unsigned long long)(total_ns / 1000000), slowest,
            (unsigned long long)(slowest_ns / 1000000));

    if (failed == 0)
        kprintf("SELFTEST: PASS (%zu tests)\n", ARRAY_SIZE(tests));
    else
        kprintf("SELFTEST: FAIL (%d of %zu)\n", failed, ARRAY_SIZE(tests));

#if CONFIG_SCHED_BALANCE
    {
        /* What the balancer did over the whole boot: the pulls are the
         * point, the refusals say which check the primitive applied, and
         * `no_candidate` dominating is the healthy shape on a machine
         * that is mostly balanced already. */
        struct sched_balance_stats bs;
        sched_balance_stats(&bs);
        kinfo("sched: balance pulled %llu threads in %llu scans, %llu found nothing far enough ahead; "
              "refused not-ready %llu current %llu preempted %llu affinity %llu",
              (unsigned long long)bs.pulls, (unsigned long long)bs.scans,
              (unsigned long long)bs.no_candidate,
              (unsigned long long)bs.refused[SCHED_MIGRATE_NOT_READY],
              (unsigned long long)bs.refused[SCHED_MIGRATE_CURRENT],
              (unsigned long long)bs.refused[SCHED_MIGRATE_PREEMPTED],
              (unsigned long long)bs.refused[SCHED_MIGRATE_AFFINITY]);
    }
#endif

#if CONFIG_SCHED_CHAOS
    /* The boot test requires this line, with a count above zero: a
     * chaos boot that moved nothing proved nothing (test-chaos). */
    uint64_t chaos_moved, chaos_refused;
    sched_chaos_stats(&chaos_moved, &chaos_refused);
    kinfo("sched: chaos migrated %llu threads from the tick, %llu calls found nothing to move; %llu migrations in all",
          (unsigned long long)chaos_moved, (unsigned long long)chaos_refused,
          (unsigned long long)sched_migration_count());
#endif

    return failed;
}
