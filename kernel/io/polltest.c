/*
 * polltest.c - io_poll self-tests (docs/kernel/io/testing.md): readiness
 * without waiting, a timeout, a wake by another thread's write, hangup,
 * ignored entries. The realtime clock's test lives here too (milestone 10).
 */

#include <kernel/errno.h>
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
    CHECK(clock_now_ns() - t0 >= 15 * 1000000ull);

    /* A write by another thread wakes the wait. */
    struct thread *t = thread_create(late_writer, w, "poll-writer", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    t0 = clock_now_ns();
    CHECK(io_poll(fds, 1, IO_POLL_FOREVER) == 1);
    CHECK(fds[0].revents == COSMO_IO_READABLE);
    CHECK(clock_now_ns() - t0 >= 10 * 1000000ull);
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

bool selftest_realtime(const char **reason)
{
    /* The wall clock: set from the RTC at boot (both QEMU machines have
     * one), so a date after 2020 and before 2100; it advances with the
     * monotonic clock. */
    uint64_t rt0 = clock_realtime_ns(), m0 = clock_now_ns();
    CHECK(rt0 / 1000000000ull > 1600000000ull);
    CHECK(rt0 / 1000000000ull < 4102444800ull);
    thread_sleep_ns(5 * 1000000ull);
    uint64_t rt1 = clock_realtime_ns(), m1 = clock_now_ns();
    uint64_t drt = rt1 - rt0, dm = m1 - m0;
    CHECK(drt >= 4 * 1000000ull);
    CHECK(drt <= dm + 1000000ull && dm <= drt + 1000000ull);   /* the same ticks behind both */
    return true;
}
