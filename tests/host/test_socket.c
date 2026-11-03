/*
 * test_socket.c - The socket layer (kernel-services/network/socket.c) on the
 * host: roadmap 2.0 SA6. A digest over seeded histories, recorded from the C,
 * of up to eight sockets of every family and type:
 *
 *   - sockets created (families and types refused, allocations failing, raw
 *     sockets with and without PRIV_NET), bound (reserved ports, the wrong
 *     family), listened on, accepted from, connected (blocking and not, in
 *     progress, refused, already connected), sent on and received from
 *     (blocking, non-blocking and through an I/O ring, short sends, end of
 *     stream, errors), shut down, named, polled and closed, also through
 *     their kobject's I/O operations;
 *   - asynchronous errors set, read once, peeked and delivered with fresh
 *     and stale tokens; wake references and the debug build's held wake.
 *
 * mbuf.c is real. The transports (TCP, UDP, raw and unix), the credentials,
 * the mutexes, kobjects and the waits are the test's: a blocking wait asks
 * its condition, and between asks the seeded world moves -- a connection
 * completes or is refused, data or a datagram arrives, room opens, an error
 * or a shutdown lands, or the waiter is killed. Every socket, pcb and mbuf is
 * freed and no mutex held at the end.
 */

#include "harness.h"

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/mutex.h>
#include <kernel/net/raw.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/object.h>
#include <kernel/percpu.h>
#include <kernel/socket.h>
#include <kernel/thread.h>
#include <kernel/unix.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (socket.c, 2026-11-03), which the CeeSafe port gives. */
#define SOCKET_DIGEST 0xc27cb34a9bd69c1dull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B001Bull
#define NSOCK 8u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix32(uint32_t v) { mix(&v, sizeof(v)); }
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static void note(uint32_t which, uint64_t value)
{
    mix32(0x6000u + which);
    mix64(value);
}

/* ---- memory: mbuf.c's caches and kzalloc over malloc, an allocation failing when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

static bool fail_now(void) { return g_fail_in >= 0 && g_fail_in-- == 0; }

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align)
{
    (void)align;
    struct kmem_cache *c = calloc(1, sizeof(*c));
    c->name = name;
    c->object_size = size;
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags)
{
    (void)flags;
    g_live++;
    return malloc(c->object_size);
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
    (void)c;
    if (p != NULL)
        g_live--;
    free(p);
}

void *kzalloc(size_t size)
{
    if (fail_now())
        return NULL;
    g_live++;
    return calloc(1, size);
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the thread, its credentials ---- */

static struct thread g_thread;
static struct percpu g_percpu = { .current = &g_thread };

struct percpu *arch_percpu_get(void) { return &g_percpu; }

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

/* ---- kobjects, mutexes, wait queues ---- */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    obj->type = type;
    obj->refcount = 1;
}

void kobject_get(struct kobject *obj) { obj->refcount++; }

bool kobject_tryget(struct kobject *obj)
{
    if (obj->refcount == 0)
        return false;
    obj->refcount++;
    return true;
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    if (--obj->refcount == 0)
        obj->type->release(obj);
}

static const struct mutex *g_held[4];
static unsigned g_nheld;

void mutex_init(struct mutex *m, const char *name)
{
    (void)m;
    (void)name;
}

void mutex_lock(struct mutex *m)
{
    for (unsigned i = 0; i < g_nheld; i++)
        EXPECT(g_held[i] != m);   /* one thread: a second lock would deadlock */
    EXPECT(g_nheld < 4);
    g_held[g_nheld++] = m;
}

void mutex_unlock(struct mutex *m)
{
    for (unsigned i = 0; i < g_nheld; i++)
        if (g_held[i] == m) {
            g_held[i] = g_held[--g_nheld];
            return;
        }
    EXPECT(!"unlocking a mutex not held");
}

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    (void)wq;
    (void)name;
}

unsigned waitqueue_wake_all(struct waitqueue *wq)
{
    (void)wq;
    note(1, 0);
    return 0;
}

/* ---- the transports ---- */

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

static uint64_t g_policy;   /* bits drawn per operation: what the transports answer */

static bool refuses(unsigned bit) { return (g_policy >> bit) & 1; }

/* A TCP pcb and what the test's TCP holds for it. */
struct tstub {
    struct tcp_pcb pcb;
    uint32_t send_space, recv_avail, backlog;
    uint32_t sent;
};

static struct tstub *tstub(struct tcp_pcb *pcb) { return container_of(pcb, struct tstub, pcb); }

static uint32_t g_pattern;

static struct tcp_pcb *new_pcb(uint16_t family)
{
    struct tstub *t = calloc(1, sizeof(*t));
    g_live++;
    t->pcb.state = TCP_CLOSED;
    t->pcb.local.family = family;
    t->pcb.remote.family = family;
    t->send_space = (uint32_t)(next() % 3000);
    return &t->pcb;
}

struct tcp_pcb *tcp_pcb_new(uint16_t family)
{
    if (fail_now())
        return NULL;
    return new_pcb(family);
}

void tcp_close(struct tcp_pcb *pcb)
{
    note(10, pcb->state);
    g_live--;
    free(tstub(pcb));
}

int tcp_bind(struct tcp_pcb *pcb, const struct netaddr *local)
{
    if (refuses(1))
        return -EADDRINUSE;
    pcb->local = *local;
    if (pcb->local.port == 0)
        pcb->local.port = 40000;
    return 0;
}

int tcp_listen(struct tcp_pcb *pcb, unsigned backlog)
{
    note(11, backlog);
    if (refuses(2))
        return -ENOMEM;
    pcb->state = TCP_LISTEN;
    return 0;
}

bool tcp_accept_ready(struct tcp_pcb *pcb) { return tstub(pcb)->backlog > 0; }

struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb, struct socket *owner)
{
    struct tstub *t = tstub(pcb);
    if (t->backlog == 0)
        return NULL;
    t->backlog--;
    struct tcp_pcb *c = new_pcb(pcb->local.family);
    c->sock = owner;
    c->state = TCP_ESTABLISHED;
    c->local = pcb->local;
    c->remote.port = (uint16_t)(1000 + next() % 50);
    c->remote.v4 = A(10, 0, 2, (uint8_t)(2 + next() % 5));
    tstub(c)->recv_avail = (uint32_t)(next() % 200);
    return c;
}

int tcp_connect(struct tcp_pcb *pcb, const struct netaddr *remote)
{
    if (pcb->error)
        return pcb->error;   /* a dead connection stays dead: its error is sticky (tcp.h) */
    pcb->remote = *remote;
    switch (next() % 6) {
    case 0: return -ENETUNREACH;
    case 1: pcb->state = TCP_ESTABLISHED; return 0;
    case 2:
        pcb->state = TCP_CLOSED;
        if (next() % 2)
            __atomic_store_n(&pcb->error, -ECONNREFUSED, __ATOMIC_RELEASE);   /* else closed with no word why */
        return 0;
    default: pcb->state = TCP_SYN_SENT; return 0;
    }
}

int64_t tcp_send(struct tcp_pcb *pcb, const void *data, size_t len)
{
    struct tstub *t = tstub(pcb);
    if (pcb->error)
        return pcb->error;
    if (refuses(3))
        return -EPIPE;
    uint32_t n = len < t->send_space ? (uint32_t)len : t->send_space;
    if (n == 0)
        return refuses(4) ? -EAGAIN : 0;
    mix(data, n < 32 ? n : 32);
    t->send_space -= n;
    t->sent += n;
    note(12, n);
    return n;
}

uint32_t tcp_send_space(struct tcp_pcb *pcb) { return tstub(pcb)->send_space; }

int64_t tcp_recv(struct tcp_pcb *pcb, void *data, size_t len, bool *peer_closed)
{
    struct tstub *t = tstub(pcb);
    if (t->recv_avail == 0) {
        if (pcb->error && refuses(5))
            return pcb->error;
        if (pcb->fin_rcvd)
            *peer_closed = true;
        return 0;
    }
    uint32_t n = len < t->recv_avail ? (uint32_t)len : t->recv_avail;
    uint8_t *b = data;
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)(g_pattern++ * 31u);
    t->recv_avail -= n;
    return n;
}

uint32_t tcp_recv_avail(struct tcp_pcb *pcb) { return tstub(pcb)->recv_avail; }

enum tcp_state tcp_state_of(struct tcp_pcb *pcb) { return pcb->state; }

int tcp_shutdown_write(struct tcp_pcb *pcb)
{
    note(13, pcb->state);
    return refuses(6) ? -ENOTCONN : 0;
}

unsigned tcp_ready(struct tcp_pcb *pcb)
{
    struct tstub *t = tstub(pcb);
    unsigned r = 0;
    if (t->recv_avail > 0 || pcb->fin_rcvd || t->backlog > 0)
        r |= COSMO_IO_READABLE;
    if (pcb->state == TCP_ESTABLISHED && t->send_space > 0)
        r |= COSMO_IO_WRITABLE;
    if (pcb->error)
        r |= COSMO_IO_ERROR;
    return r;
}

int udp_pcb_init(struct udp_pcb *pcb, uint16_t family)
{
    memset(pcb, 0, sizeof(*pcb));
    pcb->local.family = family;
    pcb->remote.family = family;
    mbufq_init(&pcb->rxq, 8, "udp-rxq");
    return 0;
}

void udp_unbind(struct udp_pcb *pcb)
{
    note(20, pcb->local.port);
    pcb->local.port = 0;
    mbufq_drain(&pcb->rxq);
}

int udp_bind(struct udp_pcb *pcb, const struct netaddr *local)
{
    if (refuses(1))
        return -EADDRINUSE;
    pcb->local = *local;
    if (pcb->local.port == 0)
        pcb->local.port = 50000;
    return 0;
}

void udp_set_remote(struct udp_pcb *pcb, const struct netaddr *remote) { pcb->remote = *remote; }

int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len, const struct netaddr *to)
{
    mix(to, sizeof(*to));
    mix(data, len < 32 ? len : 32);
    note(21, len);
    if (refuses(3))
        return -ENETUNREACH;
    if (pcb->local.port == 0)
        pcb->local.port = 50001;
    return 0;
}

struct mbuf *udp_recv(struct udp_pcb *pcb) { return mbufq_dequeue(&pcb->rxq); }

struct raw_pcb *raw_pcb_new(struct socket *sock, uint8_t proto)
{
    if (fail_now())
        return NULL;
    struct raw_pcb *p = calloc(1, sizeof(*p));
    g_live++;
    p->proto = proto;
    p->sock = sock;
    mbufq_init(&p->rxq, 8, "raw-rxq");
    sock->raw = p;
    return p;
}

void raw_pcb_free(struct raw_pcb *pcb)
{
    if (pcb == NULL)
        return;
    mbufq_drain(&pcb->rxq);
    g_live--;
    free(pcb);
}

int raw_sendto(struct raw_pcb *pcb, const void *data, size_t len, const struct netaddr *to)
{
    (void)pcb;
    if (to == NULL)
        return -EDESTADDRREQ;
    mix(data, len < 32 ? len : 32);
    note(22, len);
    return refuses(3) ? -ENETUNREACH : 0;
}

struct mbuf *raw_recv(struct raw_pcb *pcb) { return mbufq_dequeue(&pcb->rxq); }

int unix_create(struct socket *s)
{
    (void)s;
    return refuses(7) ? -ENOMEM : 0;
}

void unix_release(struct socket *s)
{
    (void)s;
    note(30, 0);
}

int unix_listen(struct socket *s, int backlog)
{
    (void)s;
    note(31, (uint32_t)backlog);
    return refuses(2) ? -EINVAL : 0;
}

int unix_accept(struct socket *s, struct socket **out)
{
    (void)s;
    (void)out;
    return refuses(2) ? -EINVAL : -EAGAIN;
}

int64_t unix_send(struct socket *s, const void *buf, size_t len, const struct unix_addr *to, struct unix_handles *h,
                  bool dontwait)
{
    (void)s;
    (void)to;
    (void)h;
    mix(buf, len < 32 ? len : 32);
    note(32, ((uint64_t)dontwait << 32) | len);
    return refuses(3) ? -EPIPE : (int64_t)len;
}

int64_t unix_recv(struct socket *s, void *buf, size_t len, struct unix_addr *from, struct unix_handles *h,
                  unsigned *flags, bool dontwait)
{
    (void)s;
    (void)from;
    (void)h;
    (void)flags;
    note(33, ((uint64_t)dontwait << 32) | len);
    size_t n = len < 7 ? len : 7;
    memcpy(buf, "unixdgm", n);
    return refuses(3) ? -EAGAIN : (int64_t)n;
}

int unix_shutdown(struct socket *s, int how)
{
    (void)s;
    note(34, (uint32_t)how);
    return 0;
}

unsigned unix_ready(struct socket *s)
{
    (void)s;
    return (unsigned)(g_policy >> 8) & (COSMO_IO_READABLE | COSMO_IO_WRITABLE);
}

/* ---- the world, which moves while a caller waits ---- */

static struct mbuf *datagram(void)
{
    struct mbuf *m = m_getcl();
    EXPECT(m != NULL);
    uint8_t b[64];
    uint32_t n = (uint32_t)(next() % sizeof(b));
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
    m_append(m, b, n);
    m->pkt.src.family = COSMO_AF_INET;
    m->pkt.src.port = (uint16_t)(next() % 100);
    m->pkt.src.v4 = A(10, 0, 2, 9);
    return m;
}

static void world_step(struct socket *s)
{
    uint64_t k = next() % 8;
    note(40, k);
    if (k == 0) {
        __atomic_fetch_or(&s->shut, 1u, __ATOMIC_RELAXED);   /* another thread shut it down */
        return;
    }
    if (k == 1) {
        sock_set_error(s, -ECONNRESET);
        return;
    }
    if (s->family == COSMO_AF_UNIX)
        return;
    if (s->type == COSMO_SOCK_RAW) {
        mbufq_enqueue(&s->raw->rxq, datagram());
        return;
    }
    if (s->type == COSMO_SOCK_DGRAM) {
        mbufq_enqueue(&s->udp.rxq, datagram());
        return;
    }
    struct tcp_pcb *p = s->tcp;
    struct tstub *t = tstub(p);
    switch (k) {
    case 2:
        if (p->state == TCP_SYN_SENT || p->state == TCP_SYN_RCVD)
            p->state = next() % 4 == 0 ? TCP_CLOSED : TCP_ESTABLISHED;
        else
            t->recv_avail += (uint32_t)(1 + next() % 300);
        break;
    case 3: t->send_space += (uint32_t)(1 + next() % 2000); break;
    case 4: p->fin_rcvd = true; break;
    case 5: __atomic_store_n(&p->error, -ETIMEDOUT, __ATOMIC_RELEASE); p->state = TCP_CLOSED; break;
    case 6: t->backlog++; break;
    default: t->recv_avail += 5; break;
    }
}

static unsigned g_waits;   /* this operation's waits: a socket.c loop that waits without progress is caught */

int wait_floor_until_killable(struct waitqueue *wq, bool (*cond)(void *), void *arg)
{
    (void)wq;
    if (++g_waits > 1000) {
        EXPECT(!"a wait loop made no progress");
        return -EINTR;
    }
    for (unsigned k = 0; k < 6; k++) {
        if (cond(arg))
            return 0;
        if (next() % 16 == 0)
            return -EINTR;
        world_step(arg);
    }
    return -EINTR;
}

bool wait_floor_until_timeout(struct waitqueue *wq, bool (*cond)(void *), void *arg, uint64_t ns)
{
    (void)wq;
    note(41, ns);
    return cond(arg);   /* no other thread: what is not true now never becomes true */
}

void wait_floor_until(struct waitqueue *wq, bool (*cond)(void *), void *arg)
{
    (void)wq;
    EXPECT(cond(arg));   /* one thread: a wait that is not already over never ends */
}

/* ---- the histories ---- */

static struct socket *g_socks[NSOCK];

static unsigned free_slot(void)
{
    for (unsigned i = 0; i < NSOCK; i++)
        if (g_socks[i] == NULL)
            return i;
    return NSOCK;
}

static struct socket *some_sock(unsigned *slot)
{
    unsigned i = (unsigned)(next() % NSOCK);
    *slot = i;
    return g_socks[i];
}

static void some_addr(struct netaddr *a, int family)
{
    memset(a, 0, sizeof(*a));
    a->family = (uint16_t)(next() % 8 == 0 ? COSMO_AF_INET6 + COSMO_AF_INET - family : family);
    static const uint16_t PORTS[] = { 0, 80, 1023, 1024, 8080 };
    a->port = PORTS[next() % 5];
    a->v4 = next() % 4 == 0 ? 0 : A(10, 0, 2, (uint8_t)(1 + next() % 9));
}

static void mix_addr(const struct netaddr *a) { mix(a, sizeof(*a)); }

static void op_create(void)
{
    unsigned i = free_slot();
    if (i == NSOCK)
        return;
    static const int FAMILIES[] = { COSMO_AF_INET, COSMO_AF_INET, COSMO_AF_INET6, COSMO_AF_UNIX, 99 };
    static const int TYPES[] = { COSMO_SOCK_STREAM, COSMO_SOCK_STREAM, COSMO_SOCK_DGRAM, COSMO_SOCK_RAW, 77 };
    int family = FAMILIES[next() % 5];
    int type = TYPES[next() % 5];
    g_fail_in = next() % 16 == 0 ? (int)(next() % 2) : -1;
    struct socket *s = NULL;
    int rc;
    if (next() % 6 == 0) {
        static const int PROTOS[] = { IPPROTO_ICMP, IPPROTO_ICMP, IPPROTO_UDP };
        g_cred.privs = next() % 4 == 0 ? 0 : COSMO_PRIV_NET;
        if (next() % 4 == 0)
            g_fail_in = (int)(next() % 2);   /* the socket's allocation, or the pcb's */
        rc = ksock_create_raw(family, PROTOS[next() % 3], 1000, &s);
    } else {
        rc = ksock_create(family, type, (uint32_t)(next() % 2000), &s);
    }
    g_fail_in = -1;
    note(50, ((uint64_t)(uint32_t)rc << 32) | ((uint64_t)(uint32_t)family << 8) | (uint32_t)type);
    if (rc == 0) {
        EXPECT(s != NULL);
        mix32((uint32_t)s->state);
        g_socks[i] = s;
    }
}

static void op_bind(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    struct netaddr a;
    some_addr(&a, s->family);
    g_cred.privs = next() % 2 ? COSMO_PRIV_NET : 0;
    g_cred.euid = next() % 8 == 0 ? 1000 : 0;
    note(51, ((uint64_t)i << 32) | (uint32_t)ksock_bind(s, &a));
    mix32((uint32_t)s->state);
}

static void op_listen(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    note(52, ((uint64_t)i << 32) | (uint32_t)ksock_listen(s, (int)(next() % 6) - 1));
    mix32((uint32_t)s->state);
}

static void op_accept_on(unsigned i)
{
    struct socket *s = g_socks[i];
    if (s == NULL)
        return;
    struct socket *c = NULL;
    struct netaddr peer;
    memset(&peer, 0x5a, sizeof(peer));
    bool want_peer = next() % 2;
    g_fail_in = next() % 16 == 0 ? 0 : -1;
    int rc = ksock_accept(s, &c, want_peer ? &peer : NULL);
    g_fail_in = -1;
    note(53, ((uint64_t)i << 32) | (uint32_t)rc);
    if (want_peer)
        mix_addr(&peer);
    if (rc == 0) {
        mix32((uint32_t)c->state);
        unsigned j = free_slot();
        if (j == NSOCK)
            ksock_put(c);
        else
            g_socks[j] = c;
    }
}

static void op_accept(void)
{
    op_accept_on((unsigned)(next() % NSOCK));
}

static void op_connect(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    struct netaddr a;
    some_addr(&a, s->family);
    note(54, ((uint64_t)i << 32) | (uint32_t)ksock_connect(s, &a));
    mix32((uint32_t)s->state);
}

static uint8_t g_buf[6000];

static void op_send(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    size_t len = next() % 8 == 0 ? 5000 : (size_t)(next() % 300);
    for (size_t k = 0; k < 32 && k < len; k++)
        g_buf[k] = (uint8_t)next();
    struct netaddr a;
    some_addr(&a, s->family);
    bool to = next() % 3 == 0;
    int64_t n = ksock_sendto(s, g_buf, len, to ? &a : NULL);
    note(55, ((uint64_t)i << 32) | (uint32_t)n);
    mix32((uint32_t)s->state);
}

static void op_recv(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    size_t len = (size_t)(next() % 400);
    struct netaddr from;
    memset(&from, 0x5a, sizeof(from));
    bool want_from = next() % 2;
    int64_t n = ksock_recvfrom(s, g_buf, len, want_from ? &from : NULL);
    note(56, ((uint64_t)i << 32) | (uint32_t)n);
    if (n > 0)
        mix(g_buf, (size_t)n);
    if (want_from)
        mix_addr(&from);
}

static void op_shutdown(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    int how = (int)(next() % 5) - 1;
    note(57, ((uint64_t)i << 32) | (uint32_t)ksock_shutdown(s, how));
    mix32(s->shut);
}

static void op_names(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    struct netaddr a, b;
    memset(&a, 0x5a, sizeof(a));
    memset(&b, 0x5a, sizeof(b));
    int ra = ksock_getsockname(s, &a);
    int rb = ksock_getpeername(s, &b);
    note(58, ((uint64_t)(uint32_t)ra << 32) | (uint32_t)rb);
    if (ra == 0)
        mix_addr(&a);
    if (rb == 0)
        mix_addr(&b);
}

static void op_ready(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    note(59, ksock_ready(s));
}

static void op_errors(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    switch (next() % 4) {
    case 0: sock_set_error(s, -(int)(1 + next() % 120)); break;
    case 1: note(60, (uint32_t)ksock_error(s)); break;
    default: {
        uint64_t token;
        int e = ksock_error_peek(s, &token);
        note(61, (uint32_t)e);
        mix64(token);
        if (next() % 3 == 0)
            sock_set_error(s, e ? e : -EIO);   /* the same errno again while the first is in flight */
        ksock_error_delivered(s, token);
        mix64(s->error);
        break;
    }
    }
}

static void op_wake(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    note(62, sock_wake_ref(NULL) == NULL);
    struct socket dying;
    memset(&dying, 0, sizeof(dying));   /* its count at zero: being released, so a wake may not take it */
    note(62, sock_wake_ref(&dying) == NULL);
    if (s == NULL)
        return;
    uint64_t k = next() % 8;
    if (k == 0) {
        sock_test_wake_hold_arm(s);   /* armed, and released with no wake held */
        note(64, sock_test_wake_hold_release());
    } else if (k == 1) {
        sock_test_wake_hold_arm(s);   /* two wakes: the second finds the hold taken */
        for (unsigned n = 0; n < 2; n++) {
            struct socket *r = sock_wake_ref(s);
            sock_wake(r);
            sock_wake_unref(r);
        }
        note(64, sock_test_wake_hold_release());
    } else if (k < 4) {
        sock_test_wake_hold_arm(s);
        struct socket *r = sock_wake_ref(s);
        EXPECT(r == s);
        sock_wake(r);
        sock_wake_unref(r);
        note(63, sock_test_wake_hold_wait(1000));
        note(64, sock_test_wake_hold_release());
    } else {
        struct socket *r = sock_wake_ref(s);
        EXPECT(r == s);
        sock_wake(r);
        sock_wake_unref(r);
    }
    note(65, socket_wake_refs());
}

/* Through the socket's kobject: what a handle table's read, write, fstat, poll and fcntl do. */
static void op_object(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    EXPECT(socket_from_kobject(&s->obj) == s);
    const struct kobject_io_type *io = (const struct kobject_io_type *)s->obj.type;
    switch (next() % 6) {
    case 0: {
        size_t len = (size_t)(next() % 64);
        int64_t n = io->read(&s->obj, g_buf, len);
        note(66, (uint64_t)n);
        if (n > 0)
            mix(g_buf, (size_t)n);
        break;
    }
    case 1: note(67, (uint64_t)io->write(&s->obj, g_buf, (size_t)(next() % 64))); break;
    case 2: {
        struct cosmo_stat st;
        memset(&st, 0x5a, sizeof(st));
        note(68, (uint32_t)io->stat(&s->obj, &st));
        mix(&st, sizeof(st));
        break;
    }
    case 3: note(69, io->ready(&s->obj)); break;
    case 4: note(70, (uint32_t)io->set_nonblock(&s->obj, (int)(next() % 3) - 1)); break;
    default: note(71, io->poll_wq(&s->obj, COSMO_IO_READABLE) == &s->wait); break;
    }
}

/* The network moves while nobody waits: a connection completes, data or a datagram arrives, an error lands. */
static void op_progress(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s != NULL)
        world_step(s);
}

/* A server: bound, listening, accepting what arrives. */
static void op_server(void)
{
    unsigned i = free_slot();
    if (i == NSOCK)
        return;
    struct socket *s = NULL;
    g_policy = 0;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &s) != 0)
        return;
    g_socks[i] = s;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 8080;
    g_cred.privs = COSMO_PRIV_NET;
    g_cred.euid = 0;
    note(80, (uint32_t)ksock_bind(s, &a));
    note(81, (uint32_t)ksock_listen(s, 4));
    tstub(s->tcp)->backlog = (uint32_t)(next() % 3);
    for (unsigned n = 0; n < 3; n++)
        op_accept_on(i);
}

/* A non-blocking connect left in progress, then finished (or closed with no word why) before the next call. */
static void op_nb_connect(void)
{
    unsigned i = free_slot();
    if (i == NSOCK)
        return;
    struct socket *s = NULL;
    g_policy = 0;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &s) != 0)
        return;
    g_socks[i] = s;
    ksock_set_nonblock(s, true);
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 80;
    a.v4 = A(10, 0, 2, 2);
    note(82, (uint32_t)ksock_connect(s, &a));
    if (s->state != SS_CONNECTING)
        return;
    bool done = next() % 2;
    s->tcp->state = done ? TCP_ESTABLISHED : TCP_CLOSED;
    if (done && next() % 2)
        note(83, (uint64_t)ksock_sendto(s, g_buf, 10, NULL));
    else
        note(84, (uint32_t)ksock_connect(s, &a));
    mix32((uint32_t)s->state);
}

static void op_nonblock(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s != NULL)
        ksock_set_nonblock(s, next() % 2);
    g_thread.io_nonblock = next() % 8 == 0;
}

static void op_close(void)
{
    unsigned i;
    struct socket *s = some_sock(&i);
    if (s == NULL)
        return;
    if (next() % 4 == 0) {
        ksock_get(s);   /* a second holder: the first put does not release */
        ksock_put(s);
    }
    ksock_put(s);
    g_socks[i] = NULL;
    note(72, socket_count());
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    mbuf_init();
    socket_init();
    g_cred.privs = COSMO_PRIV_NET;
    struct kobject other = { 0 };
    static const struct kobject_type other_type = { .name = "other" };
    other.type = &other_type;
    EXPECT(socket_from_kobject(&other) == NULL);
    for (unsigned r = 0; r < ROUNDS; r++) {
        g_policy = next();
        g_waits = 0;
        uint64_t k = next() % 32;
        if (k < 4)
            op_create();
        else if (k < 6)
            op_bind();
        else if (k < 7)
            op_listen();
        else if (k < 9)
            op_accept();
        else if (k < 11)
            op_connect();
        else if (k < 15)
            op_send();
        else if (k < 19)
            op_recv();
        else if (k < 20)
            op_shutdown();
        else if (k < 21)
            op_names();
        else if (k < 22)
            op_ready();
        else if (k < 24)
            op_errors();
        else if (k < 25)
            op_wake();
        else if (k < 28)
            op_object();
        else if (k < 29)
            op_nonblock();
        else if (k < 30)
            op_progress();
        else if (k < 31 || r % 4 == 1)
            op_close();
        else if (r % 4 == 2)
            op_nb_connect();
        else
            op_server();
        EXPECT(g_nheld == 0);
        EXPECT(harness_locks_held() == 0);
    }
    for (unsigned i = 0; i < NSOCK; i++)
        if (g_socks[i] != NULL) {
            ksock_put(g_socks[i]);
            g_socks[i] = NULL;
        }
    note(73, socket_count());
    printf("  socket digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == SOCKET_DIGEST);
    EXPECT(socket_count() == 0);
    EXPECT(g_live == 0);
}

static const struct host_test tests[] = {
    { "socket digest", test_digest },
};

int main(void) { return harness_run(tests, sizeof(tests) / sizeof(tests[0])); }
