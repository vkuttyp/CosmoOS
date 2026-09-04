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
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

#include <arch/irq.h>
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

static const struct selftest tests[] = {
    { "printf",          test_printf },
    { "string",          test_string },
    { "bootinfo",        test_bootinfo },
    { "irq-state",       test_irq_state },
    { "breakpoint-trap", test_breakpoint_trap },
    { "pmm",             selftest_pmm },
    { "vmm",             selftest_vmm },
    { "kmalloc",         selftest_kmalloc },
    { "acpi",            selftest_acpi },
    { "timer",           selftest_timer },
    { "irq-route",       selftest_irq_route },
    { "thread",          selftest_thread },
    { "yield",           selftest_yield },
    { "preempt",         selftest_preempt },
    { "sleep",           selftest_sleep },
    { "mutex",           selftest_mutex },
    { "semaphore",       selftest_semaphore },
    { "completion",      selftest_completion },
    { "waitqueue",       selftest_waitqueue },
    { "smp-online",      selftest_smp_online },
    { "smp-affinity",    selftest_smp_affinity },
    { "smp-parallel",    selftest_smp_parallel },
    { "smp-call",        selftest_smp_call },
    { "smp-shootdown",   selftest_smp_shootdown },
    { "smp-wake",        selftest_smp_wake },
    { "smp-ticks",       selftest_smp_ticks },
    { "smp-mutex",       selftest_smp_mutex },
    { "objects",         selftest_objects },
    { "elf",             selftest_elf },
    { "process-reject",  selftest_process_reject },
    { "process-user",    selftest_process_selftest },
    { "process-fault",   selftest_process_fault },
};

int selftest_run_all(void)
{
    int failed = 0;

    /* A test that hangs is worth more with a scheduler dump than as a
     * bare harness timeout. */
    sched_watchdog_arm(8ull * 1000 * 1000 * 1000);

    for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
        const char *reason = "";
        sched_watchdog_kick();
        bool ok = tests[i].fn(&reason);
        if (ok) {
            kprintf("SELFTEST: %-16s ... ok\n", tests[i].name);
        } else {
            kprintf("SELFTEST: %-16s ... FAIL: %s\n", tests[i].name, reason);
            failed++;
        }
    }

    sched_watchdog_disarm();

    if (failed == 0)
        kprintf("SELFTEST: PASS (%zu tests)\n", ARRAY_SIZE(tests));
    else
        kprintf("SELFTEST: FAIL (%d of %zu)\n", failed, ARRAY_SIZE(tests));

    return failed;
}
