/*
 * clocktest.c - The clock's cross-CPU contract
 * (docs/audit/next-subsystem-cpu-clock.md).
 *
 * `clock_now_ns` promises values that two CPUs may subtract from each
 * other. These tests are what makes that a claim about the machine
 * rather than about the comment above it.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/selftest.h>
#include <kernel/timer.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/*
 * The saturating subtraction itself, in isolation.
 *
 * Every other test in this unit exercises `clock_since_ns` through
 * something -- a block timeout, a lockup report -- where a failure could
 * be blamed on the something. This one leaves nowhere else for a failure
 * to come from: the stamp is a number this test chose, and the only code
 * between it and the assertion is the helper.
 */
bool selftest_clock_since_saturates(const char **reason)
{
    uint64_t now = clock_now_ns();

    /*
     * A stamp from the future -- what residual skew looks like from the
     * far side -- is an age of zero, not of 584 years.
     *
     * The margin is a minute rather than a nanosecond, and that is not
     * timidity: `clock_since_ns` reads the clock itself, so a stamp one
     * nanosecond ahead has already been overtaken by the time the call
     * reads it. Asserting on that margin measures how fast the clock
     * advances between two statements, not whether the subtraction
     * saturates -- which is how the first draft of this test failed.
     * The one-nanosecond boundary is asserted below on `clock_delta_ns`,
     * where both operands are chosen and no clock runs between them.
     */
    CHECK(clock_since_ns(now + 60ull * 1000000000ull) == 0);
    CHECK(clock_since_ns(UINT64_MAX) == 0);

    /* The form that takes a caller's `now`: exact, including the
     * boundary either side of equality. */
    CHECK(clock_delta_ns(now, now + 1) == 0);
    CHECK(clock_delta_ns(now, UINT64_MAX) == 0);
    CHECK(clock_delta_ns(now, now) == 0);
    CHECK(clock_delta_ns(now + 1, now) == 1);
    CHECK(clock_delta_ns(now + 1000, now) == 1000);

    /* A stamp in the past is still an interval: saturating must not have
     * flattened the ordinary case. A real elapsed time is bounded below
     * by the offset and above by that plus however long this test is
     * descheduled for, so the upper bound is generous on purpose. */
    uint64_t past = now - 1000000ull;   /* 1 ms ago */
    uint64_t age = clock_since_ns(past);
    CHECK(age >= 1000000ull);
    CHECK(age < 10ull * 1000000000ull);

    kinfo("selftest: clock-since-saturates: a stamp 60 s in the future reads as an age of 0; one 1 ms in the past reads as %llu ns",
          (unsigned long long)age);
    return true;
}

/*
 * What the correction costs on the clock path.
 *
 * The report's claim is that it is not measurable; a benchmark is what
 * turns that into a measurement rather than an expectation. What is
 * being timed is `clock_now_ns` as shipped -- one counter read, a
 * 128-bit multiply and shift, then one array load and one add -- against
 * `clock_raw_ns`, which is the same without the last two.
 *
 * The branch is measured too, and it is why the difference is not zero
 * on a machine applying no correction. It is not there to save the add:
 * `arch_cpu_id()` reads the per-CPU block through GS, so the flag is
 * what keeps `clock_now_ns` from depending on percpu being installed --
 * including on an AP partway through its own bring-up.
 */
bool selftest_clock_cost(const char **reason)
{
    enum { N = 200000 };
    uint64_t sink = 0;

    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < N; i++)
        sink += clock_raw_ns();
    uint64_t raw_ns = clock_since_ns(t0);

    t0 = clock_now_ns();
    for (unsigned i = 0; i < N; i++)
        sink += clock_now_ns();
    uint64_t corrected_ns = clock_since_ns(t0);

    CHECK(sink != 0);   /* neither loop was optimised away */
    CHECK(raw_ns > 0 && corrected_ns > 0);

    kinfo("selftest: clock-cost: %llu ns per clock_now_ns, %llu ns per clock_raw_ns over %u calls each (%lld ns for the correction)",
          (unsigned long long)(corrected_ns / N), (unsigned long long)(raw_ns / N), (unsigned)N,
          (long long)((int64_t)corrected_ns - (int64_t)raw_ns) / (int64_t)N);
    return true;
}

#if CONFIG_DEBUG

#include <kernel/percpu.h>
#include <kernel/thread.h>

#include <arch/cpu.h>
#include <arch/timer.h>

/*
 * The bracket: does a reading taken on CPU B fall between two readings
 * taken on CPU A either side of it?
 *
 * A reads t0, hands the turn to B, B reads tb and hands it back, A reads
 * t1. The handshake orders the three reads in *real* time, so on a clock
 * common to both CPUs t0 <= tb <= t1 must hold numerically too. How far
 * tb falls outside that bracket is the apparent offset between the two
 * CPUs, and it is the only quantity these tests assert on.
 *
 * Note what is deliberately *not* asserted: that the offset is zero. Two
 * CPUs read an advancing counter at two different instants, so t0, tb
 * and t1 differ even on hardware with one counter -- an equality would
 * fail on a correct machine. The bracket is the right shape because it
 * is exactly as tight as the round trip allows and no tighter.
 */

#define BRACKET_ROUNDS 200u

struct bracket {
    volatile unsigned turn;     /* 0: A's, 1: B's, 2: B is done */
    volatile uint64_t tb;
    volatile unsigned bcpu, acpu;
    volatile bool bready, stop, stalled;
    uint64_t worst_outside;     /* furthest tb fell outside the bracket */
    uint64_t worst_bracket;     /* widest bracket seen */
    uint64_t at_bracket;        /* the bracket width of the worst round */
    unsigned rounds;
};

/* Spin until `cond`, giving up after a second rather than hanging the
 * boot if the other side never gets scheduled. */
#define SPIN_UNTIL(s, cond)                                                    \
    do {                                                                       \
        uint64_t spin0_ = clock_now_ns();                                      \
        while (!(cond)) {                                                      \
            if (clock_since_ns(spin0_) > NS_PER_SEC) {                         \
                (s)->stalled = true;                                           \
                return;                                                        \
            }                                                                  \
            arch_cpu_relax();                                                  \
        }                                                                      \
    } while (0)

static void bracket_b(void *arg)
{
    struct bracket *s = arg;
    s->bcpu = arch_cpu_id();
    __atomic_store_n(&s->bready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s->turn, __ATOMIC_ACQUIRE) == 1) {
            s->tb = clock_now_ns();
            __atomic_store_n(&s->turn, 2u, __ATOMIC_RELEASE);
        }
        arch_cpu_relax();
    }
}

static void bracket_a(void *arg)
{
    struct bracket *s = arg;
    s->acpu = arch_cpu_id();
    SPIN_UNTIL(s, __atomic_load_n(&s->bready, __ATOMIC_ACQUIRE));

    for (unsigned r = 0; r < BRACKET_ROUNDS; r++) {
        uint64_t t0 = clock_now_ns();
        __atomic_store_n(&s->turn, 1u, __ATOMIC_RELEASE);
        SPIN_UNTIL(s, __atomic_load_n(&s->turn, __ATOMIC_ACQUIRE) == 2);
        uint64_t t1 = clock_now_ns();
        uint64_t tb = s->tb;

        uint64_t width = clock_delta_ns(t1, t0);
        uint64_t outside = tb < t0 ? t0 - tb : (tb > t1 ? tb - t1 : 0);
        if (outside > s->worst_outside) {
            s->worst_outside = outside;
            s->at_bracket = width;
        }
        if (width > s->worst_bracket)
            s->worst_bracket = width;
        s->rounds++;
        __atomic_store_n(&s->turn, 0u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
}

/*
 * Every online pair, both ways round. Returns false only if a thread
 * could not be created or a handshake stalled; a measured offset, however
 * large, is a result rather than an error -- the caller decides what
 * bound it has to meet.
 */
static bool bracket_all_pairs(struct bracket *worst, const char **reason)
{
    memset(worst, 0, sizeof(*worst));
    unsigned n = cpu_count();
    if (n < 2) {
        *reason = "a cross-CPU claim needs two CPUs";
        return false;
    }
    for (unsigned a = 0; a < n; a++) {
        for (unsigned b = 0; b < n; b++) {
            if (a == b || !cpu_online(a) || !cpu_online(b))
                continue;
            struct bracket *s = kzalloc(sizeof(*s));
            if (s == NULL) {
                *reason = "out of memory";
                return false;
            }
            struct thread *tb = thread_create_on(bracket_b, s, "clk-b", SCHED_PRIO_DEFAULT, CPUMASK_OF(b));
            struct thread *ta = NULL;
            if (tb != NULL)
                ta = thread_create_on(bracket_a, s, "clk-a", SCHED_PRIO_DEFAULT, CPUMASK_OF(a));
            if (tb == NULL || ta == NULL) {
                __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
                if (tb)
                    thread_join(tb);
                kfree(s);
                *reason = "cannot create the pinned threads";
                return false;
            }
            thread_join(ta);
            __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
            thread_join(tb);

            bool bad = s->stalled || s->acpu != a || s->bcpu != b || s->rounds != BRACKET_ROUNDS;
            uint64_t outside = s->worst_outside, at = s->at_bracket, wide = s->worst_bracket;
            unsigned ra = s->acpu, rb = s->bcpu, rounds = s->rounds;
            bool stalled = s->stalled;
            kfree(s);
            if (bad) {
                kerror("selftest: clock bracket: pair %u/%u ran on %u/%u, %u of %u rounds%s",
                       a, b, ra, rb, rounds, BRACKET_ROUNDS, stalled ? " (a handshake stalled)" : "");
                *reason = "the pinned threads did not run where they were pinned";
                return false;
            }
            if (outside > worst->worst_outside) {
                worst->worst_outside = outside;
                worst->at_bracket = at;
                worst->acpu = a;
                worst->bcpu = b;
            }
            if (wide > worst->worst_bracket)
                worst->worst_bracket = wide;
            worst->rounds += rounds;
        }
    }
    return true;
}

/*
 * The promise, checked against the number the boot advertised.
 *
 * The bound is what `clock_worst_offset_ns()` says plus the width of the
 * bracket the round itself formed -- the second term because the two
 * outer reads are not simultaneous, so a reading at the far edge of a
 * wide bracket is not evidence of skew.
 */
bool selftest_clock_cross_cpu(const char **reason)
{
    if (!clock_is_common()) {
        /* The boot said this counter is not comparable across CPUs, so
         * there is no promise here to check. Asserting anything would be
         * inventing one. */
        kinfo("selftest: clock-cross-cpu: this machine's clock is not common to every CPU; no cross-CPU claim to check");
        (void)reason;
        return true;
    }

    struct bracket w;
    if (!bracket_all_pairs(&w, reason))
        return false;

    uint64_t bound = clock_worst_offset_ns() + w.at_bracket;
    if (w.worst_outside > bound) {
        kerror("selftest: clock-cross-cpu: CPU %u read %llu ns outside CPU %u's bracket (width %llu ns); the boot advertised %llu ns",
               w.bcpu, (unsigned long long)w.worst_outside, w.acpu,
               (unsigned long long)w.at_bracket, (unsigned long long)clock_worst_offset_ns());
        *reason = "a reading fell further outside the bracket than the advertised bound allows";
        return false;
    }
    kinfo("selftest: clock-cross-cpu: %u handshakes over every online pair; worst reading %llu ns outside its bracket (widest bracket %llu ns, advertised bound %llu ns)",
          w.rounds, (unsigned long long)w.worst_outside, (unsigned long long)w.worst_bracket,
          (unsigned long long)clock_worst_offset_ns());
    return true;
}

/*
 * AArch64: `cntpct_el0` is the system counter, architecturally one
 * counter for the whole system rather than one per PE. This unit does
 * not change that; it is the test that says so, because "no skew" that
 * nobody measures is a belief. It is also the control for step 4: if the
 * correction ever moves an AArch64 timestamp, this fails.
 */
bool selftest_clock_scope_aarch64(const char **reason)
{
#if !defined(__aarch64__)
    kinfo("selftest: clock-scope-aarch64: not AArch64; skipping");
    (void)reason;
    return true;
#else
    struct bracket w;
    if (!bracket_all_pairs(&w, reason))
        return false;

    /* The counter is common, so the advertised bound is zero and every
     * reading lands inside its bracket -- not "the offset is zero",
     * which would fail on correct hardware. */
    if (clock_worst_offset_ns() != 0) {
        kerror("selftest: clock-scope-aarch64: the boot advertised a bound of %llu ns on a shared counter",
               (unsigned long long)clock_worst_offset_ns());
        *reason = "the system counter is common to every PE: the bound must be zero";
        return false;
    }
    if (w.worst_outside != 0) {
        kerror("selftest: clock-scope-aarch64: CPU %u read %llu ns outside CPU %u's bracket (width %llu ns)",
               w.bcpu, (unsigned long long)w.worst_outside, w.acpu, (unsigned long long)w.at_bracket);
        *reason = "a reading fell outside its bracket on a shared counter";
        return false;
    }
    kinfo("selftest: clock-scope-aarch64: %u handshakes over every online pair; every reading inside its bracket (widest %llu ns), advertised bound 0",
          w.rounds, (unsigned long long)w.worst_bracket);
    return true;
#endif
}

/*
 * The oracle, proved on a machine that has no skew.
 *
 * `clock-cross-cpu` and `clock-scope-aarch64` both pass trivially here:
 * every machine this project boots on has counters that agree, and the
 * run above measured exactly zero nanoseconds outside the bracket. A
 * test that cannot fail is not evidence, so this one injects the skew
 * the hardware declines to supply and asserts that the oracle *rejects*
 * it -- in both directions, because an offset the other way through a
 * one-sided comparison would sail through, and a bug-proof that happened
 * to be negative would then certify an oracle that does not work.
 *
 * It also checks the converse: widen the advertised bound and the same
 * measurement is accepted. Without that, an oracle that ignored
 * `clock_worst_offset_ns()` entirely would pass everything here.
 *
 * Injecting an offset makes one CPU's clock jump, which every timestamp
 * subtraction on that CPU then sees -- safe only because step 1's sweep
 * made those subtractions saturate. This test would have been a hazard
 * on the tree as it stood an hour ago.
 */
bool selftest_clock_skew_detected(const char **reason)
{
    if (!clock_is_common()) {
        kinfo("selftest: clock-skew-detected: this machine's clock is not common to every CPU; no bound to test against");
        return true;
    }
    unsigned victim = CONFIG_MAX_CPUS;
    for (unsigned c = 1; c < cpu_count(); c++) {
        if (cpu_online(c)) {
            victim = c;
            break;
        }
    }
    if (victim == CONFIG_MAX_CPUS) {
        kinfo("selftest: clock-skew-detected: one online CPU; skipping");
        return true;
    }

    static const int64_t MAG = 2000000;   /* 2 ms: far wider than any bracket, far short of any watchdog */
    static const int64_t dirs[2] = { MAG, -MAG };
    bool ok = true;

    for (unsigned d = 0; d < 2 && ok; d++) {
        struct bracket w;
        clock_test_set_cpu_offset_ns(victim, dirs[d]);
        bool ran = bracket_all_pairs(&w, reason);
        clock_test_set_cpu_offset_ns(victim, 0);
        if (!ran)
            return false;

        /* The injected magnitude shows up, whichever side of the pair
         * the victim was on: ahead as B puts its reading past t1, ahead
         * as A puts B's reading before t0. The round trip eats a little
         * of it, so the window is generous rather than exact. */
        if (w.worst_outside < (uint64_t)MAG / 2 || w.worst_outside > (uint64_t)MAG * 2) {
            kerror("selftest: clock-skew-detected: %lld ns injected on CPU %u, but the worst reading was only %llu ns outside its bracket",
                   (long long)dirs[d], victim, (unsigned long long)w.worst_outside);
            *reason = "an injected offset did not show up as a reading outside the bracket";
            return false;
        }
        /* ...and it is more than the advertised bound allows, which is
         * what makes clock-cross-cpu reject it. */
        if (w.worst_outside <= clock_worst_offset_ns() + w.at_bracket) {
            *reason = "an injected offset would not have failed the cross-CPU assertion";
            return false;
        }

        /* The converse: advertise a bound wide enough and the same
         * measurement is accepted, so the bound is consulted. */
        clock_test_set_worst_offset_ns((uint64_t)MAG * 2);
        bool accepted = w.worst_outside <= clock_worst_offset_ns() + w.at_bracket;
        clock_test_set_worst_offset_ns(0);
        if (!accepted) {
            *reason = "widening the advertised bound did not accept the measurement: the bound is not being read";
            return false;
        }

        kinfo("selftest: clock-skew-detected: %lld ns on CPU %u read as %llu ns outside the bracket; rejected against a 0 ns bound, accepted against %lld ns",
              (long long)dirs[d], victim, (unsigned long long)w.worst_outside, (long long)MAG * 2);
    }
    return ok;
}

/*
 * What the advertised bound is allowed to be.
 *
 * The first run of the measurement reported an uncertainty of +-0 ns and
 * would have advertised a bound of zero: the narrowest bracket had width
 * zero, the counter not having advanced across a cross-CPU handshake
 * that certainly took real time. A bound of zero is a promise no
 * measurement can make, so the three cases are kept apart here and one
 * of them is not allowed to be produced by measuring:
 *
 *   bound == 0                      only when nothing was measured,
 *                                   because the counter is shared by
 *                                   construction and zero is exact
 *   bound == CLOCK_OFFSET_UNBOUNDED only when the kernel has declined to
 *                                   promise at all
 *   otherwise                       at least one tick of the counter
 */
bool selftest_clock_offset_bound(const char **reason)
{
    uint64_t bound = clock_worst_offset_ns();
    uint64_t res = clock_resolution_ns();

    CHECK(res > 0);

    /*
     * The floor, checked wherever a measurement happened -- including on
     * a machine that measured and then declined to trust the result,
     * which is every x86-64 machine available to this project. Without
     * this the third row below would be unreachable on both
     * architectures and the +-0 ns defect could come back unnoticed.
     */
    if (clock_offsets_measured()) {
        uint64_t m = clock_measured_bound_ns();
        if (m < res) {
            kerror("selftest: clock-offset-bound: the measurement produced %llu ns, finer than the counter's own %llu ns resolution",
                   (unsigned long long)m, (unsigned long long)res);
            *reason = "a measured bound finer than the counter can express";
            return false;
        }
        kinfo("selftest: clock-offset-bound: the measurement produced %llu ns against a %llu ns counter resolution",
              (unsigned long long)m, (unsigned long long)res);
    }

    if (bound == CLOCK_OFFSET_UNBOUNDED) {
        CHECK(!clock_is_common());
        kinfo("selftest: clock-offset-bound: no cross-CPU promise on this machine; the bound is unbounded rather than a number");
        return true;
    }
    CHECK(clock_is_common());

    if (bound == 0) {
        /* Exact, and only because nothing was measured to produce it. */
        if (clock_offsets_measured()) {
            kerror("selftest: clock-offset-bound: a bound of 0 ns was produced by measuring %s",
                   "an offset, which no measurement can justify");
            *reason = "a measured bound of zero: the counter resolution is the floor";
            return false;
        }
        kinfo("selftest: clock-offset-bound: 0 ns, exact -- %s is one counter for the whole system and nothing was measured",
              arch_clock_name());
        return true;
    }

    CHECK(clock_offsets_measured());
    if (bound < res) {
        kerror("selftest: clock-offset-bound: bound %llu ns is finer than the counter's own %llu ns resolution",
               (unsigned long long)bound, (unsigned long long)res);
        *reason = "the advertised bound is finer than the counter can express";
        return false;
    }
    kinfo("selftest: clock-offset-bound: %llu ns, at or above the counter's %llu ns resolution",
          (unsigned long long)bound, (unsigned long long)res);
    return true;
}

/*
 * The gate: a machine whose counter is not common to every CPU must stop
 * promising that two CPUs' readings may be subtracted, and must say so.
 *
 * What this does not test, stated plainly: the CPUID read itself. Every
 * x86-64 machine this project boots on has the invariant-TSC bit set, so
 * `arch_clock_is_common` returning false cannot be produced here -- and
 * on AArch64 the answer is true by architecture. What is tested is
 * everything downstream of the answer, which is the part that can
 * silently rot while the one-line bit read keeps working: that a "no"
 * empties the advertised bound rather than leaving a stale number, that
 * `clock_is_common` reports it, and that the cross-CPU tests then
 * decline to make a claim instead of asserting against UINT64_MAX.
 */
bool selftest_clock_invariant_gate(const char **reason)
{
    /* As booted, the two answers must agree with each other -- an
     * unbounded offset on a clock the kernel calls common, or a number
     * on one it does not, is the gate half-wired. */
    CHECK(clock_is_common() == (clock_worst_offset_ns() != CLOCK_OFFSET_UNBOUNDED));
    bool was_common = clock_is_common();

    clock_test_force_uncommon(true);
    bool common_now = clock_is_common();
    uint64_t bound_now = clock_worst_offset_ns();
    /* The cross-CPU test must decline rather than assert, and it is run
     * here rather than trusted to: a version of it that asserted against
     * an unbounded bound would pass, and pass meaninglessly. */
    const char *sub = NULL;
    bool cross_ok = selftest_clock_cross_cpu(&sub);
    clock_test_force_uncommon(false);

    CHECK(!common_now);
    CHECK(bound_now == CLOCK_OFFSET_UNBOUNDED);
    CHECK(cross_ok);

    /* ...and putting it back restores what the boot decided. */
    CHECK(clock_is_common() == was_common);
    CHECK(clock_is_common() == (clock_worst_offset_ns() != CLOCK_OFFSET_UNBOUNDED));

    kinfo("selftest: clock-invariant-gate: %s is %scommon to every CPU as booted; forcing the gate shut advertises no bound and the cross-CPU claim stands down",
          arch_clock_name(), was_common ? "" : "not ");
    return true;
}

#else

bool selftest_clock_offset_bound(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_clock_invariant_gate(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_clock_skew_detected(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_clock_cross_cpu(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_clock_scope_aarch64(const char **reason)
{
    (void)reason;
    return true;
}

#endif /* CONFIG_DEBUG */
