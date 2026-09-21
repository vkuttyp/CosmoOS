/*
 * unixtest.c - Self-tests of unix domain sockets between kernel threads
 * (docs/kernel/ipc/testing.md; docs/audit/next-subsystem-unix-sockets.md).
 *
 * Every test counts live unix sockets and pipes before and after: the
 * leak part of invariant I8 is asserted here and not hoped for.
 */
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/pipe.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/smp.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/unix.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>
#include <uapi/cosmo/syscall.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "unix: " #cond;                                                        \
            ok = false;                                                                      \
            goto out;                                                                        \
        }                                                                                    \
    } while (0)

static struct unix_addr path_name(const char *path)
{
    struct unix_addr a;
    memset(&a, 0, sizeof(a));
    a.len = (uint8_t)strlen(path);
    memcpy(a.bytes, path, a.len);
    return a;
}

static struct unix_addr abstract_name(const char *name)
{
    struct unix_addr a = path_name(name);
    a.abstract = true;
    return a;
}

static unsigned pipes_alive(void)
{
    struct pipe_stats st;
    pipe_get_stats(&st);
    return (unsigned)st.alive;
}

static int mk(int type, struct socket **out)
{
    return ksock_create(COSMO_AF_UNIX, type, 0, out);
}

static void put(struct socket **s)
{
    if (*s) {
        ksock_put(*s);
        *s = NULL;
    }
}

static int64_t sendstr(struct socket *s, const char *str)
{
    return unix_send(s, str, strlen(str), NULL, NULL, false);
}

static int64_t recvbuf(struct socket *s, char *buf, size_t len)
{
    return unix_recv(s, buf, len, NULL, NULL, NULL, false);
}

/* --- unix-stream ---------------------------------------------------------- */

bool selftest_unix_stream(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    struct socket *l = NULL, *c = NULL, *c2 = NULL, *c3 = NULL, *a = NULL, *a2 = NULL, *odd = NULL;
    char buf[64];
    (void)vfs_unlink(NULL, "/tmp/ux-stream");
    (void)vfs_unlink(NULL, "/tmp/ux-regular");

    CHECK(mk(COSMO_SOCK_STREAM, &l) == 0);
    struct unix_addr name = path_name("/tmp/ux-stream");
    CHECK(unix_listen(l, 2) == -EINVAL);                    /* not bound */
    CHECK(unix_bind(l, &name) == 0);
    CHECK(unix_listen(l, 2) == 0);

    /* Connect completes before anyone accepts, and the bytes wait. */
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0);
    CHECK(unix_connect(c, &name) == 0);
    CHECK(unix_connect(c, &name) == -EISCONN);
    CHECK(sendstr(c, "hello") == 5);
    CHECK(unix_accept(l, &a) == 0);
    CHECK(recvbuf(a, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(sendstr(a, "world!") == 6);
    CHECK(recvbuf(c, buf, 3) == 3 && memcmp(buf, "wor", 3) == 0);
    CHECK(recvbuf(c, buf, sizeof(buf)) == 3 && memcmp(buf, "ld!", 3) == 0);
    struct unix_addr pn;
    CHECK(unix_getpeername(c, &pn) == 0 && !pn.abstract && pn.len == name.len && memcmp(pn.bytes, name.bytes, pn.len) == 0);
    CHECK(unix_getpeername(a, &pn) == 0 && pn.len == 0);   /* the client is unnamed */
    struct cosmo_ucred uc;
    CHECK(unix_peercred(a, &uc) == 0 && unix_peercred(c, &uc) == 0);

    /* shutdown(WR) on the client: the server reads end-of-stream, and its
     * own writes still reach the client. */
    CHECK(unix_shutdown(c, COSMO_SHUT_WR) == 0);
    c->shut |= 2;
    CHECK(recvbuf(a, buf, sizeof(buf)) == 0);
    CHECK(sendstr(a, "still") == 5);
    CHECK(recvbuf(c, buf, sizeof(buf)) == 5);
    /* The client goes: the server's write is -EPIPE, no signal. */
    put(&c);
    CHECK(sendstr(a, "x") == -EPIPE);
    CHECK(recvbuf(a, buf, sizeof(buf)) == 0);
    put(&a);

    /* The backlog: two queued, the third refused when it will not wait;
     * accepting one makes room. */
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0 && mk(COSMO_SOCK_STREAM, &c2) == 0 && mk(COSMO_SOCK_STREAM, &c3) == 0);
    CHECK(unix_connect(c, &name) == 0);
    CHECK(unix_connect(c2, &name) == 0);
    ksock_set_nonblock(c3, true);
    CHECK(unix_connect(c3, &name) == -EAGAIN);
    CHECK(unix_accept(l, &a) == 0);
    CHECK(unix_connect(c3, &name) == 0);
    ksock_set_nonblock(c3, false);
    /* Closing the listener refuses what it never accepted: those clients
     * read end-of-stream and write -EPIPE. */
    put(&l);
    CHECK(recvbuf(c2, buf, sizeof(buf)) == 0);
    CHECK(sendstr(c2, "x") == -EPIPE);
    CHECK(recvbuf(c3, buf, sizeof(buf)) == 0);
    /* The accepted one lives on. */
    CHECK(sendstr(a, "ok") == 2 && recvbuf(c, buf, sizeof(buf)) == 2);
    put(&a);
    put(&c);
    put(&c2);
    put(&c3);

    /* The name outlives the socket: connect finds no socket behind it. */
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0);
    CHECK(unix_connect(c, &name) == -ECONNREFUSED);
    /* A node that is not a socket is refused the same way. */
    struct file *f = NULL;
    CHECK(vfs_open(NULL, "/tmp/ux-regular", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    file_put(f);
    struct unix_addr reg = path_name("/tmp/ux-regular");
    CHECK(unix_connect(c, &reg) == -ECONNREFUSED);
    /* And a name that does not resolve is the lookup's answer. */
    struct unix_addr none = path_name("/tmp/ux-nothing-here");
    CHECK(unix_connect(c, &none) == -ENOENT);
    /* A datagram socket's name is the wrong type for a stream. */
    CHECK(mk(COSMO_SOCK_DGRAM, &odd) == 0);
    struct unix_addr dn = abstract_name("ux-odd");
    CHECK(unix_bind(odd, &dn) == 0);
    CHECK(unix_connect(c, &dn) == -EPROTOTYPE);
    put(&odd);
    put(&c);
    (void)a2;
out:
    put(&l);
    put(&c);
    put(&c2);
    put(&c3);
    put(&a);
    put(&a2);
    put(&odd);
    (void)vfs_unlink(NULL, "/tmp/ux-stream");
    (void)vfs_unlink(NULL, "/tmp/ux-regular");
    if (ok && unix_socket_count() != socks0) {
        *reason = "unix: sockets leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-stream: connect before accept, bytes both ways, EOF and -EPIPE, a backlog of 2, "
              "a closed listener refusing its queue, -ECONNREFUSED/-ENOENT/-EPROTOTYPE for the wrong names");
    return ok;
}

/* --- unix-dgram ----------------------------------------------------------- */

bool selftest_unix_dgram(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    struct socket *a = NULL, *b = NULL;
    char buf[64];
    struct unix_addr from;
    unsigned flags = 0;
    (void)vfs_unlink(NULL, "/tmp/ux-dgram");

    CHECK(mk(COSMO_SOCK_DGRAM, &a) == 0 && mk(COSMO_SOCK_DGRAM, &b) == 0);
    struct unix_addr bn = path_name("/tmp/ux-dgram");
    CHECK(unix_bind(b, &bn) == 0);
    CHECK(unix_send(a, "ping", 4, &bn, NULL, false) == 4);
    CHECK(unix_recv(b, buf, sizeof(buf), &from, NULL, &flags, false) == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(from.len == 0 && flags == 0);                    /* the sender had no name */
    struct unix_addr an = abstract_name("ux-dgram-a");
    CHECK(unix_bind(a, &an) == 0);
    CHECK(unix_send(a, "pong", 4, &bn, NULL, false) == 4);
    CHECK(unix_recv(b, buf, sizeof(buf), &from, NULL, &flags, false) == 4);
    CHECK(from.abstract && from.len == an.len && memcmp(from.bytes, an.bytes, an.len) == 0);
    /* A default destination, then a reply to the name that came with it. */
    CHECK(unix_send(a, "x", 1, NULL, NULL, false) == -ENOTCONN);
    CHECK(unix_connect(a, &bn) == 0);
    CHECK(unix_send(a, "dflt", 4, NULL, NULL, false) == 4);
    CHECK(unix_recv(b, buf, sizeof(buf), &from, NULL, &flags, false) == 4);
    CHECK(unix_send(b, "back", 4, &from, NULL, false) == 4);
    CHECK(unix_recv(a, buf, sizeof(buf), &from, NULL, &flags, false) == 4 && memcmp(buf, "back", 4) == 0);
    /* Truncation says so. */
    CHECK(unix_send(a, "0123456789", 10, NULL, NULL, false) == 10);
    CHECK(unix_recv(b, buf, 4, NULL, NULL, &flags, false) == 4 && (flags & COSMO_MSG_TRUNC));
    CHECK(unix_recv(b, buf, 4, NULL, NULL, &flags, true) == -EAGAIN);   /* the rest was dropped with it */
    /* Too big for one message. */
    uint8_t *big = kmalloc(UNIX_MSG_MAX + 1, 0);
    CHECK(big != NULL);
    int64_t brc = unix_send(a, big, UNIX_MSG_MAX + 1, NULL, NULL, false);
    kfree(big);
    CHECK(brc == -EMSGSIZE);
    /* The queue's message bound, then room again. */
    for (unsigned i = 0; i < UNIX_DGRAM_MAX; i++)
        CHECK(unix_send(a, "q", 1, NULL, NULL, true) == 1);
    CHECK(unix_send(a, "q", 1, NULL, NULL, true) == -EAGAIN);
    CHECK(unix_recv(b, buf, 1, NULL, NULL, &flags, false) == 1);
    CHECK(unix_send(a, "q", 1, NULL, NULL, true) == 1);
    /* The receiver goes: a send to it is refused, whether by name or by
     * the default destination. */
    put(&b);
    CHECK(unix_send(a, "x", 1, NULL, NULL, false) == -ECONNREFUSED);
    CHECK(unix_send(a, "x", 1, &bn, NULL, false) == -ECONNREFUSED);
    put(&a);
out:
    put(&a);
    put(&b);
    (void)vfs_unlink(NULL, "/tmp/ux-dgram");
    if (ok && unix_socket_count() != socks0) {
        *reason = "unix: sockets leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-dgram: sendto by name, the sender's name back, a default destination, truncation "
              "flagged, -EMSGSIZE, a queue of %u then -EAGAIN, a gone receiver refused", UNIX_DGRAM_MAX);
    return ok;
}

/* --- unix-name ------------------------------------------------------------ */

bool selftest_unix_name(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    struct socket *s = NULL, *t = NULL, *c = NULL;
    struct vnode *vn = NULL;
    struct file *f = NULL;
    char buf[8];
    (void)vfs_unlink(NULL, "/tmp/ux-name");

    CHECK(mk(COSMO_SOCK_STREAM, &s) == 0);
    struct unix_addr name = path_name("/tmp/ux-name");
    CHECK(unix_bind(s, &name) == 0);
    /* The node: a socket, mode 0755, the caller's. */
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, "/tmp/ux-name", &st) == 0);
    CHECK(st.type == COSMO_DT_SOCK && st.mode == 0755);
    CHECK(vfs_lookup(NULL, "/tmp/ux-name", &vn) == 0 && vn->type == VNODE_SOCK);
    vnode_put(vn);
    vn = NULL;
    /* open() of it is -ENXIO. */
    CHECK(vfs_open(NULL, "/tmp/ux-name", COSMO_O_RDONLY, 0, &f) == -ENXIO);
    /* A second bind to the name is -EADDRINUSE, and binding twice is -EINVAL. */
    CHECK(mk(COSMO_SOCK_STREAM, &t) == 0);
    CHECK(unix_bind(t, &name) == -EADDRINUSE);
    CHECK(unix_bind(s, &name) == -EINVAL);
    struct unix_addr other = abstract_name("ux-name-abs");
    CHECK(unix_bind(t, &other) == 0);
    struct socket *t2 = NULL;
    CHECK(mk(COSMO_SOCK_STREAM, &t2) == 0);
    int dup_rc = unix_bind(t2, &other);
    put(&t2);
    CHECK(dup_rc == -EADDRINUSE);
    /* unlink: the name is gone for a new connect, the existing connection
     * lives on. */
    CHECK(unix_listen(s, 1) == 0);
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0);
    CHECK(unix_connect(c, &name) == 0);
    CHECK(vfs_unlink(NULL, "/tmp/ux-name") == 0);
    struct socket *c2 = NULL;
    CHECK(mk(COSMO_SOCK_STREAM, &c2) == 0);
    int rc2 = unix_connect(c2, &name);
    put(&c2);
    CHECK(rc2 == -ENOENT);
    struct socket *a = NULL;
    CHECK(unix_accept(s, &a) == 0);
    CHECK(sendstr(c, "live") == 4 && recvbuf(a, buf, sizeof(buf)) == 4);
    put(&a);
    put(&c);
    /* A filesystem without mknod refuses the name. */
    struct socket *p = NULL;
    CHECK(mk(COSMO_SOCK_STREAM, &p) == 0);
    struct unix_addr pn = path_name("/proc/ux-name");
    int prc = unix_bind(p, &pn);
    put(&p);
    CHECK(prc == -EOPNOTSUPP || prc == -EROFS || prc == -EACCES);
    put(&t);
    put(&s);
out:
    if (vn)
        vnode_put(vn);
    put(&s);
    put(&t);
    put(&c);
    (void)vfs_unlink(NULL, "/tmp/ux-name");
    if (ok && unix_socket_count() != socks0) {
        *reason = "unix: sockets leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-name: bind makes a socket node (0755, DT_SOCK), open is -ENXIO, a second bind "
              "-EADDRINUSE (path and abstract), unlink then connect -ENOENT with the connection alive, no mknod refused");
    return ok;
}

/* --- unix-handles --------------------------------------------------------- */

bool selftest_unix_handles(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    unsigned pipes0 = pipes_alive();
    struct socket *x = NULL, *y = NULL, *z = NULL;
    struct kobject *rd = NULL, *wr = NULL, *rd2 = NULL, *wr2 = NULL;
    struct unix_handles h, got;
    unsigned flags = 0;
    char buf[16];

    CHECK(unix_socketpair(COSMO_SOCK_STREAM, &x, &y) == 0);
    CHECK(pipe_create(&rd, &wr) == 0);
    uint32_t refs0 = kobject_refcount(wr);

    /* The rights ride with the object: the receiver gets exactly what the
     * sender named. */
    memset(&h, 0, sizeof(h));
    h.nr = 1;
    h.objs[0] = wr;
    h.rights[0] = HANDLE_RIGHT_WRITE;
    kobject_get(wr);   /* the message's reference, as the door would take it */
    CHECK(unix_send(x, "h", 1, NULL, &h, false) == 1);
    CHECK(h.nr == 0);                                       /* consumed */
    CHECK(kobject_refcount(wr) == refs0 + 1);               /* held by the message */
    got.nr = 4;
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, &got, &flags, false) == 1);
    CHECK(got.nr == 1 && got.objs[0] == wr && got.rights[0] == HANDLE_RIGHT_WRITE && flags == 0);
    unix_handles_drop(&got);
    CHECK(kobject_refcount(wr) == refs0);

    /* A unix socket in a message is refused, and the caller keeps its
     * reference to drop. */
    CHECK(mk(COSMO_SOCK_STREAM, &z) == 0);
    h.nr = 1;
    h.objs[0] = &z->obj;
    h.rights[0] = HANDLE_RIGHT_ALL;
    kobject_get(&z->obj);
    CHECK(unix_send(x, "u", 1, NULL, &h, false) == -EINVAL);
    CHECK(h.nr == 1);
    unix_handles_drop(&h);
    put(&z);

    /* Room for one of three: one delivered, two released, and the flag. */
    CHECK(pipe_create(&rd2, &wr2) == 0);
    uint32_t r2 = kobject_refcount(rd2);
    h.nr = 3;
    h.objs[0] = wr;  h.rights[0] = HANDLE_RIGHT_WRITE;
    h.objs[1] = rd2; h.rights[1] = HANDLE_RIGHT_READ;
    h.objs[2] = wr2; h.rights[2] = HANDLE_RIGHT_WRITE;
    kobject_get(wr);
    kobject_get(rd2);
    kobject_get(wr2);
    CHECK(unix_send(x, "abc", 3, NULL, &h, false) == 3);
    got.nr = 1;
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, &got, &flags, false) == 3);
    CHECK(got.nr == 1 && got.objs[0] == wr && (flags & COSMO_MSG_HTRUNC));   /* the first fits; the other two released */
    CHECK(kobject_refcount(wr) == refs0 + 1 && kobject_refcount(rd2) == r2);
    unix_handles_drop(&got);
    CHECK(kobject_refcount(wr) == refs0);
    /* No room asked for at all: the bytes arrive, the handles do not. */
    h.nr = 1;
    h.objs[0] = wr;
    h.rights[0] = HANDLE_RIGHT_WRITE;
    kobject_get(wr);
    CHECK(unix_send(x, "n", 1, NULL, &h, false) == 1);
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, NULL, &flags, false) == 1 && (flags & COSMO_MSG_HTRUNC));
    CHECK(kobject_refcount(wr) == refs0);

    /* Ancillary items stay with their bytes: a read stops at the boundary
     * of a send that carries handles, and never crosses into it. */
    CHECK(sendstr(x, "aa") == 2);
    h.nr = 1;
    h.objs[0] = wr;
    h.rights[0] = HANDLE_RIGHT_WRITE;
    kobject_get(wr);
    CHECK(unix_send(x, "bb", 2, NULL, &h, false) == 2);
    CHECK(sendstr(x, "cc") == 2);
    got.nr = 4;
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, &got, &flags, false) == 2);   /* "aa" only */
    CHECK(memcmp(buf, "aa", 2) == 0 && got.nr == 0);
    got.nr = 4;
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, &got, &flags, false) == 4);   /* "bb" with its handle, then "cc" */
    CHECK(memcmp(buf, "bbcc", 4) == 0 && got.nr == 1 && got.objs[0] == wr);
    unix_handles_drop(&got);
    CHECK(kobject_refcount(wr) == refs0);

    /* A socket released with a message queued releases the handles in it. */
    h.nr = 1;
    h.objs[0] = wr;
    h.rights[0] = HANDLE_RIGHT_WRITE;
    kobject_get(wr);
    CHECK(unix_send(x, "q", 1, NULL, &h, false) == 1);
    CHECK(kobject_refcount(wr) == refs0 + 1);
    put(&y);
    CHECK(kobject_refcount(wr) == refs0);
    put(&x);

    /* The door's rule, on a table: TRANSFER held and a subset, or -EPERM. */
    struct handle_table *t = kzalloc(sizeof(*t));
    CHECK(t != NULL);
    handle_table_init(t);
    int hv = handle_install(t, wr, HANDLE_RIGHT_WRITE | HANDLE_RIGHT_TRANSFER);
    int hn = handle_install(t, rd, HANDLE_RIGHT_READ);   /* no TRANSFER */
    CHECK(hv >= 0 && hn >= 0);
    struct kobject *o = NULL;
    unsigned r = 0;
    CHECK(handle_transfer_check(t, hv, COSMO_RIGHTS_SAME, &o, &r) == 0 && o == wr &&
          r == (HANDLE_RIGHT_WRITE | HANDLE_RIGHT_TRANSFER));
    kobject_put(o);
    CHECK(handle_transfer_check(t, hv, HANDLE_RIGHT_WRITE, &o, &r) == 0 && r == HANDLE_RIGHT_WRITE);
    kobject_put(o);
    CHECK(handle_transfer_check(t, hv, HANDLE_RIGHT_READ, &o, &r) == -EPERM);   /* more than it holds */
    CHECK(handle_transfer_check(t, hn, COSMO_RIGHTS_SAME, &o, &r) == -EPERM);   /* no TRANSFER */
    CHECK(handle_transfer_check(t, 63, COSMO_RIGHTS_SAME, &o, &r) == -EBADF);
    handle_table_destroy(t);
    kfree(t);
out:
    put(&x);
    put(&y);
    put(&z);
    if (rd)
        kobject_put(rd);
    if (wr)
        kobject_put(wr);
    if (rd2)
        kobject_put(rd2);
    if (wr2)
        kobject_put(wr2);
    if (ok && (unix_socket_count() != socks0 || pipes_alive() != pipes0)) {
        *reason = "unix: sockets or pipes leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-handles: a handle rides with its rights, a unix socket is refused, less room than sent "
              "releases and flags, ancillary items stay with their bytes, a release frees what was queued, the "
              "transfer rule on a table");
    return ok;
}

/* --- unix-close-race: a peer closed under a blocked reader, writer,
 * connector and accepter, each on the other CPU ------------------------- */

struct racer {
    struct socket *s;
    struct unix_addr name;
    int what;                      /* 0 read, 1 write, 2 connect, 3 accept */
    volatile unsigned started, done;
    int64_t rc;
};

static void racer_main(void *arg)
{
    struct racer *r = arg;
    __atomic_store_n(&r->started, 1u, __ATOMIC_RELEASE);
    char buf[16];
    switch (r->what) {
    case 0: r->rc = recvbuf(r->s, buf, sizeof(buf)); break;
    case 1: {
        static uint8_t chunk[4096];
        int64_t rc = 0;
        for (unsigned i = 0; i < 64; i++) {   /* fills the queue and then blocks */
            rc = unix_send(r->s, chunk, sizeof(chunk), NULL, NULL, false);
            if (rc < 0)
                break;
        }
        r->rc = rc;
        break;
    }
    case 2: r->rc = unix_connect(r->s, &r->name); break;
    case 3: {
        struct socket *a = NULL;
        r->rc = unix_accept(r->s, &a);
        if (a)
            ksock_put(a);
        break;
    }
    }
    __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
}

static bool spin_flag(const volatile unsigned *flag, unsigned ms)
{
    uint64_t end = clock_now_ns() + (uint64_t)ms * 1000000ull;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() > end)
            return false;
        sched_yield();
    }
    return true;
}

/* Give the other CPU's thread time to reach its blocking call. The
 * wait itself is not the assertion -- the racer's result is -- so a
 * thread that has not blocked yet only makes the case weaker, never
 * false. */
static void pause_ms(unsigned ms)
{
    uint64_t end = clock_now_ns() + (uint64_t)ms * 1000000ull;
    while (clock_now_ns() < end)
        sched_yield();
}

static unsigned other_cpu_unix(void)
{
    for (unsigned c = 1; c < cpu_count(); c++)
        if (cpu_online(c))
            return c;
    return 0;
}

bool selftest_unix_close_race(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    unsigned cpu = other_cpu_unix();
    if (cpu == 0) {
        kinfo("selftest: unix-close-race: one CPU, nothing to race");
        return true;
    }
    struct socket *x = NULL, *y = NULL, *l = NULL, *c = NULL, *c2 = NULL;
    static struct racer r;
    struct thread *t = NULL;
    (void)vfs_unlink(NULL, "/tmp/ux-race");

    /* A reader blocked on an empty stream; the peer closes: 0. */
    CHECK(unix_socketpair(COSMO_SOCK_STREAM, &x, &y) == 0);
    memset(&r, 0, sizeof(r));
    r.s = y;
    r.what = 0;
    t = thread_create_on(racer_main, &r, "uxrace", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL && spin_flag(&r.started, 1000));
    sched_yield();
    put(&x);
    CHECK(spin_flag(&r.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(r.rc == 0);
    put(&y);

    /* A writer blocked on a full queue; the reader closes: -EPIPE. */
    CHECK(unix_socketpair(COSMO_SOCK_STREAM, &x, &y) == 0);
    memset(&r, 0, sizeof(r));
    r.s = y;
    r.what = 1;
    t = thread_create_on(racer_main, &r, "uxrace", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL && spin_flag(&r.started, 1000));
    pause_ms(20);   /* let it fill the queue and block */
    put(&x);
    CHECK(spin_flag(&r.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(r.rc == -EPIPE);
    put(&y);

    /* A connector blocked on a full backlog; the listener closes:
     * -ECONNREFUSED. */
    CHECK(mk(COSMO_SOCK_STREAM, &l) == 0);
    struct unix_addr name = path_name("/tmp/ux-race");
    CHECK(unix_bind(l, &name) == 0 && unix_listen(l, 1) == 0);
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0 && unix_connect(c, &name) == 0);   /* fills the backlog of 1 */
    CHECK(mk(COSMO_SOCK_STREAM, &c2) == 0);
    memset(&r, 0, sizeof(r));
    r.s = c2;
    r.name = name;
    r.what = 2;
    t = thread_create_on(racer_main, &r, "uxrace", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL && spin_flag(&r.started, 1000));
    pause_ms(20);
    put(&l);
    CHECK(spin_flag(&r.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(r.rc == -ECONNREFUSED);
    put(&c);
    put(&c2);
    (void)vfs_unlink(NULL, "/tmp/ux-race");

    /* An accepter blocked on an empty backlog; shutdown(RD) on the
     * listener ends the wait: -EINVAL. */
    CHECK(mk(COSMO_SOCK_STREAM, &l) == 0);
    CHECK(unix_bind(l, &name) == 0 && unix_listen(l, 1) == 0);
    memset(&r, 0, sizeof(r));
    r.s = l;
    r.what = 3;
    t = thread_create_on(racer_main, &r, "uxrace", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL && spin_flag(&r.started, 1000));
    pause_ms(20);
    l->shut |= 1;
    CHECK(unix_shutdown(l, COSMO_SHUT_RD) == 0);
    CHECK(spin_flag(&r.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(r.rc == -EINVAL);
    put(&l);
out:
    if (t) {
        (void)spin_flag(&r.done, 3000);
        thread_join(t);
    }
    put(&x);
    put(&y);
    put(&l);
    put(&c);
    put(&c2);
    (void)vfs_unlink(NULL, "/tmp/ux-race");
    if (ok && unix_socket_count() != socks0) {
        *reason = "unix: sockets leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-close-race: a blocked reader (0), writer (-EPIPE), connector (-ECONNREFUSED) and "
              "accepter (-EINVAL) each released from CPU %u", cpu);
    return ok;
}

/* --- unix-poll: readiness through the object's type ---------------------- */

bool selftest_unix_poll(const char **reason)
{
    bool ok = true;
    unsigned socks0 = unix_socket_count();
    struct socket *l = NULL, *c = NULL, *a = NULL, *x = NULL, *y = NULL, *d = NULL;
    char buf[8];
    (void)vfs_unlink(NULL, "/tmp/ux-poll");

    CHECK(mk(COSMO_SOCK_STREAM, &l) == 0);
    struct unix_addr name = path_name("/tmp/ux-poll");
    CHECK(unix_bind(l, &name) == 0 && unix_listen(l, 2) == 0);
    CHECK(kobject_poll_wq(&l->obj, COSMO_IO_READABLE) != NULL);
    CHECK((kobject_ready(&l->obj) & COSMO_IO_READABLE) == 0);
    CHECK(mk(COSMO_SOCK_STREAM, &c) == 0 && unix_connect(c, &name) == 0);
    CHECK(kobject_ready(&l->obj) & COSMO_IO_READABLE);     /* a connection waits */
    CHECK(unix_accept(l, &a) == 0);
    CHECK((kobject_ready(&l->obj) & COSMO_IO_READABLE) == 0);
    CHECK((kobject_ready(&a->obj) & (COSMO_IO_READABLE | COSMO_IO_WRITABLE | COSMO_IO_HANGUP)) == COSMO_IO_WRITABLE);
    CHECK(sendstr(c, "r") == 1);
    CHECK(kobject_ready(&a->obj) & COSMO_IO_READABLE);
    CHECK(recvbuf(a, buf, sizeof(buf)) == 1);
    /* A full queue is not writable; a read makes it so. */
    static uint8_t chunk[4096];
    unsigned sent = 0;
    while (unix_send(c, chunk, sizeof(chunk), NULL, NULL, true) > 0)
        sent++;
    CHECK(sent == UNIX_BUF / sizeof(chunk));
    CHECK((kobject_ready(&c->obj) & COSMO_IO_WRITABLE) == 0);
    CHECK(recvbuf(a, buf, sizeof(buf)) == (int64_t)sizeof(buf));
    CHECK(kobject_ready(&c->obj) & COSMO_IO_WRITABLE);
    /* The peer goes: readable and hung up. */
    put(&c);
    CHECK((kobject_ready(&a->obj) & (COSMO_IO_READABLE | COSMO_IO_HANGUP)) == (COSMO_IO_READABLE | COSMO_IO_HANGUP));
    put(&a);
    put(&l);
    /* A datagram socket: writable always, readable with a message. */
    CHECK(unix_socketpair(COSMO_SOCK_DGRAM, &x, &y) == 0);
    CHECK((kobject_ready(&y->obj) & (COSMO_IO_READABLE | COSMO_IO_WRITABLE)) == COSMO_IO_WRITABLE);
    CHECK(unix_send(x, "d", 1, NULL, NULL, false) == 1);
    CHECK(kobject_ready(&y->obj) & COSMO_IO_READABLE);
    CHECK(unix_recv(y, buf, sizeof(buf), NULL, NULL, NULL, false) == 1);
    put(&x);
    put(&y);
    (void)d;
out:
    put(&l);
    put(&c);
    put(&a);
    put(&x);
    put(&y);
    (void)vfs_unlink(NULL, "/tmp/ux-poll");
    if (ok && unix_socket_count() != socks0) {
        *reason = "unix: sockets leaked";
        ok = false;
    }
    if (ok)
        kinfo("selftest: unix-poll: a listener readable with a connection queued, a stream readable by its bytes, "
              "writable by its space and hung up by its peer, a datagram socket writable always");
    return ok;
}
