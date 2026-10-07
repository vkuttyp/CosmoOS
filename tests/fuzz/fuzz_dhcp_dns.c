/*
 * fuzz_dhcp_dns.c - The tap services' two parsers: DHCP and the DNS proxy.
 *
 * tapsvc.c (TAPSVC_HOST_TEST) over a bare interface on the host
 * (shim_net.c): no tap, no sockets, no threads. The instance serves the
 * guest 10.75.0.15 behind the gateway 10.75.0.1; its DHCP filter is fed
 * frames as the tap's far end would inject them, and each side of the DNS
 * proxy is fed one datagram at a time, as its thread would read it from
 * its socket. Replies are captured: a DHCP reply as the frame the stack
 * transmits out the interface, a DNS relay as the datagram the proxy sends
 * through its (stubbed) socket.
 *
 * The input is a sequence of records: a kind byte, a little-endian 16-bit
 * length, the payload.
 *   0  an Ethernet frame into the DHCP filter
 *   1  a DNS query from the guest (payload: the DNS message)
 *   2  a DNS answer from the upstream resolver; bit 4 of the kind byte
 *      makes it arrive from a stranger instead, bit 5 stamps the id the
 *      proxy most recently lent upstream (so an answer finds its query)
 *   3  time passes: ms in the payload's first two bytes (pending queries
 *      expire; the age sweep runs)
 *   4  the resolver address string from the firmware configuration
 *   5  the upstream resolver is configured (payload byte 0 odd) or not
 *
 * Oracles: every DHCP reply the filter builds is a well-formed BOOTP reply
 * (op 2, the request's xid, the magic cookie, a message type that is
 * OFFER, ACK or NAK) carried in a correct IPv4/UDP frame from port 67 to
 * 68; a reply goes only to a request the filter claimed; a query with the
 * upstream configured is relayed once, to the upstream, byte for byte but
 * for its id, and never relayed without the upstream; an answer is relayed
 * only when it arrives from the configured upstream and carries an id the
 * proxy lent and has not yet consumed, and then with the guest's id
 * restored, to the guest that asked; live pending queries never exceed
 * DNS_PENDING_MAX and go to zero as time passes; every lock released; after
 * teardown no mbuf alive and the allocator at its baseline.
 */

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/udp.h>
#include <kernel/socket.h>
#include <kernel/thread.h>
#include <kernel/fwcfg.h>
#include <kernel/net/tap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

#define K_DHCP     0u
#define K_QUERY    1u
#define K_ANSWER   2u
#define K_TIME     3u
#define K_PARSE    4u
#define K_UPSTREAM 5u
#define K_STRANGER (1u << 4)
#define K_LENT_ID  (1u << 5)

static const uint8_t k_mac1[6] = { 0x02, 0xf2, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t k_guest_mac[6] = { 0x02, 0xa1, 0x00, 0x00, 0x00, 0x0f };
#define IP1      IPV4_ADDR(10, 75, 0, 1)
#define GUEST    IPV4_ADDR(10, 75, 0, 15)
#define MASK24   htonl(0xffffff00u)
#define UPSTREAM IPV4_ADDR(10, 0, 2, 3)
#define STRANGER IPV4_ADDR(10, 0, 2, 9)
#define DNS_PENDING_MAX_EXPECTED 128u

static struct fz_netif g_f1;
static size_t g_baseline;
static struct tapsvc *g_svc;
static unsigned g_bad_frames;

/* --- the stubs tapsvc.c links against ------------------------------------- */

static const struct kobject_type g_sock_type = { .name = "fz-socket" };
static struct socket g_gsock, g_usock;

/* What the proxy sent, through which socket, to whom. */
struct sent {
    struct socket *via;
    struct netaddr to;
    uint8_t buf[512];
    size_t n;
};
static struct sent g_sent[8];
static unsigned g_nsent;

int64_t ksock_sendto(struct socket *s, const void *buf, size_t len, const struct netaddr *to)
{
    struct sent *e = &g_sent[g_nsent % 8];   /* a ring: the oracles read the newest entries */
    e->via = s;
    e->to = *to;
    e->n = len < sizeof(e->buf) ? len : sizeof(e->buf);
    memcpy(e->buf, buf, e->n);
    g_nsent++;
    return (int64_t)len;
}

int ksock_create(int family, int type, uint32_t uid, struct socket **out)
{
    (void)family; (void)type; (void)uid; (void)out;
    return -ENOSYS;   /* dns_start is never called here */
}
int ksock_bind(struct socket *s, const struct netaddr *addr) { (void)s; (void)addr; return -ENOSYS; }
int64_t ksock_recvfrom(struct socket *s, void *buf, size_t len, struct netaddr *from)
{ (void)s; (void)buf; (void)len; (void)from; return -ENOSYS; }
int ksock_shutdown(struct socket *s, int how) { (void)s; (void)how; return 0; }
struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{ (void)entry; (void)arg; (void)name; (void)priority; return NULL; }
void thread_exit(int code) { (void)code; abort(); }
int thread_join(struct thread *t) { (void)t; return 0; }
bool fwcfg_get_string(const char *key, char *buf, size_t len) { (void)key; (void)buf; (void)len; return false; }
struct netif *tap_netif(struct tap *t) { (void)t; return NULL; }
void tap_set_input_filter(struct tap *t, tap_input_fn fn, void *arg) { (void)t; (void)fn; (void)arg; }

/* --- the DHCP reply oracle ------------------------------------------------- */

static uint32_t g_claimed_xid;
static bool g_claimed;       /* the last frame was claimed by the filter */

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    (void)f;
    if (np_check_checksums(frame, len) != 0) {
        g_bad_frames++;
        return;
    }
    /* A frame out of the interface during a DHCP record is the filter's reply. */
    if (!g_claimed) {
        g_bad_frames++;   /* a reply to a frame the filter did not claim */
        return;
    }
    if (len < NP_ETH + NP_IPV4 + NP_UDP + 240 || np_get16(frame + 12) != ETH_P_IP) {
        g_bad_frames++;
        return;
    }
    const uint8_t *ip = frame + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    const uint8_t *uh = ip + ihl;
    if (ip[9] != IPPROTO_UDP || np_get16(uh) != 67 || np_get16(uh + 2) != 68) {
        g_bad_frames++;
        return;
    }
    const uint8_t *dh = uh + NP_UDP;
    uint32_t dhlen = np_get16(uh + 4) - NP_UDP;
    if (dhlen < 240 || dh[0] != 2 || np_get32(dh + 4) != g_claimed_xid || np_get32(dh + 236) != 0x63825363u) {
        g_bad_frames++;
        return;
    }
    /* Option 53 present, with a reply type; options end with 255 inside the datagram. */
    const uint8_t *o = dh + 240, *end = dh + dhlen;
    bool type_ok = false, ended = false;
    while (o < end) {
        if (*o == 255) { ended = true; break; }
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        if (o[0] == 53 && o[1] == 1)
            type_ok = o[2] == DHCP_OFFER || o[2] == DHCP_ACK || o[2] == DHCP_NAK;
        o += 2 + o[1];
    }
    if (!type_ok || !ended)
        g_bad_frames++;
}

/* --- setup and teardown -------------------------------------------------- */

static uint16_t g_last_lent;   /* the id the proxy most recently sent upstream */
static bool g_up_set;

static void setup(void)
{
    fz_random_seed(0xd4c);
    g_bad_frames = 0;
    g_nsent = 0;
    g_claimed = false;
    g_last_lent = 0;
    fz_netif_register(&g_f1, "fz1", k_mac1, IP1, MASK24, 0, NETIF_NODEFAULT);
    g_f1.on_frame = on_frame;
    memset(&g_gsock, 0, sizeof(g_gsock));
    memset(&g_usock, 0, sizeof(g_usock));
    kobject_init(&g_gsock.obj, &g_sock_type);
    kobject_init(&g_usock.obj, &g_sock_type);
    g_svc = tapsvc_test_new(&g_f1.nif);
    FUZZ_ASSERT(g_svc != NULL);
    tapsvc_test_set_sockets(g_svc, &g_gsock, &g_usock);
    tapsvc_test_set_upstream(g_svc, UPSTREAM, 53);
    g_up_set = true;
}

static void teardown(void)
{
    struct tapsvc_stats st;
    tapsvc_get_stats(&st);
    FUZZ_ASSERT(st.dns_pending <= DNS_PENDING_MAX_EXPECTED);
    tapsvc_dns_age(fz_now() + 10ull * 1000000000ull);   /* every pending query is past its 5 s */
    tapsvc_get_stats(&st);
    FUZZ_ASSERT(st.dns_pending == 0);
    tapsvc_test_free(g_svc);
    g_svc = NULL;
    fz_netif_unregister_all();
    FUZZ_ASSERT(harness_locks_held() == 0);
    FUZZ_ASSERT(g_bad_frames == 0);
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    FUZZ_ASSERT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    FUZZ_ASSERT(fz_allocs_live() == g_baseline);
}

/* --- the records --------------------------------------------------------- */

static void dhcp_record(const uint8_t *p, size_t n)
{
    uint8_t frame[FZ_CAPTURE_MAX];
    if (n > sizeof(frame))
        n = sizeof(frame);
    memcpy(frame, p, n);
    /* The xid the oracle expects a reply to echo: the request's, when the
     * frame is long enough to hold one. */
    g_claimed_xid = n >= NP_ETH + NP_IPV4 + NP_UDP + 8 ? np_get32(frame + NP_ETH + (frame[NP_ETH] & 0xf) * 4u + NP_UDP + 4)
                                                        : 0;
    unsigned tx = g_f1.transmits;
    g_claimed = true;   /* provisionally: the filter answers only what it claims, checked below */
    bool claimed = tapsvc_test_dhcp(g_svc, frame, (uint32_t)n);
    g_claimed = false;
    if (!claimed)
        FUZZ_ASSERT(g_f1.transmits == tx);   /* a frame left to the stack drew no reply from the filter */
    fz_run_work();
}

static void dns_query(const uint8_t *p, size_t n)
{
    uint8_t buf[512];
    if (n > sizeof(buf))
        n = sizeof(buf);
    memcpy(buf, p, n);
    struct netaddr from;
    memset(&from, 0, sizeof(from));
    from.family = COSMO_AF_INET;
    from.port = 40000;
    from.v4 = GUEST;
    struct tapsvc_stats s0, s1;
    tapsvc_get_stats(&s0);
    unsigned sent0 = g_nsent;
    uint8_t orig[512];
    memcpy(orig, buf, n);
    tapsvc_test_dns_query(g_svc, buf, (uint32_t)n, &from);
    tapsvc_get_stats(&s1);
    if (n < 12) {
        FUZZ_ASSERT(g_nsent == sent0);   /* shorter than a header: not a query */
        return;
    }
    FUZZ_ASSERT(s1.dns_query == s0.dns_query + 1);
    if (!g_up_set) {
        /* SERVFAIL back to the guest, through the guest socket, same length. */
        FUZZ_ASSERT(g_nsent == sent0 + 1 && g_sent[sent0 % 8].via == &g_gsock && g_sent[sent0 % 8].n == n);
        FUZZ_ASSERT(g_sent[sent0 % 8].to.v4 == GUEST && g_sent[sent0 % 8].to.port == 40000);
        FUZZ_ASSERT((g_sent[sent0 % 8].buf[2] & 0x80) && (g_sent[sent0 % 8].buf[3] & 0xf) == 2);
        return;
    }
    if (s1.dns_drop_full > s0.dns_drop_full) {
        FUZZ_ASSERT(g_nsent == sent0);
        FUZZ_ASSERT(s0.dns_pending == DNS_PENDING_MAX_EXPECTED);   /* dropped only when the table is full */
        return;
    }
    /* Relayed once, upstream, byte for byte but the id. */
    FUZZ_ASSERT(g_nsent == sent0 + 1);
    struct sent *e = &g_sent[sent0 % 8];
    FUZZ_ASSERT(e->via == &g_usock && e->to.v4 == UPSTREAM && e->to.port == 53 && e->n == n);
    if (memcmp(e->buf + 2, orig + 2, n - 2) != 0) {
        fprintf(stderr, "fuzz_dhcp_dns: the relayed query differs from the guest's beyond the id (%zu bytes):\n  sent:", n);
        for (size_t k = 0; k < n && k < 32; k++)
            fprintf(stderr, " %02x", e->buf[k]);
        fprintf(stderr, "\n  orig:");
        for (size_t k = 0; k < n && k < 32; k++)
            fprintf(stderr, " %02x", orig[k]);
        fprintf(stderr, "\n");
        FUZZ_ASSERT(!"relayed query altered");
    }
    g_last_lent = (uint16_t)((e->buf[0] << 8) | e->buf[1]);
    FUZZ_ASSERT(g_last_lent != 0);
    FUZZ_ASSERT(s1.dns_pending == s0.dns_pending + 1);
}

static void dns_answer(const uint8_t *p, size_t n, bool stranger, bool lent)
{
    uint8_t buf[512];
    if (n > sizeof(buf))
        n = sizeof(buf);
    memcpy(buf, p, n);
    if (lent && n >= 2) {
        buf[0] = (uint8_t)(g_last_lent >> 8);
        buf[1] = (uint8_t)g_last_lent;
    }
    struct netaddr from;
    memset(&from, 0, sizeof(from));
    from.family = COSMO_AF_INET;
    from.port = 53;
    from.v4 = stranger ? STRANGER : UPSTREAM;
    struct tapsvc_stats s0, s1;
    tapsvc_get_stats(&s0);
    unsigned sent0 = g_nsent;
    tapsvc_test_dns_answer(g_svc, buf, (uint32_t)n, &from);
    tapsvc_get_stats(&s1);
    if (n < 12 || stranger) {
        FUZZ_ASSERT(g_nsent == sent0 && s1.dns_answer == s0.dns_answer);
        return;
    }
    if (s1.dns_answer == s0.dns_answer) {
        FUZZ_ASSERT(g_nsent == sent0);   /* no live query lent this id: dropped */
        return;
    }
    /* Relayed to the guest that asked, with its id restored and the rest intact. */
    FUZZ_ASSERT(s1.dns_answer == s0.dns_answer + 1 && g_nsent == sent0 + 1);
    struct sent *e = &g_sent[sent0 % 8];
    FUZZ_ASSERT(e->via == &g_gsock && e->to.v4 == GUEST && e->to.port == 40000 && e->n == n);
    FUZZ_ASSERT(memcmp(e->buf + 2, p + 2, n - 2) == 0);
    FUZZ_ASSERT(s1.dns_pending + 1 == s0.dns_pending);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_net_init();
    if (g_baseline == 0)
        g_baseline = fz_allocs_live();
    setup();
    size_t off = 0;
    unsigned records = 0;
    while (off + 3 <= size && records++ < 64) {
        uint8_t kind = data[off];
        size_t len = data[off + 1] | ((size_t)data[off + 2] << 8);
        off += 3;
        if (len > size - off)
            len = size - off;
        const uint8_t *p = data + off;
        off += len;
        switch (kind & 0xf) {
        case K_DHCP:
            dhcp_record(p, len);
            break;
        case K_QUERY:
            dns_query(p, len);
            break;
        case K_ANSWER:
            dns_answer(p, len, (kind & K_STRANGER) != 0, (kind & K_LENT_ID) != 0);
            break;
        case K_TIME: {
            uint16_t ms = len >= 2 ? (uint16_t)(p[0] | (p[1] << 8)) : 1000;
            fz_clock_advance((uint64_t)ms * 1000000ull);
            tapsvc_dns_age(fz_now());
            break;
        }
        case K_PARSE: {
            char s[64];
            size_t m = len < sizeof(s) - 1 ? len : sizeof(s) - 1;
            memcpy(s, p, m);
            s[m] = '\0';
            uint32_t ip;
            if (tapsvc_test_parse_ip(s, &ip)) {
                /* A parse that succeeds reproduces the string's four octets. */
                unsigned o[4] = { 0 };
                sscanf(s, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]);
                FUZZ_ASSERT(ip == IPV4_ADDR(o[0], o[1], o[2], o[3]));
            }
            break;
        }
        case K_UPSTREAM:
            g_up_set = len >= 1 && (p[0] & 1);
            tapsvc_test_set_upstream(g_svc, g_up_set ? UPSTREAM : 0, 53);
            break;
        default:
            break;
        }
        FUZZ_ASSERT(harness_locks_held() == 0);
        struct tapsvc_stats st;
        tapsvc_get_stats(&st);
        FUZZ_ASSERT(st.dns_pending <= DNS_PENDING_MAX_EXPECTED);
    }
    teardown();
    return 0;
}

size_t fuzz_max_len(void)
{
    return 8192;
}

/* --- seeds ---------------------------------------------------------------- */

static size_t record(uint8_t *buf, size_t cap, uint8_t kind, const uint8_t *payload, size_t n)
{
    if (3 + n > cap)
        return 0;
    buf[0] = kind;
    buf[1] = (uint8_t)n;
    buf[2] = (uint8_t)(n >> 8);
    memcpy(buf + 3, payload, n);
    return 3 + n;
}

/* A DHCP request frame of message type `mt`, from `mac`, broadcast. */
static size_t dhcp_frame(uint8_t *f, uint8_t mt, const uint8_t mac[6], uint32_t reqip)
{
    uint8_t dh[300];
    memset(dh, 0, sizeof(dh));
    dh[0] = 1;   /* BOOTREQUEST */
    dh[1] = 1;   /* Ethernet */
    dh[2] = 6;
    np_put32(dh + 4, 0xdeadbeefu);
    np_put16(dh + 10, 0x8000);   /* broadcast flag */
    memcpy(dh + 28, mac, 6);
    np_put32(dh + 236, 0x63825363u);
    uint8_t *o = dh + 240;
    *o++ = 53; *o++ = 1; *o++ = mt;
    if (reqip) {
        *o++ = 50; *o++ = 4;
        memcpy(o, &reqip, 4);
        o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* parameter request list */
    *o++ = 255;
    return np_frame_udp4(f, eth_broadcast, mac, 0, INADDR_BROADCAST_N, 68, 67, dh, (size_t)(o - dh));
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    uint8_t f[600];
    static const uint8_t query[] = { 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01 };
    static const uint8_t answer[] = { 0x00, 0x00, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                      7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01,
                                      0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x04, 93, 184, 216, 34 };
    static const uint8_t other_mac[6] = { 0x02, 0xbb, 0, 0, 0, 0x16 };
    size_t n, a, b, c;
    switch (i) {
    case 0:   /* discover, request, release */
        a = record(buf, cap, K_DHCP, f, dhcp_frame(f, DHCP_DISCOVER, k_guest_mac, 0));
        b = record(buf + a, cap - a, K_DHCP, f, dhcp_frame(f, DHCP_REQUEST, k_guest_mac, GUEST));
        c = record(buf + a + b, cap - a - b, K_DHCP, f, dhcp_frame(f, DHCP_RELEASE, k_guest_mac, 0));
        return a + b + c;
    case 1:   /* a request for the wrong address: NAK; a second client: ignored */
        a = record(buf, cap, K_DHCP, f, dhcp_frame(f, DHCP_REQUEST, k_guest_mac, IPV4_ADDR(10, 75, 0, 99)));
        b = record(buf + a, cap - a, K_DHCP, f, dhcp_frame(f, DHCP_DISCOVER, other_mac, 0));
        return a + b;
    case 2:   /* a query, its answer with the lent id, and an answer from a stranger */
        a = record(buf, cap, K_QUERY, query, sizeof(query));
        b = record(buf + a, cap - a, K_ANSWER | K_LENT_ID, answer, sizeof(answer));
        c = record(buf + a + b, cap - a - b, K_ANSWER | K_STRANGER | K_LENT_ID, answer, sizeof(answer));
        return a + b + c;
    case 3: { /* no upstream: SERVFAIL; then upstream back, a query that expires */
        uint8_t off = 0, on = 1, ms[2] = { 0x88, 0x13 };   /* 5000 ms */
        a = record(buf, cap, K_UPSTREAM, &off, 1);
        b = record(buf + a, cap - a, K_QUERY, query, sizeof(query));
        c = record(buf + a + b, cap - a - b, K_UPSTREAM, &on, 1);
        size_t d = record(buf + a + b + c, cap - a - b - c, K_QUERY, query, sizeof(query));
        size_t e = record(buf + a + b + c + d, cap - a - b - c - d, K_TIME, ms, 2);
        size_t g = record(buf + a + b + c + d + e, cap - a - b - c - d - e, K_ANSWER | K_LENT_ID, answer, sizeof(answer));
        return a + b + c + d + e + g;
    }
    case 4:   /* resolver strings */
        a = record(buf, cap, K_PARSE, (const uint8_t *)"10.0.2.3", 8);
        b = record(buf + a, cap - a, K_PARSE, (const uint8_t *)"256.1.1.1", 9);
        c = record(buf + a + b, cap - a - b, K_PARSE, (const uint8_t *)"1.2.3", 5);
        return a + b + c;
    case 5:   /* a DHCP frame that is not DHCP: left to the stack */
        n = np_frame_udp4(f, k_mac1, k_guest_mac, GUEST, IP1, 5000, 53, query, sizeof(query));
        return record(buf, cap, K_DHCP, f, n);
    case 6:   /* many queries: the table fills */
        n = 0;
        for (unsigned k = 0; k < 140 && n + 3 + sizeof(query) <= cap; k++) {
            uint8_t q[sizeof(query)];
            memcpy(q, query, sizeof(q));
            q[0] = (uint8_t)k;
            n += record(buf + n, cap - n, K_QUERY, q, sizeof(q));
        }
        return n;
    default:
        return 0;
    }
}
