/*
 * polltest.c - io_poll self-tests (docs/kernel/io/testing.md): readiness
 * without waiting, a timeout, a wake by another thread's write, hangup,
 * ignored entries. The realtime clock's test lives here too (milestone 10).
 */

#include <arch/cpu.h>
#include <kernel/percpu.h>
#include <kernel/smp.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/poll.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <uapi/cosmo/syscall.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

static void late_writer(void *arg)
{
    struct kobject *w = arg;
    thread_sleep_ns(20 * 1000000ull);
    kobject_io_of(w)->write(w, "abc", 3);
}

bool selftest_io_poll(const char **reason)
{
    struct kobject *r, *w;
    CHECK(pipe_create(&r, &w) == 0);

    /* Nothing to read, room to write: decided without waiting. */
    struct io_pollfd fds[3] = {
        { .obj = r, .events = COSMO_IO_READABLE },
        { .obj = w, .events = COSMO_IO_WRITABLE },
        { .obj = NULL, .events = COSMO_IO_READABLE },   /* ignored */
    };
    CHECK(io_poll(fds, 3, 0) == 1);
    CHECK(fds[0].revents == 0 && fds[1].revents == COSMO_IO_WRITABLE && fds[2].revents == 0);
    CHECK(io_poll(fds, 1, 0) == 0);

    /* A timeout passes with nothing ready. */
    uint64_t t0 = clock_now_ns();
    CHECK(io_poll(fds, 1, 20 * 1000000ull) == 0);
    CHECK(clock_since_ns(t0) >= 15 * 1000000ull);

    /* A write by another thread wakes the wait. */
    struct thread *t = thread_create(late_writer, w, "poll-writer", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    t0 = clock_now_ns();
    CHECK(io_poll(fds, 1, IO_POLL_FOREVER) == 1);
    CHECK(fds[0].revents == COSMO_IO_READABLE);
    CHECK(clock_since_ns(t0) >= 10 * 1000000ull);
    thread_join(t);
    char buf[8];
    CHECK(kobject_io_of(r)->read(r, buf, sizeof(buf)) == 3);
    CHECK(io_poll(fds, 1, 0) == 0);

    /* The writer gone: readable (EOF) and hung up, reported even when only
     * READABLE was asked for. */
    kobject_put(w);
    fds[0].events = COSMO_IO_READABLE;
    CHECK(io_poll(fds, 1, 0) == 1);
    CHECK((fds[0].revents & COSMO_IO_HANGUP) && (fds[0].revents & COSMO_IO_READABLE));
    kobject_put(r);
    return true;
}

/* Both clocks, read close enough together that comparing them is exact.
 * The wall clock is the monotonic clock plus an offset (timer.c), so a
 * pair read `gap` apart is off the true pair by at most `gap`. A pair
 * whose bracketing monotonic reads are more than CLOCK_PAIR_NS apart was
 * interrupted -- a tick, or the host holding the vCPU -- and is read
 * again; a thousand interrupted reads in a row is a failure worth
 * seeing. On a quiet host the first read is a few hundred nanoseconds. */
#define CLOCK_PAIR_NS (100ull * 1000)

static bool clock_pair(uint64_t *rt, uint64_t *mono)
{
    for (int tries = 0; tries < 1000; tries++) {
        uint64_t m0 = clock_now_ns();
        uint64_t r = clock_realtime_ns();
        uint64_t m1 = clock_now_ns();
        if (m1 - m0 <= CLOCK_PAIR_NS) {
            *rt = r;
            *mono = m0;
            return true;
        }
    }
    return false;
}

bool selftest_realtime(const char **reason)
{
    /* The wall clock: set from the RTC at boot (both QEMU machines have
     * one), so a date after 2020 and before 2100; it advances with the
     * monotonic clock. */
    uint64_t rt0, m0, rt1, m1;
    CHECK(clock_pair(&rt0, &m0));
    CHECK(rt0 / 1000000000ull > 1600000000ull);
    CHECK(rt0 / 1000000000ull < 4102444800ull);
    thread_sleep_ns(5 * 1000000ull);
    CHECK(clock_pair(&rt1, &m1));
    uint64_t drt = rt1 - rt0, dm = m1 - m0;
    CHECK(drt >= 4 * 1000000ull);
    /* The same ticks behind both: each pair was read within CLOCK_PAIR_NS,
     * so the two advances differ by at most that. This was a 1 ms
     * tolerance on pairs read back to back, which held only while nothing
     * -- a tick, the host -- landed between the two reads of a pair; the
     * bracket makes that a re-read instead of a failure, and the
     * tolerance ten times tighter. */
    CHECK(drt <= dm + CLOCK_PAIR_NS && dm <= drt + CLOCK_PAIR_NS);
    return true;
}

/*
 * poll-timer-outlives: io_poll's timer and its argument live in io_poll's
 * frame, and the callback runs on the CPU that armed it; io_poll must not
 * return while that callback still runs, or the callback writes into a
 * frame that is gone. The callback is held (after it marks the alarm
 * fired) on CPU A; a write wakes the poller and it is moved to CPU B, where
 * it finds the pipe readable and returns -- and must wait there, in the
 * timer's synchronous cancel, until the callback is let go. It used to
 * return at once: the cancel did not wait for a running callback, and
 * under scp and sftp traffic that corrupted the run queue. Needs three
 * CPUs: this test's, A and B (the poller spins in the cancel, so B cannot
 * be this test's CPU). The poller spins without calls once io_poll has
 * returned, so the dead frame below it is not reused while the old code
 * is shown failing.
 */
#if CONFIG_SELFTEST
struct pto {
    struct kobject *r;
    volatile int started, returned, release, rc;
    volatile unsigned done_at_return;
};

static void pto_poller(void *arg)
{
    struct pto *p = arg;
    struct io_pollfd fd = { .obj = p->r, .events = COSMO_IO_READABLE };
    p->started = 1;
    p->rc = io_poll(&fd, 1, 20 * 1000000ull);
    p->done_at_return = poll_test_alarm_done();
    p->returned = 1;
    while (!p->release)
        arch_cpu_relax();
}

static unsigned other_cpu(unsigned not_a, unsigned not_b)
{
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        if (cpu_online(c) && c != not_a && c != not_b)
            return c;
    return CONFIG_MAX_CPUS;
}
#endif

bool selftest_poll_timer_outlives(const char **reason)
{
#if !CONFIG_SELFTEST
    (void)reason;
    return true;
#else
    cpumask_t was = thread_pin_self();
    unsigned x = arch_cpu_id();
    unsigned a = other_cpu(x, x);
    unsigned b = a < CONFIG_MAX_CPUS ? other_cpu(x, a) : CONFIG_MAX_CPUS;
    if (b >= CONFIG_MAX_CPUS) {
        thread_set_affinity_self(was);
        kinfo("selftest: poll-timer-outlives: needs three online CPUs; skipping");
        return true;
    }
    struct kobject *r, *w;
    CHECK(pipe_create(&r, &w) == 0);
    static struct pto p;
    p = (struct pto){ .r = r };
    poll_test_alarm_hold(true);
    struct thread *t = thread_create_on(pto_poller, &p, "poll-outlives", SCHED_PRIO_DEFAULT, (cpumask_t)1 << a);
    CHECK(t != NULL);
    thread_get(t);
    uint64_t until = clock_now_ns() + 2000000000ull;
    while (poll_test_alarm_in() == 0 && clock_now_ns() < until)
        thread_sleep_ms(1);
    bool held = poll_test_alarm_in() == 1;
    enum sched_migrate_result mr = SCHED_MIGRATE_RESULT_COUNT;
    bool early = false;
    if (held) {
        /* Wake the poller (READY on A, whose CPU is held in the callback)
         * and move it to B, where it can run and return. */
        thread_set_affinity(t, ((cpumask_t)1 << a) | ((cpumask_t)1 << b));
        kobject_io_of(w)->write(w, "x", 1);
        for (int i = 0; i < 100 && mr != SCHED_MIGRATED; i++) {
            mr = sched_migrate(t, b);
            if (mr != SCHED_MIGRATED)
                thread_sleep_ms(1);
        }
        /* With the callback still held, the poller must not have returned. */
        uint64_t look = clock_now_ns() + 30000000ull;
        while (clock_now_ns() < look && !p.returned)
            thread_sleep_ms(1);
        early = p.returned != 0;
    }
    poll_test_alarm_hold(false);
    until = clock_now_ns() + 2000000000ull;
    while (!p.returned && clock_now_ns() < until)
        thread_sleep_ms(1);
    p.release = 1;
    thread_join(t);
    thread_put(t);
    kobject_put(r);
    kobject_put(w);
    thread_set_affinity_self(was);
    CHECK(held);
    CHECK(mr == SCHED_MIGRATED);
    CHECK(!early);                      /* io_poll waited for the callback it had armed */
    CHECK(p.returned && p.done_at_return == 1);
    CHECK(p.rc == 1);                   /* the pipe was readable */
    kinfo("selftest: poll-timer-outlives: the poller on cpu %u waited for its timer's callback on cpu %u", b, a);
    return true;
#endif
}
