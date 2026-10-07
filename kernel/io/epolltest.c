/*
 * epolltest.c - epoll interest removal on the member's final close
 * (docs/kernel/io/design.md, "epoll"; invariant A9).
 *
 * A registration lives exactly as long as some handle-table slot, in any
 * process, holds its member: closing the last one removes it, without
 * EPOLL_CTL_DEL, as Linux drops an epitem at the file's final close. The
 * tests here drive the kernel API with a handle table of their own, so
 * the property is checked where it lives and not through a door.
 */

#include <kernel/epoll.h>
#include <kernel/errno.h>
#include <kernel/eventfd.h>
#include <kernel/handle.h>
#include <kernel/net/inet.h>
#include <kernel/net/tcp.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
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

#define EPOLL_CLOSE_PORT 6098u

/* Register `obj` (referenced by the table) in `ep` under `fd`, taking the
 * reference the add owns. */
static int add(struct kobject *ep, int fd, struct kobject *obj, unsigned want)
{
    kobject_get(obj);
    int rc = epoll_obj_add(ep, fd, obj, want, 0, (uint64_t)fd, false, false);
    if (rc)
        kobject_put(obj);
    return rc;
}

/* Spin (yielding) until `obj`'s count reads `n`, up to `ms`. */
static bool refcount_settles(struct kobject *obj, uint32_t n, unsigned ms)
{
    uint64_t deadline = clock_now_ns() + (uint64_t)ms * 1000000ull;
    while (kobject_refcount(obj) != n) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* --- a loopback server that accepts one connection and holds it ------------ */

struct hold_server {
    struct netaddr addr;
    struct socket *ls, *c;
    volatile bool listening, accepted, stop, done;
    int result;
};

static void hold_server_thread(void *arg)
{
    struct hold_server *srv = arg;
    srv->result = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &srv->ls);
    if (srv->result == 0)
        srv->result = ksock_bind(srv->ls, &srv->addr);
    if (srv->result == 0)
        srv->result = ksock_listen(srv->ls, 1);
    __atomic_store_n(&srv->listening, true, __ATOMIC_RELEASE);   /* ready, or failed: the test looks at result */
    if (srv->result == 0)
        srv->result = ksock_accept(srv->ls, &srv->c, NULL);
    __atomic_store_n(&srv->accepted, true, __ATOMIC_RELEASE);
    while (!srv->stop)
        thread_sleep_ms(5);
    __atomic_store_n(&srv->done, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

/* --- a waiter blocked in epoll_obj_wait ------------------------------------- */

struct waiter {
    struct kobject *ep;
    struct epoll_ready out[4];
    int64_t rc;
    volatile bool done;
};

static void waiter_thread(void *arg)
{
    struct waiter *w = arg;
    w->rc = epoll_obj_wait(w->ep, w->out, 4, EPOLL_WAIT_FOREVER);
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

bool selftest_epoll_close(const char **reason)
{
    static struct handle_table t;   /* 64 slots: too big for a stack frame */
    handle_table_init(&t);
    struct kobject *ep;
    CHECK(epoll_obj_create(&ep) == 0);
    struct epoll_ready out[4];
    unsigned socks0 = socket_count();

    /* 1. The baseline: a pipe's read end in the set, closed without DEL.
     * The registration must be gone with the close (the test's creator
     * reference the only one left on the end), the set must report nothing
     * for it, and once the test drops its own reference the end is released
     * and the writer learns the reader is gone (-EPIPE). Before this unit
     * the registration kept the end alive: a second reference remained, the
     * set went on evaluating a closed descriptor and the write succeeded. */
    struct kobject *rd, *wr;
    CHECK(pipe_create(&rd, &wr) == 0);
    int h = handle_install(&t, rd, HANDLE_RIGHT_READ);
    CHECK(h >= 0);
    CHECK(add(ep, h, rd, COSMO_IO_READABLE) == 0);
    CHECK(kobject_refcount(rd) == 3);   /* ours, the table's, the registration's */
    CHECK(handle_close(&t, h) == 0);
    CHECK(kobject_refcount(rd) == 1);   /* the registration left with the last descriptor: the baseline */
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    kobject_put(rd);                    /* the end is released ... */
    CHECK(kobject_io_of(wr)->write(wr, "x", 1) == -EPIPE);   /* ... and its writer has no reader */
    kobject_put(wr);

    /* 2. An eventfd, readable, closed without DEL: not reported, released. */
    struct kobject *ev;
    CHECK(eventfd_obj_create(1, false, &ev) == 0);
    h = handle_install(&t, ev, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    CHECK(h >= 0);
    CHECK(add(ep, h, ev, COSMO_IO_READABLE) == 0);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 1 && out[0].fd == h);
    CHECK(handle_close(&t, h) == 0);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);

    /* 3. A TCP socket in the set, closed without DEL: the peer sees the FIN
     * (its connection reaches CLOSE_WAIT) and the socket is released. This is
     * the leak the unit is for: an event loop that relies on Linux's removal
     * never sent a FIN here. */
    static struct hold_server srv;
    memset(&srv, 0, sizeof(srv));
    srv.addr.family = COSMO_AF_INET;
    srv.addr.v4 = INADDR_LOOPBACK_N;
    srv.addr.port = EPOLL_CLOSE_PORT;
    struct thread *st = thread_create(hold_server_thread, &srv, "epoll-hold", 32);
    CHECK(st != NULL);
    for (unsigned i = 0; i < 400 && !__atomic_load_n(&srv.listening, __ATOMIC_ACQUIRE); i++)
        thread_sleep_ms(5);   /* until it listens (bounded at 2 s), not a fixed settle */
    CHECK(srv.listening && srv.result == 0);
    struct socket *c;
    CHECK(ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv.addr) == 0);
    for (unsigned i = 0; i < 200 && !__atomic_load_n(&srv.accepted, __ATOMIC_ACQUIRE); i++)
        thread_sleep_ms(5);
    CHECK(srv.accepted && srv.result == 0 && srv.c != NULL);
    h = handle_install(&t, &c->obj, HANDLE_RIGHT_SOCK_CONNECTED);
    CHECK(h >= 0);
    CHECK(add(ep, h, &c->obj, COSMO_IO_READABLE) == 0);
    CHECK(handle_close(&t, h) == 0);
    CHECK(kobject_refcount(&c->obj) == 1);        /* the registration is gone; ours is the last reference */
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    ksock_put(c);                                 /* ... so this is the close: the FIN goes now */
    bool fin = false;
    for (unsigned i = 0; i < 400 && !fin; i++) {
        fin = tcp_state_of(srv.c->tcp) == TCP_CLOSE_WAIT;
        if (!fin)
            thread_sleep_ms(5);
    }
    CHECK(fin);                                   /* the peer saw the FIN */
    srv.stop = true;
    for (unsigned i = 0; i < 400 && !__atomic_load_n(&srv.done, __ATOMIC_ACQUIRE); i++)
        thread_sleep_ms(5);
    CHECK(srv.done);
    thread_join(st);
    ksock_put(srv.c);
    ksock_put(srv.ls);

    /* 4. dup keeps the registration; the last close removes it. Two slots
     * for one eventfd, registered under the first: closing the first leaves
     * the event reported (under its key), closing the second removes it. */
    CHECK(eventfd_obj_create(1, false, &ev) == 0);
    int h1 = handle_install(&t, ev, HANDLE_RIGHT_READ), h2 = handle_install(&t, ev, HANDLE_RIGHT_READ);
    CHECK(h1 >= 0 && h2 >= 0 && h1 != h2);
    CHECK(add(ep, h1, ev, COSMO_IO_READABLE) == 0);
    CHECK(handle_close(&t, h1) == 0);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 1 && out[0].fd == h1);   /* one descriptor left: still in */
    CHECK(kobject_refcount(ev) == 3);
    CHECK(handle_close(&t, h2) == 0);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);

    /* 5. An add racing the last close: the object has no slot left, so the
     * registration is refused rather than made and never removed. */
    CHECK(eventfd_obj_create(0, false, &ev) == 0);
    h = handle_install(&t, ev, HANDLE_RIGHT_READ);
    CHECK(h >= 0);
    CHECK(handle_close(&t, h) == 0);
    CHECK(add(ep, h, ev, COSMO_IO_READABLE) == -EBADF);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);

    /* 6. The close racing a blocked epoll_wait. A waiter sleeps on two members
     * neither of which is ready; the pipe's end is closed under it. The
     * waiter is woken by the removal, drops its pin on the end, and the end
     * is released while the waiter is still blocked (on the eventfd alone);
     * the eventfd then wakes it with that one event, never the closed one. */
    CHECK(pipe_create(&rd, &wr) == 0);
    CHECK(eventfd_obj_create(0, false, &ev) == 0);
    int hp = handle_install(&t, rd, HANDLE_RIGHT_READ), he = handle_install(&t, ev, HANDLE_RIGHT_READ);
    CHECK(hp >= 0 && he >= 0);
    CHECK(add(ep, hp, rd, COSMO_IO_READABLE) == 0);
    CHECK(add(ep, he, ev, COSMO_IO_READABLE) == 0);
    static struct waiter w;
    memset(&w, 0, sizeof(w));
    w.ep = ep;
    struct thread *wt = thread_create(waiter_thread, &w, "epoll-waiter", 32);
    CHECK(wt != NULL);
    for (unsigned i = 0; i < 200 && __atomic_load_n(&wt->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED; i++)
        thread_sleep_ms(5);
    CHECK(__atomic_load_n(&wt->state, __ATOMIC_ACQUIRE) == THREAD_BLOCKED);   /* asleep with both pinned */
    CHECK(handle_close(&t, hp) == 0);
    CHECK(refcount_settles(rd, 1, 1000));          /* the waiter dropped its pin; the end is ours alone */
    CHECK(!__atomic_load_n(&w.done, __ATOMIC_ACQUIRE));   /* ... and it is still waiting */
    uint64_t one = 1;
    CHECK(kobject_io_of(ev)->write(ev, &one, 8) == 8);
    for (unsigned i = 0; i < 400 && !__atomic_load_n(&w.done, __ATOMIC_ACQUIRE); i++)
        thread_sleep_ms(5);
    CHECK(w.done);
    thread_join(wt);
    CHECK(w.rc == 1 && w.out[0].fd == he);
    CHECK(handle_close(&t, he) == 0);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);
    kobject_put(rd);
    kobject_put(wr);

    /* 7. The set closed while it holds entries, then the member's last close:
     * the release must have taken the entries off the member's list, or this
     * close would walk freed items. */
    struct kobject *ep2;
    CHECK(epoll_obj_create(&ep2) == 0);
    CHECK(eventfd_obj_create(1, false, &ev) == 0);
    h = handle_install(&t, ev, HANDLE_RIGHT_READ);
    int hep = handle_install(&t, ep2, HANDLE_RIGHT_READ);
    CHECK(h >= 0 && hep >= 0);
    CHECK(add(ep2, h, ev, COSMO_IO_READABLE) == 0);
    kobject_put(ep2);                 /* the table's is the last reference */
    CHECK(handle_close(&t, hep) == 0);   /* the set is released here */
    CHECK(kobject_refcount(ev) == 2);    /* ours and the table's: the release dropped the registration's */
    CHECK(handle_close(&t, h) == 0);     /* the member's last close finds no watcher */
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);

    /* 8. A set's own last close with members registered elsewhere is nothing
     * special (nesting is refused, so a set is never a member); and the table
     * destroyed with a registered member closes it like any other. */
    CHECK(eventfd_obj_create(1, false, &ev) == 0);
    h = handle_install(&t, ev, HANDLE_RIGHT_READ);
    CHECK(h >= 0 && add(ep, h, ev, COSMO_IO_READABLE) == 0);
    handle_table_destroy(&t);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);
    kobject_put(ep);
    thread_sleep_ms(50);
    CHECK(socket_count() == socks0);
    return true;
}

/* --- epoll-scale: a wait costs the ready members ------------------------------
 *
 * N eventfds registered, one of them readable: the cost of a wait must not
 * grow with N (A10). Before the epoll-callback unit every wait walked every
 * item (and a blocking one pinned each member and parked an entry on each
 * queue), so the figures grew with N; with readiness by callback a wait
 * walks the ready list. Two shapes: a non-blocking wait that finds the one
 * ready member, and a blocking wait woken by a thread writing it. Medians of
 * 20; the figures are printed so a boot's numbers can be compared, and the
 * 1024-member figures are bounded against the 1-member ones.
 */
#define SCALE_TABLES 16u   /* 16 x 64 slots: room for 1024 members */
#define SCALE_ROUNDS 20u

struct scale_writer {
    struct kobject *ev;
    volatile bool go, done;
    uint64_t wrote_at;
};

static void scale_writer_thread(void *arg)
{
    struct scale_writer *w = arg;
    uint64_t one = 1;
    while (!__atomic_load_n(&w->go, __ATOMIC_ACQUIRE))
        sched_yield();
    thread_sleep_ms(2);   /* the waiter is asleep by now */
    w->wrote_at = clock_now_ns();
    kobject_io_of(w->ev)->write(w->ev, &one, 8);
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

static void sort_u64s(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t x = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* One size: register `n` eventfds, make the first readable, measure. Fills
 * the two medians (ns). The tables and objects are the caller's. */
static bool scale_one(const char **reason, struct handle_table *tabs, struct kobject **evs, unsigned n,
                      uint64_t *poll_ns, uint64_t *wake_ns)
{
    struct kobject *ep;
    CHECK(epoll_obj_create(&ep) == 0);
    struct epoll_ready out[4];
    for (unsigned i = 0; i < n; i++) {
        int h = handle_install(&tabs[i / HANDLE_TABLE_SIZE], evs[i], HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
        CHECK(h >= 0);
        CHECK(add(ep, (int)i, evs[i], COSMO_IO_READABLE) == 0);
    }
    uint64_t one = 1, sink;
    uint64_t t[SCALE_ROUNDS];
    /* A non-blocking wait with one member readable. */
    CHECK(kobject_io_of(evs[0])->write(evs[0], &one, 8) == 8);
    for (unsigned r = 0; r < SCALE_ROUNDS; r++) {
        uint64_t t0 = clock_now_ns();
        int64_t got = epoll_obj_wait(ep, out, 4, 0);
        t[r] = clock_now_ns() - t0;
        CHECK(got == 1 && out[0].fd == 0);
    }
    sort_u64s(t, SCALE_ROUNDS);
    *poll_ns = t[SCALE_ROUNDS / 2];
    CHECK(kobject_io_of(evs[0])->read(evs[0], &sink, 8) == 8);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    /* A blocking wait woken by a writer. */
    static struct scale_writer w;
    for (unsigned r = 0; r < SCALE_ROUNDS; r++) {
        memset(&w, 0, sizeof(w));
        w.ev = evs[0];
        struct thread *th = thread_create(scale_writer_thread, &w, "epoll-scale-w", 32);
        CHECK(th != NULL);
        __atomic_store_n(&w.go, true, __ATOMIC_RELEASE);
        int64_t got = epoll_obj_wait(ep, out, 4, EPOLL_WAIT_FOREVER);
        uint64_t t1 = clock_now_ns();
        thread_join(th);
        CHECK(got == 1 && out[0].fd == 0 && w.done);
        t[r] = t1 - w.wrote_at;
        CHECK(kobject_io_of(evs[0])->read(evs[0], &sink, 8) == 8);
    }
    sort_u64s(t, SCALE_ROUNDS);
    *wake_ns = t[SCALE_ROUNDS / 2];
    for (unsigned i = 0; i < SCALE_TABLES; i++)
        handle_table_destroy(&tabs[i]);   /* the last closes remove every registration */
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    kobject_put(ep);
    for (unsigned i = 0; i < SCALE_TABLES; i++)
        handle_table_init(&tabs[i]);
    return true;
}

bool selftest_epoll_scale(const char **reason)
{
    static struct handle_table tabs[SCALE_TABLES];
    static struct kobject *evs[SCALE_TABLES * HANDLE_TABLE_SIZE];
    static const unsigned sizes[4] = { 1, 16, 256, 1024 };
    uint64_t poll_ns[4], wake_ns[4];
    for (unsigned i = 0; i < SCALE_TABLES; i++)
        handle_table_init(&tabs[i]);
    for (unsigned i = 0; i < SCALE_TABLES * HANDLE_TABLE_SIZE; i++)
        CHECK(eventfd_obj_create(0, false, &evs[i]) == 0);
    bool ok = true;
    for (unsigned k = 0; k < 4 && ok; k++)
        ok = scale_one(reason, tabs, evs, sizes[k], &poll_ns[k], &wake_ns[k]);
    for (unsigned i = 0; i < SCALE_TABLES * HANDLE_TABLE_SIZE; i++)
        kobject_put(evs[i]);
    if (!ok)
        return false;
    kinfo("selftest: epoll-scale: members 1/16/256/1024: non-blocking wait with one ready %llu/%llu/%llu/%llu ns, "
          "blocking wait woken by a writer %llu/%llu/%llu/%llu ns (medians of %u)",
          (unsigned long long)poll_ns[0], (unsigned long long)poll_ns[1], (unsigned long long)poll_ns[2],
          (unsigned long long)poll_ns[3], (unsigned long long)wake_ns[0], (unsigned long long)wake_ns[1],
          (unsigned long long)wake_ns[2], (unsigned long long)wake_ns[3], SCALE_ROUNDS);
    /* Flat: the 1024-member figures within a small factor of the 1-member
     * ones, with an allowance for the clock and the scheduler. The old walk
     * cost hundreds of times more at 1024 (the report's baseline). */
    CHECK(poll_ns[3] <= 8 * poll_ns[0] + 20000);
    CHECK(wake_ns[3] <= 4 * wake_ns[0] + 200000);
    return true;
}

/* --- epoll-wake-race: wakes against DEL and the last close ---------------------
 *
 * A thread on another CPU (where there is one) writes an eventfd in a loop
 * while this thread registers it, waits, removes it, closes its last slot
 * and installs it again: every wake runs the item's callback, and the
 * removal takes it off the queue under the queue's lock before the item is
 * freed, so a callback in flight has finished and none can start (A10).
 * Then the deterministic shape: after a DEL a write of the member must
 * produce nothing in the set -- with the callback left on the queue (the
 * probe's `no-unhook`), the write would link the removed item and the wait
 * would report it.
 */
struct race_writer {
    struct kobject *ev;
    volatile bool stop;
    unsigned writes;
};

static void race_writer_thread(void *arg)
{
    struct race_writer *w = arg;
    uint64_t one = 1;
    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        kobject_io_of(w->ev)->write(w->ev, &one, 8);
        w->writes++;
        sched_yield();
    }
    thread_exit(0);
}

bool selftest_epoll_wake_race(const char **reason)
{
    static struct handle_table t;
    handle_table_init(&t);
    struct kobject *ep, *ev;
    CHECK(epoll_obj_create(&ep) == 0);
    CHECK(eventfd_obj_create(0, false, &ev) == 0);
    struct epoll_ready out[4];
    static struct race_writer w;
    memset(&w, 0, sizeof(w));
    w.ev = ev;
    unsigned ncpu = cpu_count();
    /* The other CPU where there is one: the race is between CPUs. On one
     * CPU the writer yields into us and the shape is interleaving only. */
    cpumask_t mask = ncpu > 1 ? ((cpumask_t)1 << ((raw_cpu_id() + 1) % ncpu)) : CPUMASK_ALL;
    struct thread *th = thread_create_on(race_writer_thread, &w, "epoll-race-w", 32, mask);
    CHECK(th != NULL);
    uint64_t sink;
    unsigned reported = 0;
    for (unsigned i = 0; i < 2000; i++) {
        int h = handle_install(&t, ev, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
        CHECK(h >= 0);
        CHECK(add(ep, h, ev, COSMO_IO_READABLE) == 0);
        int64_t got = epoll_obj_wait(ep, out, 4, 0);
        CHECK(got == 0 || (got == 1 && out[0].fd == h));
        reported += got == 1;
        if (i % 3 == 0)
            CHECK(epoll_obj_del(ep, h) == 0);   /* DEL under the writer */
        CHECK(handle_close(&t, h) == 0);      /* the last slot: removal under the writer (when not DELed) */
        if (i % 7 == 0)
            (void)kobject_io_of(ev)->read(ev, &sink, 8);   /* let the count fall now and then */
    }
    __atomic_store_n(&w.stop, true, __ATOMIC_RELEASE);
    thread_join(th);
    CHECK(w.writes > 0);
    /* The deterministic half. */
    (void)kobject_io_of(ev)->read(ev, &sink, 8);
    int h = handle_install(&t, ev, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    CHECK(h >= 0 && add(ep, h, ev, COSMO_IO_READABLE) == 0);
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);
    CHECK(epoll_obj_del(ep, h) == 0);
    uint64_t one = 1;
    CHECK(kobject_io_of(ev)->write(ev, &one, 8) == 8);   /* a wake of the member after DEL */
    CHECK(epoll_obj_wait(ep, out, 4, 0) == 0);           /* nothing in the set hears it */
    CHECK(handle_close(&t, h) == 0);
    kinfo("selftest: epoll-wake-race: %u writes from %s, %u of 2000 rounds reported the member",
          w.writes, ncpu > 1 ? "another CPU" : "this CPU", reported);
    CHECK(kobject_refcount(ev) == 1);
    kobject_put(ev);
    kobject_put(ep);
    handle_table_destroy(&t);
    return true;
}

/* --- epoll-nest: a set in a set ----------------------------------------------
 *
 * An inner set's wake reaches the outer set through the outer's callback on
 * the inner's queue (what refused nesting before this unit). An eventfd in
 * the inner, written from another thread while the outer blocks: the outer
 * returns the inner's descriptor, the inner returns the eventfd's; drained,
 * neither reports (the level items are re-queued and found not ready). The
 * loop check: the set itself -EINVAL, a loop -ELOOP, a chain of
 * EPOLL_MAX_NESTS sets accepted and one more refused.
 */
struct nest_writer {
    struct kobject *ev;
    volatile bool done;
};

static void nest_writer_thread(void *arg)
{
    struct nest_writer *w = arg;
    uint64_t one = 1;
    thread_sleep_ms(5);
    kobject_io_of(w->ev)->write(w->ev, &one, 8);
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

bool selftest_epoll_nest(const char **reason)
{
    static struct handle_table t;
    handle_table_init(&t);
    struct kobject *outer, *inner, *ev;
    CHECK(epoll_obj_create(&outer) == 0 && epoll_obj_create(&inner) == 0 && eventfd_obj_create(0, false, &ev) == 0);
    int he = handle_install(&t, ev, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    int hi = handle_install(&t, inner, HANDLE_RIGHT_READ);
    int ho = handle_install(&t, outer, HANDLE_RIGHT_READ);
    CHECK(he >= 0 && hi >= 0 && ho >= 0);
    struct epoll_ready out[4];
    CHECK(add(inner, he, ev, COSMO_IO_READABLE) == 0);
    CHECK(add(outer, hi, inner, COSMO_IO_READABLE) == 0);        /* a set as a member */
    CHECK(add(outer, ho, outer, COSMO_IO_READABLE) == -EINVAL);  /* itself */
    CHECK(add(inner, ho, outer, COSMO_IO_READABLE) == -ELOOP);   /* a loop: outer reaches inner */
    CHECK(epoll_obj_wait(outer, out, 4, 0) == 0);
    /* The forwarded wake: the outer blocks, a thread writes the inner's member. */
    static struct nest_writer w;
    memset(&w, 0, sizeof(w));
    w.ev = ev;
    struct thread *th = thread_create(nest_writer_thread, &w, "epoll-nest-w", 32);
    CHECK(th != NULL);
    int64_t got = epoll_obj_wait(outer, out, 4, 1000ull * 1000000ull);
    thread_join(th);
    CHECK(got == 1 && out[0].fd == hi);                           /* the outer saw the inner's event */
    CHECK(epoll_obj_wait(inner, out, 4, 0) == 1 && out[0].fd == he);
    CHECK(epoll_obj_wait(outer, out, 4, 0) == 1 && out[0].fd == hi);   /* level: re-queued while the inner has it */
    uint64_t sink;
    CHECK(kobject_io_of(ev)->read(ev, &sink, 8) == 8);
    CHECK(epoll_obj_wait(inner, out, 4, 0) == 0);                 /* drained: the inner's level item is dropped */
    CHECK(epoll_obj_wait(outer, out, 4, 0) == 0);                 /* ... and so the outer's */
    /* The depth bound: a chain of EPOLL_MAX_NESTS sets, then one more. */
    unsigned maxn = epoll_obj_max_nests();
    static struct kobject *chain[8];
    static int hc[8];
    CHECK(maxn <= 8);
    for (unsigned i = 0; i < maxn; i++) {
        CHECK(epoll_obj_create(&chain[i]) == 0);
        hc[i] = handle_install(&t, chain[i], HANDLE_RIGHT_READ);
        CHECK(hc[i] >= 0);
    }
    for (unsigned i = 1; i < maxn; i++)
        CHECK(add(chain[i - 1], hc[i], chain[i], COSMO_IO_READABLE) == 0);   /* chain[0] holds chain[1] holds ... */
    struct kobject *extra;
    CHECK(epoll_obj_create(&extra) == 0);
    int hx = handle_install(&t, extra, HANDLE_RIGHT_READ);
    CHECK(hx >= 0);
    CHECK(add(chain[maxn - 1], hx, extra, COSMO_IO_READABLE) == -ELOOP);   /* a chain of maxn + 1 */
    CHECK(add(extra, hc[0], chain[0], COSMO_IO_READABLE) == -ELOOP);       /* from above, the same chain */
    CHECK(add(chain[maxn - 1], hc[0], chain[0], COSMO_IO_READABLE) == -ELOOP);   /* a loop at the bottom */
    /* An event at the bottom of the chain reaches the top. */
    int hev = handle_install(&t, ev, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    CHECK(hev >= 0 && add(chain[maxn - 1], hev, ev, COSMO_IO_READABLE) == 0);
    memset(&w, 0, sizeof(w));
    w.ev = ev;
    th = thread_create(nest_writer_thread, &w, "epoll-nest-w", 32);
    CHECK(th != NULL);
    got = epoll_obj_wait(chain[0], out, 4, 1000ull * 1000000ull);
    thread_join(th);
    CHECK(got == 1 && out[0].fd == hc[1]);
    CHECK(kobject_io_of(ev)->read(ev, &sink, 8) == 8);
    handle_table_destroy(&t);   /* every set and the eventfd: the last closes remove the registrations */
    CHECK(kobject_refcount(ev) == 1 && kobject_refcount(inner) == 1 && kobject_refcount(outer) == 1);
    kobject_put(ev);
    kobject_put(inner);
    kobject_put(outer);
    kobject_put(extra);
    for (unsigned i = 0; i < maxn; i++)
        kobject_put(chain[i]);
    return true;
}

/* --- epoll-close-bench: the close path's cost (reports only) -------------------
 *
 * One thread per CPU (at most four), each with its own table and eventfd,
 * in a loop, three shapes: the last close of an object never registered
 * (before this unit it took the epoll watch lock, now it reads a flag); a
 * close that is not the last (a slot elsewhere holds the object: no
 * registration work on either tree); and install, ADD to a set, last close
 * -- the removal path, which takes the lock and walks the watchers. Closes
 * per second, for the report.
 */
struct close_worker {
    struct handle_table t;
    struct kobject *ev;
    struct kobject *ep;       /* the removal shape: registered here before each close */
    unsigned rounds;
    volatile bool done;
    bool ok;
};

static void close_worker_thread(void *arg)
{
    struct close_worker *w = arg;
    w->ok = true;
    for (unsigned i = 0; i < w->rounds; i++) {
        int h = handle_install(&w->t, w->ev, HANDLE_RIGHT_READ);
        if (h < 0) {
            w->ok = false;
            break;
        }
        if (w->ep != NULL && add(w->ep, h, w->ev, COSMO_IO_READABLE) != 0) {
            w->ok = false;
            break;
        }
        handle_close(&w->t, h);
    }
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

enum { BENCH_LAST, BENCH_NOT_LAST, BENCH_REMOVAL };

static bool close_bench(const char **reason, struct close_worker *ws, unsigned n, int shape, unsigned rounds,
                        uint64_t *per_s)
{
    static struct handle_table keep;
    struct kobject *eps[4] = { NULL, NULL, NULL, NULL };
    sched_watchdog_kick();   /* three shapes back to back, the removal one slow under TCG */
    for (unsigned i = 0; i < n; i++)
        ws[i].rounds = rounds;
    if (shape == BENCH_NOT_LAST) {
        /* A slot elsewhere holds each object: the loop's closes are never
         * the last. */
        handle_table_init(&keep);
        for (unsigned i = 0; i < n; i++)
            CHECK(handle_install(&keep, ws[i].ev, HANDLE_RIGHT_READ) >= 0);
    }
    for (unsigned i = 0; i < n; i++) {
        if (shape == BENCH_REMOVAL)
            CHECK(epoll_obj_create(&eps[i]) == 0);   /* one set per worker: the removal, not set contention */
        ws[i].ep = eps[i];
    }
    struct thread *th[4];
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < n; i++) {
        ws[i].done = false;
        th[i] = thread_create_on(close_worker_thread, &ws[i], "epoll-close-w", 32, (cpumask_t)1 << i);
        CHECK(th[i] != NULL);
    }
    for (unsigned i = 0; i < n; i++)
        thread_join(th[i]);
    uint64_t ns = clock_now_ns() - t0;
    *per_s = ns ? (uint64_t)n * rounds * 1000000000ull / ns : 0;
    for (unsigned i = 0; i < n; i++) {
        CHECK(ws[i].ok);
        if (eps[i] != NULL)
            kobject_put(eps[i]);
    }
    if (shape == BENCH_NOT_LAST)
        handle_table_destroy(&keep);
    return true;
}

bool selftest_epoll_close_bench(const char **reason)
{
    static struct close_worker ws[4];
    unsigned n = cpu_count() < 4 ? cpu_count() : 4;
    for (unsigned i = 0; i < n; i++) {
        handle_table_init(&ws[i].t);
        CHECK(eventfd_obj_create(0, false, &ws[i].ev) == 0);
    }
    /* The rates are the figures; the round counts keep each shape well
     * inside the per-test budget (the removal is tens of microseconds a
     * round under TCG, with the watch lock contended on the old tree). */
    uint64_t last = 0, not_last = 0, removal = 0;
    bool ok = close_bench(reason, ws, n, BENCH_LAST, 20000, &last) &&
              close_bench(reason, ws, n, BENCH_NOT_LAST, 20000, &not_last) &&
              close_bench(reason, ws, n, BENCH_REMOVAL, 2000, &removal);
    for (unsigned i = 0; i < n; i++) {
        handle_table_destroy(&ws[i].t);
        kobject_put(ws[i].ev);
    }
    if (!ok)
        return false;
    kinfo("selftest: epoll-close-bench: %u CPUs x 20000 rounds: last close of a never-registered object %llu/s, "
          "a close that is not the last %llu/s; x 2000 rounds: install+add+last close (the removal) %llu/s",
          n, (unsigned long long)last, (unsigned long long)not_last, (unsigned long long)removal);
    return true;
}
