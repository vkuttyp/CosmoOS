/*
 * test_tapsvc.c - The tap's autoconfiguration service (kernel-services/
 * network/tapsvc.c) on the host: roadmap 2.0 SA6. A digest over seeded
 * histories, recorded from the C, of nine taps, each with a guest (two, for
 * the one lease) on its own subnet 10.0.(3+k).0/24:
 *
 *   - the service started and stopped on any tap, the ninth refused, with
 *     its sockets' creation or binding failing, a proxy thread not starting
 *     (the rollback), and the firmware's resolver string well-formed,
 *     malformed, too long or absent;
 *   - the guests' frames into the DHCP filter: DISCOVER, REQUEST for the
 *     guest's address or another (option 50 or ciaddr), DECLINE, RELEASE,
 *     INFORM and types no server takes, the broadcast flag set or not, from
 *     the guest holding the lease or a second one; frames that are not
 *     DHCP-to-server (left to the stack), malformed or truncated;
 *   - the DNS proxy's two threads, each run over the datagrams its socket
 *     holds: queries of every size (shorter than a header, up to a full
 *     link's datagram), with the upstream configured or not, enough to fill
 *     the pending table; answers from the upstream, from a stranger, from
 *     the right address on the wrong port, with the id the proxy lent or
 *     another, of every size; the upstream changed and cleared;
 *   - time passing, pending queries expiring, the age sweep, the counters.
 *
 * The rest of the stack (Ethernet, ARP, mbufs, the checksum) runs as the
 * kernel's, over tests/fuzz/shim_net.c; the taps, sockets, threads and the
 * firmware configuration are this file's. A thread runs when the test runs
 * it, its loop reading what the test queued until the queue is empty (its
 * read returns 0, as a shut-down socket's does) and its thread_exit jumping
 * back here. Every frame the service transmits, every datagram it sends
 * (socket, destination, bytes), every line it logs and every return is
 * folded into the digest; every frame has correct checksums; after each
 * history nothing is allocated beyond the baseline, every socket is
 * released and no mbuf is alive.
 *
 * Not reached: kmalloc and m_getcl failing, and the 4,096 tries for a
 * free DNS id.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tap.h>
#include <kernel/net/tapsvc.h>
#include <kernel/netif.h>
#include <kernel/socket.h>
#include <kernel/thread.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

/* The digest of the C (tapsvc.c, 2026-11-03), which the CeeSafe port gives. */
#define TAPSVC_DIGEST 0x2d2700174801ec6bull
#define HISTORIES 4u
#define OPS 2000u
#define SEED 0x7A95C11E2026B00Eull
#define NTAP 9u            /* one more than the service's eight instances */
#define QMAX 160u          /* datagrams queued on one socket */
#define DGRAM_MAX 1600u
#define DNS_HDR_TEST 12u   /* tapsvc.c's DNS_HDR */

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

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

static bool g_benching;

static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix32(0x7400u + which);
    mix64(value);
}

/* shim_net.c's assertions land in the harness. */
void fuzz_fail(const char *file, int line, const char *expr) { harness_fail(file, line, expr); }

static void on_klog(int level, const char *line)
{
    note(1, (uint64_t)level);
    if (!g_benching)
        mix(line, strlen(line));
}

/* ---- the taps ---- */

struct tap {
    struct fz_netif f;
    tap_input_fn fn;
    void *arg;
};
static struct tap g_taps[NTAP];
static unsigned g_bad_frames;

static uint32_t tap_ip(unsigned k) { return IPV4_ADDR(10, 0, 3 + k, 1); }
static uint32_t guest_ip(unsigned k) { return IPV4_ADDR(10, 0, 3 + k, 15); }
#define MASK24 htonl(0xffffff00u)

static void guest_mac(unsigned k, unsigned which, uint8_t mac[6])
{
    static const uint8_t base[6] = { 0x02, 0xa1, 0x00, 0x00, 0x00, 0x00 };
    memcpy(mac, base, 6);
    mac[4] = (uint8_t)k;
    mac[5] = (uint8_t)(0x0f + which);
}

static unsigned tap_index(const struct fz_netif *f) { return (unsigned)((const struct tap *)f - g_taps); }

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    if (!g_benching && np_check_checksums(frame, len) != 0)
        g_bad_frames++;
    note(2, ((uint64_t)tap_index(f) << 32) | len);
    if (!g_benching)
        mix(frame, len);
}

struct netif *tap_netif(struct tap *t) { return &t->f.nif; }

void tap_set_input_filter(struct tap *t, tap_input_fn fn, void *arg)
{
    t->fn = fn;
    t->arg = arg;
    note(3, ((uint64_t)(unsigned)(t - g_taps) << 1) | (fn != NULL));
}

/* ---- the firmware's resolver ---- */

static const char *g_resolver;   /* NULL: absent */

bool fwcfg_get_string(const char *key, char *buf, size_t len)
{
    note(4, strcmp(key, "resolver") == 0);
    if (g_resolver == NULL || len == 0)
        return false;
    size_t n = strlen(g_resolver);
    if (n > len - 1)
        n = len - 1;   /* as fwcfg.c: what fits, terminated */
    memcpy(buf, g_resolver, n);
    buf[n] = '\0';
    return true;
}

/* ---- the sockets ---- */

struct dgram {
    struct netaddr from;
    uint32_t n;
    uint8_t b[DGRAM_MAX];
};

struct tsock {
    struct socket s;
    bool live;
    unsigned id;
    struct netaddr bound;
    struct dgram *q;               /* QMAX queued datagrams */
    unsigned qhead, qlen;
    uint16_t lent;                 /* the id of the last datagram sent through it */
};
#define NSOCK 32u
static struct tsock g_socks[NSOCK];
static unsigned g_next_sock_id, g_released;
static bool g_fail_create, g_fail_bind;
static unsigned g_fail_create_after;   /* 1: the first create succeeds, the second fails */

static struct tsock *tsock_of(struct socket *s) { return (struct tsock *)s; }

static void sock_release(struct kobject *obj)
{
    struct tsock *t = (struct tsock *)obj;
    EXPECT(t->live);
    t->live = false;
    t->qlen = 0;
    g_released++;
    note(5, t->id);
}

static const struct kobject_type g_sock_type = { .name = "test-socket", .release = sock_release };

int ksock_create(int family, int type, uint32_t uid, struct socket **out)
{
    note(6, ((uint64_t)(unsigned)family << 32) | ((uint64_t)(unsigned)type << 8) | uid);
    if (g_fail_create) {
        if (g_fail_create_after == 0) {
            g_fail_create = false;
            return -ENOMEM;
        }
        g_fail_create_after--;
    }
    for (unsigned i = 0; i < NSOCK; i++) {
        struct tsock *t = &g_socks[i];
        if (t->live)
            continue;
        struct dgram *q = t->q;
        memset(t, 0, sizeof(*t));
        t->q = q;
        kobject_init(&t->s.obj, &g_sock_type);
        t->live = true;
        t->id = g_next_sock_id++;
        *out = &t->s;
        return 0;
    }
    return -ENOMEM;
}

static void note_addr(uint32_t which, const struct netaddr *a)
{
    note(which, ((uint64_t)a->family << 48) | ((uint64_t)a->port << 32) | a->v4);
}

int ksock_bind(struct socket *s, const struct netaddr *addr)
{
    note(7, tsock_of(s)->id);
    note_addr(8, addr);
    if (g_fail_bind) {
        g_fail_bind = false;
        return -EADDRINUSE;
    }
    tsock_of(s)->bound = *addr;
    return 0;
}

int64_t ksock_sendto(struct socket *s, const void *buf, size_t len, const struct netaddr *to)
{
    struct tsock *t = tsock_of(s);
    EXPECT(t->live);
    note(9, ((uint64_t)t->id << 32) | len);
    note_addr(10, to);
    if (!g_benching)
        mix(buf, len);
    if (len >= 2)
        t->lent = (uint16_t)(((const uint8_t *)buf)[0] << 8 | ((const uint8_t *)buf)[1]);
    return (int64_t)len;
}

/* The next datagram queued, or 0 (the end: a shut-down socket's read). */
int64_t ksock_recvfrom(struct socket *s, void *buf, size_t len, struct netaddr *from)
{
    struct tsock *t = tsock_of(s);
    EXPECT(t->live);
    if (t->qlen == 0) {
        memset(from, 0, sizeof(*from));
        return 0;
    }
    struct dgram *d = &t->q[t->qhead];
    t->qhead = (t->qhead + 1) % QMAX;
    t->qlen--;
    *from = d->from;
    size_t n = d->n < len ? d->n : len;   /* as socket.cee's deliverDatagram: cut to the buffer, silently */
    memcpy(buf, d->b, n);
    note(11, ((uint64_t)t->id << 32) | ((uint64_t)d->n << 16) | n);
    return (int64_t)n;
}

int ksock_shutdown(struct socket *s, int how)
{
    note(12, ((uint64_t)tsock_of(s)->id << 8) | (unsigned)how);
    return 0;
}

static void enqueue(struct socket *s, const struct netaddr *from, const uint8_t *b, uint32_t n)
{
    struct tsock *t = tsock_of(s);
    if (t->qlen == QMAX)
        return;
    struct dgram *d = &t->q[(t->qhead + t->qlen) % QMAX];
    d->from = *from;
    d->n = n;
    memcpy(d->b, b, n);
    t->qlen++;
}

/* ---- the threads ---- */

struct tthread {
    void (*entry)(void *);
    void *arg;
    char name[16];
    bool live;
};
#define NTHREAD 32u
static struct tthread g_threads[NTHREAD];

static struct tthread *tthread_of(struct thread *t) { return (struct tthread *)(void *)t; }
static bool g_fail_thread;
static unsigned g_fail_thread_after;   /* 1: the first create succeeds, the second fails */
static jmp_buf g_thread_jmp;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    struct tthread *t;
    note(13, (uint64_t)(unsigned)priority);
    mix(name, strlen(name));
    if (g_fail_thread) {
        if (g_fail_thread_after == 0) {
            g_fail_thread = false;
            return NULL;
        }
        g_fail_thread_after--;
    }
    for (unsigned i = 0; i < NTHREAD; i++) {
        t = &g_threads[i];
        if (t->live)
            continue;
        t->entry = entry;
        t->arg = arg;
        snprintf(t->name, sizeof(t->name), "%s", name);
        t->live = true;
        return (struct thread *)(void *)t;
    }
    return NULL;
}

int thread_join(struct thread *th)
{
    struct tthread *t = tthread_of(th);
    EXPECT(t->live);
    t->live = false;
    note(14, (uint64_t)(t - g_threads));
    return 0;
}

void thread_exit(int code)
{
    note(15, (uint64_t)(unsigned)code);
    longjmp(g_thread_jmp, 1);
}

/* Run a thread's loop until its socket's queue is empty and it exits. */
static void run_thread(struct tthread *t)
{
    if (t == NULL || !t->live)
        return;
    if (setjmp(g_thread_jmp) == 0) {
        t->entry(t->arg);
        EXPECT(!"a proxy thread returned without thread_exit");
    }
}

/* ---- the instances ---- */

struct inst {
    struct tapsvc *svc;
    struct socket *gsock, *usock;  /* as the service created them: guest side first */
    struct tthread *gth, *uth;
    uint32_t up_ip;
    uint16_t up_port;
};
static struct inst g_inst[NTAP];

static unsigned live_instances(void)
{
    unsigned n = 0;
    for (unsigned k = 0; k < NTAP; k++)
        n += g_inst[k].svc != NULL;
    return n;
}

static const char *const k_resolvers[] = {
    "10.0.2.3", "8.8.8.8", "256.1.1.1", "1.2.3", "10.0.2.3 ", "", "1.2.3.4.5", "010.0.2.3",
    "100.100.100.100.100.100.100.100.100",   /* longer than the service's 32 bytes */
    "192.168.1.254", "a.b.c.d",
};

static void op_start(void)
{
    unsigned k = pick(NTAP);
    struct inst *in = &g_inst[k];
    if (in->svc != NULL)
        return;
    unsigned r = pick(12);
    g_resolver = r < sizeof(k_resolvers) / sizeof(k_resolvers[0]) ? k_resolvers[r] : NULL;
    unsigned f = pick(16);
    g_fail_create = f == 0 || f == 4;
    g_fail_create_after = f == 4;
    g_fail_bind = f == 1;
    g_fail_thread = f == 2 || f == 3;
    g_fail_thread_after = f == 3;
    unsigned first = g_next_sock_id;
    struct tthread *before[NTHREAD];
    for (unsigned i = 0; i < NTHREAD; i++)
        before[i] = g_threads[i].live ? &g_threads[i] : NULL;
    struct tapsvc *svc = tapsvc_start(&g_taps[k]);
    note(16, ((uint64_t)k << 8) | (svc != NULL));
    g_fail_create = g_fail_bind = g_fail_thread = false;
    in->svc = svc;
    in->gsock = in->usock = NULL;
    in->gth = in->uth = NULL;
    if (svc == NULL) {
        EXPECT(live_instances() == 8);
        return;
    }
    /* The service's sockets and threads: those created by this start and still held. */
    for (unsigned i = 0; i < NSOCK; i++) {
        struct tsock *t = &g_socks[i];
        if (!t->live || t->id < first)
            continue;
        if (t->id == first)
            in->gsock = &t->s;
        else
            in->usock = &t->s;
    }
    for (unsigned i = 0; i < NTHREAD; i++) {
        if (!g_threads[i].live || before[i] != NULL)
            continue;
        if (strcmp(g_threads[i].name, "dns-guest") == 0)
            in->gth = &g_threads[i];
        else
            in->uth = &g_threads[i];
    }
    in->up_ip = 0;
    in->up_port = 0;
    if (g_resolver != NULL && in->gth != NULL) {
        uint8_t a[4];
        if (netparse_ipv4_text(g_resolver, (uint32_t)(strlen(g_resolver) < 31 ? strlen(g_resolver) : 31), a)) {
            memcpy(&in->up_ip, a, 4);
            in->up_port = 53;
        }
    }
}

static void op_stop(void)
{
    unsigned k = pick(NTAP + 1);
    if (k == NTAP) {
        tapsvc_stop(NULL);
        note(17, 0);
        return;
    }
    struct inst *in = &g_inst[k];
    if (in->svc == NULL)
        return;
    tapsvc_stop(in->svc);
    EXPECT(g_taps[k].fn == NULL);
    note(17, k + 1);
    memset(in, 0, sizeof(*in));
}

/* A guest's DHCP message of type `mt` (0: no option 53) in a frame, broadcast or to the tap. */
static size_t dhcp_frame(uint8_t *f, unsigned k, unsigned who, uint8_t mt)
{
    uint8_t mac[6], dh[400];
    guest_mac(k, who, mac);
    memset(dh, 0, sizeof(dh));
    dh[BOOTP_OFF_OP] = pick(40) == 0 ? BOOTP_REPLY : BOOTP_REQUEST;
    dh[BOOTP_OFF_HTYPE] = DHCP_HTYPE_ETHER;
    dh[BOOTP_OFF_HLEN] = 6;
    np_put32(dh + BOOTP_OFF_XID, (uint32_t)next());
    if (pick(2) == 0)
        np_put16(dh + BOOTP_OFF_FLAGS, DHCP_FLAG_BROADCAST);
    uint32_t want = pick(5) == 0 ? IPV4_ADDR(10, 0, 3 + k, 16 + pick(3)) : guest_ip(k);
    bool ci = mt == DHCP_REQUEST && pick(3) == 0;
    if (ci)
        memcpy(dh + BOOTP_OFF_CIADDR, &want, 4);
    memcpy(dh + BOOTP_OFF_CHADDR, mac, 6);
    np_put32(dh + BOOTP_LEN, pick(40) == 0 ? 0x63825364u : DHCP_MAGIC);
    uint8_t *o = dh + DHCP_FIXED;
    if (mt != 0) {
        *o++ = OPT_MSGTYPE;
        *o++ = 1;
        *o++ = mt;
    }
    if (mt == DHCP_REQUEST && !ci) {
        *o++ = OPT_REQIP;
        *o++ = 4;
        memcpy(o, &want, 4);
        o += 4;
    }
    *o++ = OPT_PARAMS;
    *o++ = 3;
    *o++ = OPT_SUBNET;
    *o++ = OPT_ROUTER;
    *o++ = OPT_DNS;
    *o++ = OPT_END;
    size_t dhlen = (size_t)(o - dh);
    unsigned lk = pick(32);
    if (lk == 0)
        dhlen = pick(DHCP_FIXED);
    else if (lk < 8)
        dhlen = 300;
    uint16_t sport = DHCP_CPORT, dport = DHCP_SPORT;
    unsigned pk = pick(32);
    if (pk == 0)
        dport = 68;
    else if (pk == 1)
        sport = 1234;
    uint32_t dst = pick(4) == 0 ? tap_ip(k) : INADDR_BROADCAST_N;
    size_t n = np_frame_udp4(f, pick(3) == 0 ? g_taps[k].f.nif.mac : eth_broadcast, mac, 0, dst, sport, dport, dh,
                             dhlen);
    unsigned ck = pick(32);
    if (ck == 0)
        f[NP_ETH + NP_IPV4 + 7] ^= 0x33;   /* the UDP checksum */
    else if (ck == 1)
        f[NP_ETH + 10] ^= 0x11;            /* the IP header's */
    else if (ck == 2)
        n = NP_ETH + NP_IPV4 + pick(NP_UDP + 20);   /* truncated */
    return n;
}

static void op_dhcp(void)
{
    unsigned k = pick(NTAP);
    struct tap *t = &g_taps[k];
    static const uint8_t types[] = { DHCP_DISCOVER, DHCP_REQUEST, DHCP_DISCOVER, DHCP_REQUEST, DHCP_REQUEST,
                                     DHCP_DECLINE, DHCP_RELEASE, 8 /* INFORM */, DHCP_OFFER, 0, 99 };
    uint8_t mt = types[pick(sizeof(types))];
    unsigned who = pick(5) == 0;
    static uint8_t f[600];
    size_t n = dhcp_frame(f, k, who, mt);
    note(18, ((uint64_t)k << 40) | ((uint64_t)mt << 32) | n);
    if (t->fn == NULL)
        return;
    bool claimed = t->fn(t, f, (uint32_t)n, t->arg);
    note(19, claimed);
    fz_run_work();
}

/* A query or an answer of `n` bytes: a header with `id` and a question, then filler. */
static uint32_t dns_message(uint8_t *b, uint32_t n, uint16_t id, bool answer)
{
    static const uint8_t q[] = { 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01 };
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)(i * 7 + 3);
    if (n >= 2) {
        b[0] = (uint8_t)(id >> 8);
        b[1] = (uint8_t)id;
    }
    if (n >= 4) {
        b[2] = answer ? 0x81 : 0x01;
        b[3] = answer ? 0x80 : 0x00;
    }
    if (n >= DNS_HDR_TEST + sizeof(q))
        memcpy(b + 12, q, sizeof(q));
    return n;
}
static uint32_t dgram_size(void)
{
    static const uint32_t sizes[] = { 0, 5, 11, 12, 29, 64, 300, 511, 512, 513, 700, 1232, 1472, DGRAM_MAX };
    return sizes[pick(sizeof(sizes) / sizeof(sizes[0]))];
}

static void op_query(void)
{
    unsigned k = pick(NTAP);
    struct inst *in = &g_inst[k];
    if (in->svc == NULL || in->gth == NULL)
        return;
    unsigned count = pick(10) == 0 ? 60 + pick(100) : 1 + pick(3);   /* sometimes enough to fill the table */
    struct netaddr from;
    memset(&from, 0, sizeof(from));
    from.family = COSMO_AF_INET;
    from.v4 = guest_ip(k);
    static uint8_t b[DGRAM_MAX];
    for (unsigned i = 0; i < count; i++) {
        from.port = (uint16_t)(30000 + pick(4));
        uint32_t size = count > 3 ? 40 : dgram_size();
        uint16_t id = (uint16_t)next();   /* after the size: one random draw per statement */
        uint32_t n = dns_message(b, size, id, false);
        enqueue(in->gsock, &from, b, n);
    }
    note(20, ((uint64_t)k << 32) | count);
    run_thread(in->gth);
}

static void op_answer(void)
{
    unsigned k = pick(NTAP);
    struct inst *in = &g_inst[k];
    if (in->svc == NULL || in->uth == NULL)
        return;
    unsigned count = 1 + pick(3);
    static uint8_t b[DGRAM_MAX];
    for (unsigned i = 0; i < count; i++) {
        struct netaddr from;
        memset(&from, 0, sizeof(from));
        from.family = COSMO_AF_INET;
        unsigned w = pick(10);
        from.v4 = w < 7 ? in->up_ip : w < 9 ? IPV4_ADDR(10, 0, 2, 9) : in->up_ip;
        from.port = w == 9 ? 5353 : in->up_port != 0 ? in->up_port : 53;
        uint16_t id = pick(4) != 0 ? tsock_of(in->usock)->lent : (uint16_t)next();
        uint32_t n = dns_message(b, dgram_size(), id, true);
        enqueue(in->usock, &from, b, n);
    }
    note(21, ((uint64_t)k << 32) | count);
    run_thread(in->uth);
}

static void op_upstream(void)
{
    unsigned k = pick(NTAP);
    struct inst *in = &g_inst[k];
    if (in->svc == NULL)
        return;
    unsigned w = pick(4);
    uint32_t ip = w == 0 ? 0 : w == 1 ? IPV4_ADDR(10, 0, 2, 3) : w == 2 ? IPV4_ADDR(1, 1, 1, 1) : IPV4_ADDR(10, 0, 2, 4);
    uint16_t port = pick(4) == 0 ? 5353 : 53;
    tapsvc_test_set_upstream(in->svc, ip, port);
    in->up_ip = ip;
    in->up_port = ip != 0 ? port : 0;
    note(22, ((uint64_t)k << 48) | ((uint64_t)port << 32) | ip);
}

static void op_time(void)
{
    uint64_t ms = 1 + pick(7000);
    fz_clock_advance(ms * 1000000ull);
    if (pick(2) == 0)
        tapsvc_dns_age(fz_now());
    note(23, fz_now());
}

static void op_stats(void)
{
    struct tapsvc_stats st;
    memset(&st, 0, sizeof(st));
    tapsvc_get_stats(&st);
    note(24, st.dhcp_discover);
    note(24, st.dhcp_offer);
    note(24, st.dhcp_request);
    note(24, st.dhcp_ack);
    note(24, st.dhcp_nak);
    note(24, st.dhcp_release);
    note(24, st.dhcp_ignored);
    note(24, st.dns_query);
    note(24, st.dns_answer);
    note(24, st.dns_servfail);
    note(24, st.dns_drop_full);
    note(24, st.dns_expired);
    note(24, st.dns_pending);
    unsigned n = tapsvc_count();
    EXPECT(n == live_instances());
    note(25, n);
}

/* ---- the histories ---- */

static void setup(unsigned h)
{
    fz_random_seed(SEED ^ h);
    for (unsigned k = 0; k < NTAP; k++) {
        char name[8];
        snprintf(name, sizeof(name), "tap%u", k);
        uint8_t mac[6] = { 0x02, 0x7a, 0x00, 0x00, 0x00, (uint8_t)k };
        fz_netif_register(&g_taps[k].f, name, mac, tap_ip(k), MASK24, 0, NETIF_NODEFAULT);
        g_taps[k].f.on_frame = on_frame;
        g_taps[k].fn = NULL;
        g_taps[k].arg = NULL;
        memset(&g_inst[k], 0, sizeof(g_inst[k]));
    }
}

static void teardown(size_t baseline)
{
    for (unsigned k = 0; k < NTAP; k++)
        if (g_inst[k].svc != NULL) {
            tapsvc_stop(g_inst[k].svc);
            memset(&g_inst[k], 0, sizeof(g_inst[k]));
        }
    EXPECT(tapsvc_count() == 0);
    for (unsigned i = 0; i < NSOCK; i++)
        EXPECT(!g_socks[i].live);
    for (unsigned i = 0; i < NTHREAD; i++)
        EXPECT(!g_threads[i].live);
    fz_netif_unregister_all();
    fz_run_work();
    EXPECT(harness_locks_held() == 0);
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    EXPECT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    EXPECT(fz_allocs_live() == baseline);
    if (fz_allocs_live() != baseline)
        fz_dump_live();
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    fz_net_init();
    for (unsigned i = 0; i < NSOCK; i++)
        g_socks[i].q = calloc(QMAX, sizeof(struct dgram));
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the lines are in the digest */
    size_t baseline = fz_allocs_live();
    for (unsigned h = 0; h < HISTORIES; h++) {
        setup(h);
        for (unsigned r = 0; r < OPS; r++) {
            unsigned k = pick(32);
            if (k < 3)
                op_start();
            else if (k < 4)
                op_stop();
            else if (k < 12)
                op_dhcp();
            else if (k < 19)
                op_query();
            else if (k < 25)
                op_answer();
            else if (k < 26)
                op_upstream();
            else if (k < 30)
                op_time();
            else
                op_stats();
            EXPECT(harness_locks_held() == 0);
        }
        op_stats();
        teardown(baseline);
        note(60, h);
    }
    harness_klog_hook = NULL;
    EXPECT(g_bad_frames == 0);
    printf("  tapsvc digest 0x%016llx over %u histories of %u operations (%u sockets released)\n",
           (unsigned long long)g_digest, HISTORIES, OPS, g_released);
    EXPECT(g_digest == TAPSVC_DIGEST);
}

/* TAPSVC_BENCH=1: the median of nine runs of 20,000 rounds on one instance -- a guest's DISCOVER and REQUEST
 * answered (OFFER, ACK), a query relayed upstream and its answer relayed back. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static size_t bench_frame(uint8_t *f, uint8_t mt)
{
    uint8_t mac[6], dh[300];
    guest_mac(0, 0, mac);
    memset(dh, 0, sizeof(dh));
    dh[0] = BOOTP_REQUEST;
    dh[1] = DHCP_HTYPE_ETHER;
    dh[2] = 6;
    np_put32(dh + BOOTP_OFF_XID, 0x1234);
    memcpy(dh + BOOTP_OFF_CHADDR, mac, 6);
    np_put32(dh + BOOTP_LEN, DHCP_MAGIC);
    uint8_t *o = dh + DHCP_FIXED;
    *o++ = OPT_MSGTYPE;
    *o++ = 1;
    *o++ = mt;
    if (mt == DHCP_REQUEST) {
        uint32_t want = guest_ip(0);
        *o++ = OPT_REQIP;
        *o++ = 4;
        memcpy(o, &want, 4);
        o += 4;
    }
    *o++ = OPT_END;
    return np_frame_udp4(f, eth_broadcast, mac, 0, INADDR_BROADCAST_N, DHCP_CPORT, DHCP_SPORT, dh, 300);
}

static void tapsvc_bench(void)
{
    g_rng = SEED;
    fz_net_init();
    size_t baseline = fz_allocs_live();
    setup(0);
    g_resolver = "10.0.2.3";
    struct tap *t = &g_taps[0];
    struct inst *in = &g_inst[0];
    in->svc = tapsvc_start(t);
    EXPECT(in->svc != NULL);
    for (unsigned i = 0; i < NSOCK; i++) {
        if (!g_socks[i].live)
            continue;
        if (in->gsock == NULL)
            in->gsock = &g_socks[i].s;
        else
            in->usock = &g_socks[i].s;
    }
    for (unsigned i = 0; i < NTHREAD; i++) {
        if (!g_threads[i].live)
            continue;
        if (strcmp(g_threads[i].name, "dns-guest") == 0)
            in->gth = &g_threads[i];
        else
            in->uth = &g_threads[i];
    }
    g_benching = true;
    static uint8_t disc[600], req[600], b[DGRAM_MAX];
    size_t nd = bench_frame(disc, DHCP_DISCOVER), nr = bench_frame(req, DHCP_REQUEST);
    struct netaddr guest, up;
    memset(&guest, 0, sizeof(guest));
    guest.family = COSMO_AF_INET;
    guest.v4 = guest_ip(0);
    guest.port = 30000;
    memset(&up, 0, sizeof(up));
    up.family = COSMO_AF_INET;
    up.v4 = IPV4_ADDR(10, 0, 2, 3);
    up.port = 53;
    double times[9];
    for (unsigned r = 0; r < 9; r++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            EXPECT(t->fn(t, disc, (uint32_t)nd, t->arg));
            EXPECT(t->fn(t, req, (uint32_t)nr, t->arg));
            enqueue(in->gsock, &guest, b, dns_message(b, 64, (uint16_t)i, false));
            run_thread(in->gth);
            enqueue(in->usock, &up, b, dns_message(b, 120, tsock_of(in->usock)->lent, true));
            run_thread(in->uth);
        }
        times[r] = ns_since(&t0) / 20000;
    }
    qsort(times, 9, sizeof(double), cmp_double);
    teardown(baseline);
    g_benching = false;
    printf("  bench a round (DISCOVER, REQUEST, a query and its answer) %.1f ns\n", times[4]);
}

static const struct host_test tests[] = {
    { "tapsvc digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("TAPSVC_BENCH") != NULL)
        tapsvc_bench();
    return rc;
}
