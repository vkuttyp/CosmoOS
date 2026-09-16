/*
 * clocktest.c - The clock's cross-CPU contract
 * (docs/audit/next-subsystem-cpu-clock.md).
 *
 * `clock_now_ns` promises values that two CPUs may subtract from each
 * other. These tests are what makes that a claim about the machine
 * rather than about the comment above it.
 */

#include <kernel/log.h>
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
