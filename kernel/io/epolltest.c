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
    volatile bool accepted, stop, done;
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
    thread_sleep_ms(20);   /* let it listen */
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
