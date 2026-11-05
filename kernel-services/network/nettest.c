/*
 * nettest.c - Network self-tests: mbufs, checksums, ARP table, UDP and
 * TCP over loopback (with and without injected loss), and the
 * harness-driven echo services over the real interface.
 */

#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/completion.h>
#include <kernel/faultinject.h>
#include <kernel/fwcfg.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/fw.h>
#include <kernel/net/nat.h>
#include <kernel/net/raw.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/dhcp.h>
#include <uapi/cosmo/netctl.h>
#include <kernel/vfs.h>
#include <kernel/net/tap.h>
#include <kernel/net/tcp.h>
#include <kernel/random.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/percpu.h>
#include <arch/cpu.h>

#include "nettest.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* --- releases however a test returns (the net-leftover unit) ------------
 *
 * CHECK returns at once, and a test released what it made only on its last
 * lines: one failure kept a tap, a DHCP/DNS service slot and its sockets,
 * and eight tests after it failed for the slot (docs/audit/
 * next-subsystem-net-leftover.md). Every acquisition in this file goes
 * through one of these, which registers its release with the runner
 * (selftest_defer) in the same call, so no CHECK can fall between the two.
 * Every release goes through one too: it runs the registered release early
 * (selftest_release), where the test used to tear down, and falls back to
 * the plain call for anything not registered. What a test still holds when
 * it returns, the runner releases, last acquired first.
 */
/* Sockets abandoned connect threads still hold (the census leaves them
 * out: they close whenever their connect times out, maybe during a later
 * test, and must neither blame that test nor hide a socket it leaves). */
static unsigned g_nt_abandoned;
/* An abandoned thread whose connect has returned hands its socket here
 * and changes nothing else; the runner puts it at its next census. So
 * both counts the census reads change only on the runner's thread --
 * the one reading them -- however many connects finish at once. */
enum { NT_REAP_MAX = 16 };
static struct socket *g_nt_reap[NT_REAP_MAX];
static unsigned g_nt_nreap;
static spinlock_t g_nt_reap_lock = SPINLOCK_INIT("nt-reap");

static bool g_nt_finished;   /* the self-tests are over: no census will drain the list again */

static void nt_reap_abandoned(void)
{
    struct socket *take[NT_REAP_MAX];
    arch_irq_state_t s = spin_lock_irqsave(&g_nt_reap_lock);
    unsigned n = g_nt_nreap;
    for (unsigned i = 0; i < n; i++)
        take[i] = g_nt_reap[i];
    g_nt_nreap = 0;
    spin_unlock_irqrestore(&g_nt_reap_lock, s);
    for (unsigned i = 0; i < n; i++) {
        if (take[i])
            ksock_put(take[i]);
        g_nt_abandoned--;
    }
}

/* After the last test: drain the list, and from now on an abandoned
 * connect puts its own socket -- no census is left to drain it. */
void nettest_finish(void)
{
    arch_irq_state_t st = spin_lock_irqsave(&g_nt_reap_lock);
    g_nt_finished = true;
    spin_unlock_irqrestore(&g_nt_reap_lock, st);
    nt_reap_abandoned();
}

void nettest_census(struct nettest_census *out)
{
    out->nnetifs = netif_names(out->netifs, sizeof(out->netifs));
    out->services = tapsvc_count();
    /* The abandoned connects' sockets are subtracted. Any whose connect
     * has returned are put first, here, so the socket count and the
     * abandoned count both change only on this thread: they are read as
     * one state without any retry. */
    nt_reap_abandoned();
    /* A worker's wake reference (sock_wake_ref) is a socket's last for as
     * long as the worker takes between sock_wake and its put: a test that
     * has put every socket of its can return inside that window, and the
     * count then holds one it does not own (`net-accept-race` under the
     * chaos migrator, CI run 37679564267: "sockets 0 -> 1"). Counted only
     * at an instant no wake reference is held: a barrier through every
     * worker ends each window a worker had open, and a reference a thread
     * holds across its own wake is waited out, to a deadline. Past it the
     * count is taken anyway, and said to be -- a socket a wake reference
     * never lets go of is a leak, and the comparison then reports it. The
     * deadline bounds a thread's reference, not a worker: the barrier waits
     * for every worker's step without one, as netif_unregister's does, and
     * a worker that never finishes its step is a hang the runner's per-test
     * watchdog (armed across this census) reports with a scheduler dump. */
    uint64_t deadline = clock_now_ns() + 2000ull * 1000 * 1000;
    while (socket_wake_refs() != 0) {
        net_workers_barrier();
        if (socket_wake_refs() == 0 || clock_now_ns() >= deadline)
            break;
        thread_sleep_ms(1);
    }
    unsigned held = socket_wake_refs();
    if (held != 0)
        kwarn("selftest: census: %u wake reference(s) still held after 2 s; counting anyway", held);
    unsigned sockets = socket_count();
    out->sockets = sockets > g_nt_abandoned ? sockets - g_nt_abandoned : 0;
}

void nettest_census_take(struct nettest_census *out) { nettest_census(out); }

/* Is interface `name` (length n) among `list`'s comma-separated names? */
static bool census_has(const char *list, const char *name, size_t n)
{
    for (const char *p = list; *p;) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == n && memcmp(p, name, n) == 0)
            return true;
        if (!q)
            break;
        p = q + 1;
    }
    return false;
}

/*
 * Did the test leave anything? Only what it added counts. A later test
 * that finds fewer -- a socket an abandoned connect finally put, an
 * interface an earlier leftover's owner released -- has left nothing, and
 * failing it for that would be the cross-test failure this check exists
 * to end. The interface count is compared too: a name that did not fit
 * the buffer still counts.
 */
bool nettest_census_equal(const struct nettest_census *before, const struct nettest_census *after)
{
    if (after->nnetifs > before->nnetifs || after->services > before->services || after->sockets > before->sockets)
        return false;
    for (const char *p = after->netifs; *p;) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (!census_has(before->netifs, p, len))
            return false;   /* a new name, even if another went */
        if (!q)
            break;
        p = q + 1;
    }
    return true;
}

static void nt_rel_tap(void *a) { tap_destroy(a); }
static void nt_rel_svc(void *a) { tapsvc_stop(a); }
static void nt_rel_sock(void *a) { ksock_put(a); }
static void nt_rel_nif(void *a) { netif_unregister(a); }
static void nt_rel_file(void *a) { file_put(a); }

static struct tap *nt_tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t mac[6])
{
    struct tap *t = tap_create(name, ip, mask, mac);
    if (t != NULL && !selftest_defer(nt_rel_tap, t)) {
        tap_destroy(t);
        return NULL;
    }
    return t;
}

static void nt_tap_destroy(struct tap *t)
{
    if (!selftest_release(t))
        tap_destroy(t);
}

static struct tapsvc *nt_tapsvc_start(struct tap *t)
{
    struct tapsvc *svc = tapsvc_start(t);
    if (svc != NULL && !selftest_defer(nt_rel_svc, svc)) {
        tapsvc_stop(svc);
        return NULL;
    }
    return svc;
}

static void nt_tapsvc_stop(struct tapsvc *svc)
{
    if (!selftest_release(svc))
        tapsvc_stop(svc);
}

static int nt_ksock_create(int family, int type, uint32_t uid, struct socket **out)
{
    int rc = ksock_create(family, type, uid, out);
    if (rc == 0 && !selftest_defer(nt_rel_sock, *out)) {
        ksock_put(*out);
        *out = NULL;
        return -ENOMEM;
    }
    return rc;
}

static int nt_ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer)
{
    int rc = ksock_accept(s, out, peer);
    if (rc == 0 && !selftest_defer(nt_rel_sock, *out)) {
        ksock_put(*out);
        *out = NULL;
        return -ENOMEM;
    }
    return rc;
}

static void nt_ksock_put(struct socket *s)
{
    if (!selftest_release(s))
        ksock_put(s);
}

static int nt_netif_register(struct netif *nif)
{
    int rc = netif_register(nif);
    if (rc == 0 && !selftest_defer(nt_rel_nif, nif)) {
        netif_unregister(nif);
        return -ENOMEM;
    }
    return rc;
}

static void nt_netif_unregister(struct netif *nif)
{
    if (!selftest_release(nif))
        netif_unregister(nif);
}

/* A reference the test holds on an interface (a creator's, or one
 * netif_find gave it), released however it returns. Keyed by a static
 * holder, never by the interface: the interface's own address is its
 * unregister's key, and netif_find can return the very interface the test
 * registered. */
/* struct nt_netif_ref is in nettest.h. */

static void nt_rel_netif_ref(void *arg)
{
    struct nt_netif_ref *r = arg;
    if (r->nif)
        netif_put(r->nif);
    r->nif = NULL;
}

static bool nt_netif_hold(struct nt_netif_ref *r, struct netif *nif)
{
    r->nif = nif;
    if (!selftest_defer(nt_rel_netif_ref, r)) {
        nt_rel_netif_ref(r);
        return false;
    }
    return true;
}

static void nt_netif_ref_put(struct nt_netif_ref *r)
{
    if (!selftest_release(r))
        nt_rel_netif_ref(r);
}

/* A packet a fake interface kept, freed however the test returns. */
static void nt_rel_mbuf_slot(void *slot)
{
    struct mbuf **p = slot;
    if (*p)
        m_freem(*p);
    *p = NULL;
}

static int nt_vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out)
{
    int rc = vfs_open(start, path, flags, mode, out);
    if (rc == 0 && !selftest_defer(nt_rel_file, *out)) {
        file_put(*out);
        *out = NULL;
        return -ENOMEM;
    }
    return rc;
}

static void nt_file_put(struct file *f)
{
    if (!selftest_release(f))
        file_put(f);
}

/* Hooks and settings a test changes are restored however it returns: a
 * hook left installed with a pointer into a dead frame is called by the
 * next packet, and a lossy or black-hole loopback filter left in place
 * fails every loopback test after it. Changing one registers its
 * restore; putting it back runs that restore early. */
static char g_nt_lo_filter, g_nt_rx_hook, g_nt_steering, g_nt_keepalive, g_nt_fin_wait2;   /* keys */

static void nt_restore_lo_filter(void *k) { (void)k; loopback_set_filter(NULL, NULL); }
static void nt_restore_rx_hook(void *k) { (void)k; netif_set_rx_hook(NULL, NULL); }
static void nt_restore_steering(void *k) { (void)k; netif_set_steering(true); }
static void nt_restore_keepalive(void *k) { (void)k; tcp_set_keepalive(0, 0, 0); }
static void nt_restore_fin_wait2(void *k) { (void)k; tcp_set_fin_wait2(0); }

/* Changed: register the restore once. Put back: run it (or just put back). */
static void nt_changed(void *key, void (*restore)(void *))
{
    selftest_forget(key);   /* a second change keeps one restore */
    if (!selftest_defer(restore, key))
        restore(key);       /* no room: restore now rather than leave it */
}

static void nt_loopback_set_filter(lo_filter_fn fn, void *arg)
{
    loopback_set_filter(fn, arg);
    if (fn != NULL)
        nt_changed(&g_nt_lo_filter, nt_restore_lo_filter);
    else
        selftest_forget(&g_nt_lo_filter);
}

static void nt_netif_set_rx_hook(netif_rx_hook_fn fn, void *arg)
{
    netif_set_rx_hook(fn, arg);
    if (fn != NULL)
        nt_changed(&g_nt_rx_hook, nt_restore_rx_hook);
    else
        selftest_forget(&g_nt_rx_hook);
}

static void nt_netif_set_steering(bool on)
{
    netif_set_steering(on);
    if (!on)
        nt_changed(&g_nt_steering, nt_restore_steering);
    else
        selftest_forget(&g_nt_steering);
}

static void nt_tcp_set_keepalive(uint64_t idle_ns, uint64_t intvl_ns, unsigned cnt)
{
    tcp_set_keepalive(idle_ns, intvl_ns, cnt);
    if (idle_ns || intvl_ns || cnt)
        nt_changed(&g_nt_keepalive, nt_restore_keepalive);
    else
        selftest_forget(&g_nt_keepalive);
}

static void nt_tcp_set_fin_wait2(uint64_t ns)
{
    tcp_set_fin_wait2(ns);
    if (ns)
        nt_changed(&g_nt_fin_wait2, nt_restore_fin_wait2);
    else
        selftest_forget(&g_nt_fin_wait2);
}

static struct netaddr v4addr(uint32_t ip, uint16_t port)
{
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.v4 = ip;
    a.port = port;
    return a;
}

/* net-mbuf, net-mbufq-double, net-cksum and net-arp are CeeSafe
 * (nettest_base.cee); the values it writes as its own constants: */
_Static_assert(M_PKTHDR == 1 && M_EXT == 2 && M_QUEUED == (1u << 6) && NET_HEADROOM == 128 && EINPROGRESS == 115 &&
               NET_HEADROOM >= 12 + 14 + 40 + 60, "nettest_base.cee's mbuf.h and errno.h values");

/* --- UDP and TCP over loopback ------------------------------------------
 *
 * net-lo-udp, net-lo-tcp, net-tcp-delack, net-tcp-nagle-peer, net-tcp-mss
 * and net-lo-tcp-loss are CeeSafe (nettest_lo.cee), with the bulk
 * transfer they share with net-rx-dup and net-tcp-reorder
 * (nettest_tcp_transfer). Their servers' state, struct tcp_server, is in
 * nettest.h; the release framework below stays C, and nettest.h exports
 * the parts of it the batch calls. The values the batch writes as its own
 * constants, and struct netaddr's layout as its NettestAddr has it: */
_Static_assert(EIO == 5 && EPIPE == 32 && EINVAL == 22 && EMSGSIZE == 90 && EADDRINUSE == 98 && EISCONN == 106 &&
               ECONNREFUSED == 111, "nettest_lo.cee's errno.h values");
_Static_assert(COSMO_AF_INET == 2 && COSMO_AF_INET6 == 10 && COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 &&
               COSMO_SHUT_RD == 0 && COSMO_SHUT_WR == 1, "nettest_lo.cee's socket values");
_Static_assert(TCP_SNDBUF == 65536 && TCP_MSS_V4 == 1460 && TCP_MSS_V6 == 1440 && TCP_MSS_LO == 16384 &&
               TCP_DELACK_NS == 40ull * 1000000 && NET_EPHEMERAL_LO == 49152 && ETH_P_IP == 0x0800 &&
               IPPROTO_TCP == 6, "nettest_lo.cee's tcp.h, udp.h, ether.h and ip.h values");
_Static_assert(sizeof(struct netaddr) == 20 && __builtin_offsetof(struct netaddr, port) == 2 &&
               __builtin_offsetof(struct netaddr, v4) == 4 && __builtin_offsetof(struct netaddr, v6) == 4 &&
               _Alignof(struct netaddr) == 4, "nettest_lo.cee's NettestAddr is struct netaddr");
_Static_assert(sizeof(struct tcp_server) <= 512 && _Alignof(struct tcp_server) <= 8,
               "nettest_lo.cee's ServerRoom holds a struct tcp_server");

/* --- TCP over loopback -------------------------------------------------------------- */

static void tcp_server_publish(struct tcp_server *srv, struct socket **slot, struct socket *sock)
{
    arch_irq_state_t s = spin_lock_irqsave(&srv->lock);
    *slot = sock;
    spin_unlock_irqrestore(&srv->lock, s);
}

/*
 * Stop a server thread and join it, however its test returned: `stop`,
 * then shut the read side of whatever it is blocked on -- accept and recv
 * both return on it -- until it says it is done (5 s at most), then join.
 * A reference is taken under the lock, so a socket the thread puts
 * meanwhile is never shut after it has gone.
 */
static void tcp_server_release(void *arg)
{
    struct tcp_server *srv = arg;
    srv->stop = true;
    for (unsigned i = 0; i < 500 && !srv->done; i++) {
        struct socket *held[2] = { NULL, NULL };
        arch_irq_state_t s = spin_lock_irqsave(&srv->lock);
        if (srv->ls) { held[0] = srv->ls; ksock_get(held[0]); }
        if (srv->c) { held[1] = srv->c; ksock_get(held[1]); }
        spin_unlock_irqrestore(&srv->lock, s);
        for (unsigned k = 0; k < 2; k++)
            if (held[k]) {
                ksock_shutdown(held[k], COSMO_SHUT_RD);
                ksock_put(held[k]);
            }
        thread_sleep_ms(10);
    }
    if (srv->th)
        thread_join(srv->th);
    srv->th = NULL;
}

/* A server's storage outlives any frame (its thread may run after the
 * test returns), and its release is registered the moment it exists. */
static bool tcp_server_start(struct tcp_server *srv, void (*fn)(void *), const char *name, int prio)
{
    spinlock_init(&srv->lock, "tcp-server");
    srv->th = thread_create(fn, srv, name, prio);
    if (srv->th == NULL)
        return false;
    if (!selftest_defer(tcp_server_release, srv)) {
        tcp_server_release(srv);
        return false;
    }
    return true;
}

/* The framework, for the CeeSafe batches (nettest.h). */
bool nettest_tcp_server_start(struct tcp_server *srv, void (*fn)(void *), const char *name, int prio)
{
    return tcp_server_start(srv, fn, name, prio);
}

void nettest_tcp_server_publish(struct tcp_server *srv, bool listener, struct socket *sock)
{
    tcp_server_publish(srv, listener ? &srv->ls : &srv->c, sock);
}

int nettest_ksock_create(int family, int type, uint32_t uid, struct socket **out)
{
    return nt_ksock_create(family, type, uid, out);
}

int nettest_ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer)
{
    return nt_ksock_accept(s, out, peer);
}

void nettest_ksock_put(struct socket *s) { nt_ksock_put(s); }

void nettest_loopback_set_filter(lo_filter_fn fn, void *arg) { nt_loopback_set_filter(fn, arg); }

void nettest_netif_set_rx_hook(netif_rx_hook_fn fn, void *arg) { nt_netif_set_rx_hook(fn, arg); }

struct tap *nettest_tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t *mac)
{
    return nt_tap_create(name, ip, mask, mac);
}

void nettest_tap_destroy(struct tap *t) { nt_tap_destroy(t); }

struct tapsvc *nettest_tapsvc_start(struct tap *t) { return nt_tapsvc_start(t); }

void nettest_tapsvc_stop(struct tapsvc *svc) { nt_tapsvc_stop(svc); }

int nettest_netif_register(struct netif *nif) { return nt_netif_register(nif); }

void nettest_netif_unregister(struct netif *nif) { nt_netif_unregister(nif); }

bool nettest_netif_hold(struct nt_netif_ref *r, struct netif *nif) { return nt_netif_hold(r, nif); }

void nettest_netif_ref_put(struct nt_netif_ref *r) { nt_netif_ref_put(r); }

void nettest_netif_set_steering(bool on) { nt_netif_set_steering(on); }

bool nettest_defer_mbuf_slot(struct mbuf **slot) { return selftest_defer(nt_rel_mbuf_slot, slot); }

/* --- the pcb and its timers, and a duplicated received frame -----------------
 *
 * tcp-pcb-timer-free and net-rx-dup are CeeSafe (nettest_hooks.cee), each
 * exported only where its hooks exist: the timer test's body under
 * CONFIG_DEBUG, net-rx-dup under CONFIG_FAULTINJECT. Without them the C's
 * skips stand here. The pinning wrapper is this file's.
 */
bool nettest_tcp_pcb_timer_free_pinned(const char **reason);

#if !CONFIG_DEBUG
bool nettest_tcp_pcb_timer_free_pinned(const char **reason)
{
    (void)reason;
    kinfo("selftest: tcp-pcb-timer-free: no test hooks in this build; skipping");
    return true;
}
#endif

/* Pinned for the whole test: "another CPU than mine" is a claim about
 * this thread's CPU that must outlive its sleeps (S25); a callback or a
 * spinner parked on that other CPU must never find this thread queued
 * behind it. */
bool selftest_tcp_pcb_timer_free(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = nettest_tcp_pcb_timer_free_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

#if !CONFIG_FAULTINJECT
bool selftest_net_rx_dup(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-rx-dup: fault injection compiled out of this build; skipping");
    return true;
}
#endif

/* The values nettest_hooks.cee writes as its own constants, and its room: */
_Static_assert(SCHED_PRIO_DEFAULT == 32 && TICK_NS == 4000000 && FI_NET_RX_DUP == 12 && COSMO_AF_INET == 2 &&
                   ETH_HLEN == 14 && sizeof(cpumask_t) == 8,
               "nettest_hooks.cee's thread.h, timer.h, faultinject.h and ether.h values");
_Static_assert(sizeof(struct fi_stats) <= 512 && sizeof(struct arp_stats) <= 512, "nettest_hooks.cee's rooms");


static void sort_u64(uint64_t *v, unsigned n)
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

/* --- harness-driven echo over the real interface --------------------------------------- */

static volatile bool g_h_quit, g_h_stop;
static volatile int g_h_tcp_conns, g_h_udp_pkts, g_h_wwr_conns;

/*
 * The write-write-read exchange (docs/kernel-services/network/testing.md,
 * "The host harness"): a request of two WWR_HALF-byte halves, written as two
 * sends, answered with the request once both halves are in. The host drives
 * it against the service on port 8 with its default socket (Nagle on) and
 * again with TCP_NODELAY; the guest drives it the other way on the
 * back-connection, where the host answers in two writes, with and without
 * Nagle. Both sides count WWR_ROUNDS rounds per mode; tests/boot/nettest.py
 * carries the same two constants.
 */
#define WWR_HALF   16u
#define WWR_ROUNDS 50u

static void h_wwr_thread(void *arg)
{
    struct socket *ls = arg;
    for (;;) {
        struct socket *c;
        if (ksock_accept(ls, &c, NULL) != 0)
            break;
        g_h_wwr_conns++;
        uint8_t req[2 * WWR_HALF];
        for (;;) {
            size_t have = 0;
            bool eof = false;
            while (have < sizeof(req)) {
                int64_t n = ksock_recvfrom(c, req + have, sizeof(req) - have, NULL);
                if (n <= 0) {
                    eof = true;
                    break;
                }
                have += (size_t)n;
            }
            if (eof || ksock_sendto(c, req, sizeof(req), NULL) != (int64_t)sizeof(req))
                break;
        }
        ksock_put(c);
    }
    ksock_put(ls);   /* its own reference: the harness took one for it */
    thread_exit(0);
}

/* The guest's side of the reverse exchange, on the back-connection after the
 * hello: WWR_ROUNDS rounds with the host's Nagle on, then WWR_ROUNDS with it
 * off (the host switches after the first batch). Prints one line per mode;
 * false if a round did not complete. */
static bool h_wwr_client(struct socket *c)
{
    static uint64_t us[2][WWR_ROUNDS];
    uint8_t half[WWR_HALF], reply[2 * WWR_HALF];
    memset(half, 'w', sizeof(half));
    for (unsigned m = 0; m < 2; m++) {
        for (unsigned r = 0; r < WWR_ROUNDS; r++) {
            uint64_t t0 = clock_now_ns();
            if (ksock_sendto(c, half, sizeof(half), NULL) != (int64_t)sizeof(half) ||
                ksock_sendto(c, half, sizeof(half), NULL) != (int64_t)sizeof(half)) {
                kprintf("NETTEST: wwr guest-client: send failed in mode %u round %u\n", m, r);
                return false;
            }
            size_t have = 0;
            while (have < sizeof(reply)) {
                int64_t n = ksock_recvfrom(c, reply + have, sizeof(reply) - have, NULL);
                if (n <= 0) {
                    kprintf("NETTEST: wwr guest-client: recv %lld in mode %u round %u after %zu bytes\n",
                            (long long)n, m, r, have);
                    return false;
                }
                have += (size_t)n;
            }
            us[m][r] = (clock_now_ns() - t0) / 1000;
            if (memcmp(reply, half, sizeof(half)) != 0 || memcmp(reply + sizeof(half), half, sizeof(half)) != 0) {
                kprintf("NETTEST: wwr guest-client: the reply is not the request in mode %u round %u\n", m, r);
                return false;
            }
        }
        sort_u64(us[m], WWR_ROUNDS);
        kprintf("NETTEST: wwr guest-client host-nagle=%s rounds=%u min=%llu p50=%llu p90=%llu max=%llu us\n",
                m == 0 ? "on" : "off", WWR_ROUNDS,
                (unsigned long long)us[m][0], (unsigned long long)us[m][WWR_ROUNDS / 2],
                (unsigned long long)us[m][WWR_ROUNDS * 9 / 10], (unsigned long long)us[m][WWR_ROUNDS - 1]);
    }
    return true;
}

static void h_tcp_echo_thread(void *arg)
{
    struct socket *ls = arg;
    for (;;) {
        struct socket *c;
        if (ksock_accept(ls, &c, NULL) != 0)
            break;
        g_h_tcp_conns++;
        uint8_t *buf = kmalloc(8192, 0);
        bool quit = false;
        for (;;) {
            int64_t n = ksock_recvfrom(c, buf, 8192, NULL);
            if (n <= 0)
                break;
            if (n >= 4 && memcmp(buf, "QUIT", 4) == 0) {
                quit = true;
                break;
            }
            if (ksock_sendto(c, buf, (size_t)n, NULL) != n)
                break;
        }
        kfree(buf);
        ksock_put(c);
        if (quit) {
            g_h_quit = true;
            break;
        }
    }
    ksock_put(ls);   /* its own reference: the harness took one for it */
    thread_exit(0);
}

static void h_udp_echo_thread(void *arg)
{
    struct socket *s = arg;
    uint8_t *buf = kmalloc(2048, 0);
    while (!g_h_stop) {
        struct netaddr from;
        int64_t n = ksock_recvfrom(s, buf, 2048, &from);
        if (n <= 0)
            break;
        if (g_h_stop)
            break;
        g_h_udp_pkts++;
        ksock_sendto(s, buf, (size_t)n, &from);
    }
    kfree(buf);
    ksock_put(s);   /* its own reference: the harness took one for it */
    thread_exit(0);
}

/*
 * How many times the guest will run the back-connection exchange.
 *
 * Three, and the number comes from the shape of the defect rather than
 * taste: every recorded sighting is a *single* connection being reset,
 * never a sequence, and slirp answered a probe through the same instance
 * in 1 ms while one connection was dead. If a second and a third attempt
 * both die, what is being measured is no longer the flake this bound was
 * written for, and the test should say so by failing.
 */
#define HARNESS_ATTEMPTS 3u

#if CONFIG_DEBUG
/*
 * Break the first N attempts from inside the guest, so the retry path
 * runs on every boot of a build that asks for it rather than on one boot
 * in twenty. It shuts the socket down both ways after the connect, which
 * is the shape the defect leaves behind -- a connection that completed
 * its handshake and cannot carry the exchange -- without pretending to
 * reproduce slirp's cause, which is not reproducible from in here.
 * Set by CONFIG_HARNESS_BREAK (`make test-harness-retry`).
 */
static unsigned g_h_break_attempts = CONFIG_HARNESS_BREAK;
#endif

bool selftest_net_harness(const char **reason)
{
    char cfg[64];
    if (!fwcfg_get_string("nettest", cfg, sizeof(cfg))) {
        kinfo("selftest: net-harness: no opt/cosmo/nettest parameter; skipping");
        return true;
    }
    struct netif *nif = netif_default();
    CHECK(nif != NULL && nif->ip4.addr != 0);
    netif_put(nif);
    unsigned hostport = 0;
    if (strncmp(cfg, "tcp=", 4) == 0) {
        for (const char *p = cfg + 4; *p >= '0' && *p <= '9'; p++)
            hostport = hostport * 10 + (unsigned)(*p - '0');
    }
    CHECK(hostport > 0 && hostport < 65536);

    /* Echo services on port 7 for the harness's port forwards. */
    struct socket *tls, *us;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &tls) == 0);
    struct netaddr any7 = v4addr(0, 7);
    CHECK(ksock_bind(tls, &any7) == 0 && ksock_listen(tls, 4) == 0);
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &us) == 0);
    CHECK(ksock_bind(us, &any7) == 0);
    /* The write-write-read service on port 8 (h_wwr_thread). */
    struct socket *wls;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &wls) == 0);
    struct netaddr any8 = v4addr(0, 8);
    CHECK(ksock_bind(wls, &any8) == 0 && ksock_listen(wls, 4) == 0);
    g_h_quit = g_h_stop = false;
    g_h_tcp_conns = g_h_udp_pkts = g_h_wwr_conns = 0;
    ksock_get(tls);
    ksock_get(us);
    ksock_get(wls);
    CHECK(thread_create(h_tcp_echo_thread, tls, "nettest-tcp", 32) != NULL);
    CHECK(thread_create(h_udp_echo_thread, us, "nettest-udp", 32) != NULL);
    CHECK(thread_create(h_wwr_thread, wls, "nettest-wwr", 32) != NULL);
    kprintf("NETTEST: ready tcp=7 udp=7 wwr=8\n");

    /*
     * Connect back to the harness through the gateway (QEMU forwards
     * 10.0.2.2 to the host's loopback), and do it up to
     * HARNESS_ATTEMPTS times.
     *
     * The retry is not for this kernel's benefit. QEMU's user-mode
     * networking resets the guest's half of one connection while keeping
     * its own half open, and answers a probe through the same instance a
     * millisecond later; the guest is correct from first SYN to final
     * reset, verified against a packet capture. Three units localised
     * that, and a fourth could only find the same answer, because what
     * is left is in slirp's source
     * (docs/audit/next-subsystem-nettest-retry.md).
     *
     * So the exchange retries, and three things keep it honest:
     *
     *   - the bound is a **failure**, not a fallback: exhausting it
     *     fails the test exactly as one reset does today, with every
     *     attempt's full diagnostics;
     *   - every attempt prints those diagnostics, so a boot that needed
     *     a second one says what went wrong with the first;
     *   - both outcome lines name the attempt, so a *passing* boot whose
     *     first attempt was reset is still a sighting, countable by the
     *     same grep, and `docs/testing/flakes.md` keeps counting it.
     *
     * A retry that hid the flake would be worse than the flake.
     */
    bool client_ok = false, wwr_ok = false;
    unsigned attempt = 0;
    struct socket *c = NULL;
    struct netaddr host = v4addr(nif->ip4.gateway, (uint16_t)hostport);
    for (attempt = 1; attempt <= HARNESS_ATTEMPTS && !client_ok; attempt++) {
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    /* Sampled BEFORE the connect. PR #169's window opened after it
     * returned, so a reset arriving during the handshake or before the
     * first sample fell outside it -- which is why one failure showed
     * `rsts_in +0` and another `+1` for the same defect. */
    struct tcp_stats t0, t1;
    tcp_get_stats(&t0);
    uint64_t ns0 = clock_now_ns();
    int rc = ksock_connect(c, &host);
    uint64_t connect_ms = (clock_now_ns() - ns0) / 1000000ull;
#if CONFIG_DEBUG
    if (rc == 0 && attempt <= __atomic_load_n(&g_h_break_attempts, __ATOMIC_ACQUIRE)) {
        /* The injected shape: connected, and then unusable. */
        ksock_shutdown(c, COSMO_SHUT_RDWR);
        kprintf("NETTEST: attempt %u broken on purpose\n", attempt);
    }
#endif

    /*
     * This exchange has failed repeatedly, on both architectures and on
     * CI and locally (the tally is kept in docs/testing/flakes.md, "The
     * count", and deliberately nowhere else), and until now said only
     * "client failed (0)" -- the
     * *connect's* result, printed for a failure in a later step
     * (docs/audit/next-subsystem-twelve-bytes.md).
     *
     * What is asked instead is the connection itself. Data sits in the
     * send buffer until it is acknowledged, so `space` before the send,
     * after it and after the read says whether the twelve bytes were
     * queued and whether they were ever acknowledged -- for this
     * connection, which the global counters below cannot be, since they
     * count this connect's own SYN and every other socket's traffic.
     *
     * The distinction matters because QEMU's user-mode networking is a
     * proxy rather than a wire: it terminates the guest's TCP in its own
     * stack and writes onward to a separate host socket, so it
     * acknowledges these bytes into its own buffer before the host has
     * seen them. A drained send buffer with nothing at the far end is
     * therefore a real and expected outcome, and it is the one that says
     * this kernel is not at fault.
     */
    int64_t sent = -1, got = -1;
    uint32_t space0 = 0, space1 = 0, space2 = 0;
    uint64_t send_ms = 0, recv_ms = 0;
    if (rc == 0) {
        space0 = tcp_send_space(c->tcp);
        uint64_t ns1 = clock_now_ns();
        sent = ksock_sendto(c, "cosmo hello\n", 12, NULL);
        send_ms = (clock_now_ns() - ns1) / 1000000ull;
        space1 = tcp_send_space(c->tcp);
        if (sent == 12) {
            char buf[32];
            uint64_t ns2 = clock_now_ns();
            got = ksock_recvfrom(c, buf, sizeof(buf), NULL);
            recv_ms = (clock_now_ns() - ns2) / 1000000ull;
            client_ok = got == 12 && memcmp(buf, "cosmo world\n", 12) == 0;
        }
        space2 = tcp_send_space(c->tcp);
    }
    /* The reverse write-write-read exchange rides on this connection once
     * the hello has been answered: the host serves it before closing. */
    wwr_ok = client_ok && h_wwr_client(c);
    tcp_get_stats(&t1);
    enum tcp_state st = tcp_state_of(c->tcp);
    /* The socket's own verdict, which no caller could ask for until this
     * unit: `rsts_in` is machine-wide and says a reset happened somewhere,
     * where this names the errno THIS connection died of. */
    int pending = ksock_error(c);
    /* The connection's timer and work state, read while the socket is
     * still ours: the put below closes the pcb and frees the socket. */
    int rx_state = c->tcp ? (int)c->tcp->rexmit.state : -1;
    unsigned rx_cpu = c->tcp ? c->tcp->rexmit.cpu : 0u;
    long long rx_in_ms = c->tcp ? (long long)((int64_t)c->tcp->rexmit.expires_ns - (int64_t)clock_now_ns()) / 1000000 : 0;
    unsigned long long rto_ms = c->tcp ? (unsigned long long)(c->tcp->rto_ns / 1000000) : 0ull;
    int work_queued = c->tcp ? (int)__atomic_load_n(&c->tcp->work.queued, __ATOMIC_ACQUIRE) : -1;
    unsigned work_flags = c->tcp ? __atomic_load_n(&c->tcp->work_flags, __ATOMIC_ACQUIRE) : 0u;
    int pcb_state = c->tcp ? (int)c->tcp->state : -1;
    nt_ksock_put(c);
    c = NULL;
    if (client_ok) {
        kprintf("NETTEST: client ok (attempt %u of %u)\n", attempt, HARNESS_ATTEMPTS);
    } else {
        /* The three samples are labelled by *when*, not by what they are
         * taken to mean. space1 is read after ksock_sendto has released
         * the pcb lock, so an acknowledgement can already have drained
         * the buffer -- "outstanding after the send" is 0 in that case
         * and nothing is wrong, whereas calling it "queued" would print
         * `sent 12, queued 0` and contradict itself. The discriminator
         * is the third: still outstanding after the read gave up means
         * the twelve bytes were never acknowledged. */
        /* The connection's own timer and work state first, on a short line
         * of its own: the long line below has been cut mid-way by another
         * thread's print in the logs this is for. */
        kprintf("NETTEST: client state: rexmit timer state %d on cpu %u expiring in %lld ms (rto %llu ms), "
                "work queued %d flags 0x%x, pcb state %d\n",
                rx_state, rx_cpu, rx_in_ms, rto_ms, work_queued, work_flags, pcb_state);
        kprintf("NETTEST: client failed: connect %d in %llu ms, sent %lld in %llu ms, "
                "recv %lld in %llu ms, pending error %d, "
                "sndbuf free %u before, %u after send, %u after read "
                "(outstanding %d then %d), state %d, "
                "segs_out +%llu retransmits +%llu refused +%llu rsts_in +%llu "
                "(counters from before the connect)\n",
                rc, (unsigned long long)connect_ms,
                (long long)sent, (unsigned long long)send_ms,
                (long long)got, (unsigned long long)recv_ms, pending,
                space0, space1, space2,
                (int)(space0 - space1), (int)(space0 - space2), (int)st,
                (unsigned long long)(t1.segs_out - t0.segs_out),
                (unsigned long long)(t1.retransmits - t0.retransmits),
                (unsigned long long)(t1.out_refused - t0.out_refused),
                (unsigned long long)(t1.rsts_in - t0.rsts_in));
        kprintf("NETTEST: client attempt %u of %u failed\n", attempt, HARNESS_ATTEMPTS);
    }
    }   /* for each attempt */
    if (!client_ok)
        kprintf("NETTEST: client failed every attempt (%u of %u)\n", HARNESS_ATTEMPTS, HARNESS_ATTEMPTS);

    /* Serve echo until the harness sends QUIT (60 s budget). */
    for (unsigned i = 0; i < 6000 && !g_h_quit; i++) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
    g_h_stop = true;
    ksock_shutdown(tls, COSMO_SHUT_RD);   /* accept returns */
    ksock_shutdown(wls, COSMO_SHUT_RD);
    struct netaddr self = v4addr(INADDR_LOOPBACK_N, 7);
    ksock_sendto(us, "x", 1, &self);       /* the UDP thread wakes and exits */
    thread_sleep_ms(50);
    kprintf("NETTEST: done tcp_conns=%d udp_pkts=%d wwr_conns=%d quit=%d\n", g_h_tcp_conns, g_h_udp_pkts,
            g_h_wwr_conns, g_h_quit ? 1 : 0);
    nt_ksock_put(tls);
    nt_ksock_put(us);
    nt_ksock_put(wls);
    CHECK(client_ok);
    CHECK(wwr_ok);
    CHECK(g_h_quit);
    return true;
}

/* --- the interface's lifetime against the neighbour tables -------------- *
 *
 * net-arp-retry-unregister, net-nd-retry-unregister, net-arp-flush-counts,
 * net-nd-flush-counts and net-netif-lifetime are CeeSafe
 * (nettest_lifetime.cee), with fake interfaces of their own. The two retry
 * tests need the debug build's park hooks; without them the C's skips stand
 * here. The values the port writes as its own constants, and its rooms:
 */
#if !CONFIG_DEBUG
bool selftest_net_arp_retry_unregister(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-arp-retry-unregister: needs the debug retry hook; skipping");
    return true;
}

bool selftest_net_nd_retry_unregister(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-nd-retry-unregister: needs the debug retry hook; skipping");
    return true;
}
#endif
_Static_assert(EINVAL == 22 && EEXIST == 17 && ENODEV == 19 && EINPROGRESS == 115 && NETIF_UP == 1 &&
                   NETIF_LOOPBACK == 2 && NETIF_NOARP == 4 && NETIF_GONE == 8 && NETIF_NODEFAULT == 0x10 &&
                   SCHED_PRIO_DEFAULT == 32 && NS_PER_SEC == 1000000000ull,
               "nettest_lifetime.cee's errno.h, netif.h, thread.h and time values");
_Static_assert(__builtin_offsetof(struct netif, obj) == 0 && sizeof(struct arp_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_lifetime.cee's object and rooms");

/* --- a neighbour entry is (interface, address) ---------------------------- *
 *
 * net-arp-per-interface, net-nd-per-interface and net-neigh-down-race are
 * CeeSafe (nettest_neigh.cee), with fake interfaces of their own. The down
 * race needs the debug build's park hooks; without them the C's skip stands
 * here. The values the port writes as its own constants, its rooms and the
 * IPv6 header it builds:
 */
#if !CONFIG_DEBUG
bool selftest_net_neigh_down_race(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-neigh-down-race: needs the debug park hooks; skipping");
    return true;
}
#endif
_Static_assert(EINPROGRESS == 115 && ENETUNREACH == 101 && ENOMEM == 12 && NETIF_UP == 1 && NETIF_NODEFAULT == 0x10 &&
                   ETH_HLEN == 14 && ETH_P_IP == 0x0800 && ETH_P_ARP == 0x0806 && ETH_P_IPV6 == 0x86DD &&
                   ICMPV6_NS == 135 && ICMPV6_NA == 136 && SCHED_PRIO_DEFAULT == 32 && NS_PER_SEC == 1000000000ull,
               "nettest_neigh.cee's errno.h, netif.h, ether.h, ip.h, thread.h and time values");
_Static_assert(sizeof(struct ipv6_hdr) == 40 && __builtin_offsetof(struct ipv6_hdr, src) == 8 &&
                   __builtin_offsetof(struct ipv6_hdr, dst) == 24 && sizeof(struct in6_addr) == 16 &&
                   sizeof(struct arp_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_neigh.cee's IPv6 header and rooms");

/* --- accept against its races, the census against a wake reference -------
 *
 * net-accept-race, net-census-wake-ref and net-accept-order are CeeSafe
 * (nettest_accept.cee), with the listen-backlog step net-lo-tcp shares and
 * the wait they used. The latter two need the debug build's seams; without
 * them the C's skips stand here. The values the port writes as its own
 * constants:
 */
#if !CONFIG_DEBUG
bool selftest_net_census_wake_ref(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-census-wake-ref: no test hooks in this build; skipping");
    return true;
}

bool selftest_net_accept_order(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-accept-order: no test hooks in this build; skipping");
    return true;
}
#endif
_Static_assert(EINVAL == 22 && COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 && COSMO_IO_READABLE == 1 &&
                   NET_EPHEMERAL_LO == 49152 && SCHED_PRIO_DEFAULT == 32,
               "nettest_accept.cee's errno.h, syscall.h, inet.h and thread.h values");

/* --- milestone 8: hardening -------------------------------------------------------- *
 *
 * net-tcp-syncache, net-tcp-rfc5961, net-tcp-reorder, net-tcp-keepalive,
 * net-icmp-limit, net-raw-icmp and net-nonblock are CeeSafe
 * (nettest_tcp.cee), with their injected segment, filters, holding server
 * and predicates; nettest_tcp.cee and nettest_accept.cee each have
 * nettest.c's old wait_until under its contract. The values it writes as
 * its own constants: */
_Static_assert(EAGAIN == 11 && EDESTADDRREQ == 89 && EPROTONOSUPPORT == 93 && EOPNOTSUPP == 95 && ECONNRESET == 104 &&
               EISCONN == 106 && ETIMEDOUT == 110 && EALREADY == 114 && EINPROGRESS == 115,
               "nettest_tcp.cee's errno.h values");
_Static_assert(COSMO_IO_READABLE == 1 && COSMO_IO_WRITABLE == 2 && COSMO_IO_HANGUP == 4 && TH_SYN == 0x02 &&
               TH_RST == 0x04 && TH_ACK == 0x10 && TCP_CLOSED == 0 && TCP_ESTABLISHED == 4 &&
               TCP_SYNCACHE_SIZE == 64 && sizeof(struct tcp_hdr) == 20 && sizeof(struct icmp_hdr) == 8,
               "nettest_tcp.cee's syscall.h and tcp.h values");
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_UDP == 17 && IP_DEFAULT_TTL == 64 && ICMP_ECHO_REPLY == 0 &&
               ICMP_ECHO == 8 && ICMP_DEST_UNREACH == 3 && ICMP_UNREACH_NEEDFRAG == 4 && ICMP_RATE_PER_SEC == 100 &&
               RAW_RXQ_MAX == 64 && PIPE_SIZE == 16384 && SCHED_PRIO_DEFAULT == 32,
               "nettest_tcp.cee's ip.h, raw.h, pipe.h and thread.h values");

/* The framework for nettest_tcp.cee (nettest.h). */
int nettest_ksock_create_raw(int family, int proto, struct socket **out)
{
    int rc = ksock_create_raw(family, proto, 0, out);
    if (rc == 0 && !selftest_defer(nt_rel_sock, *out)) {
        ksock_put(*out);
        *out = NULL;
        return -ENOMEM;
    }
    return rc;
}

void nettest_tcp_set_keepalive(uint64_t idle_ns, uint64_t intvl_ns, unsigned cnt)
{
    nt_tcp_set_keepalive(idle_ns, intvl_ns, cnt);
}

void nettest_tcp_set_fin_wait2(uint64_t ns) { nt_tcp_set_fin_wait2(ns); }

unsigned nettest_raw_queued(struct socket *s) { return mbufq_len(&s->raw->rxq); }

int64_t nettest_kobject_read(struct kobject *obj, void *buf, size_t len)
{
    return kobject_io_of(obj)->read(obj, buf, len);
}

int64_t nettest_kobject_write(struct kobject *obj, const void *buf, size_t len)
{
    return kobject_io_of(obj)->write(obj, buf, len);
}

/* --- receive steering and checksum offload (unit 11) ------------------------
 *
 * net-steer and net-csum-offload are CeeSafe (nettest_nif.cee). The
 * values it writes as its own constants: */
_Static_assert(NETIF_LOOPBACK == 2 && NETIF_CAP_TXCSUM == 1 && NETIF_CAP_RXCSUM == 2 && M_CSUM_OK == 16 &&
               NET_CSUM_TCP == 1 && NET_CSUM_TX == 3 && CONFIG_MAX_CPUS == 64 && sizeof(((struct netif *)0)->name) == 8,
               "nettest_nif.cee's netif.h, mbuf.h and percpu.h values");

/* --- benchmark (unit 11) ----------------------------------------------------------
 *
 * Reports, never fails on timing: loopback TCP throughput with one and
 * with two concurrent flows, and UDP datagrams per second, each with
 * steering off (the single-queue architecture) and on.
 *
 * Each phase is a fixed time window, not a fixed amount of work (restated
 * 2026-10-11, as net-nicbench's UDP phase was in PR #317): a TCP client
 * sends until its window closes and the sink must then have received
 * exactly what was sent; the UDP sender sends until its window closes. The
 * rates are over the window. A fixed amount of work made the bench's wall
 * time the host's speed -- on CI's aarch64 runners 6-8 s against the 8 s
 * hang guard with every assertion passing, and neither retransmission
 * timeouts nor drops in the slow rounds (docs/testing/flakes.md). Each
 * round still prints its counters (bench_report).
 */
#define BENCH_TCP_WINDOW_NS (300ull * 1000000ull)
#define BENCH_UDP_WINDOW_NS (300ull * 1000000ull)

struct bench_sink {
    struct netaddr addr;
    uint32_t bytes;
    int err;
    volatile bool done;
};

static void bench_sink_main(void *arg)
{
    struct bench_sink *b = arg;
    struct socket *ls, *c;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0) {
        if (ksock_bind(ls, &b->addr) == 0 && ksock_listen(ls, 2) == 0 && ksock_accept(ls, &c, NULL) == 0) {
            uint8_t *buf = kmalloc(16384, 0);
            for (;;) {
                int64_t n = ksock_recvfrom(c, buf, 16384, NULL);
                if (n <= 0) {
                    b->err = (int)n;
                    break;
                }
                b->bytes += (uint32_t)n;
            }
            kfree(buf);
            ksock_put(c);
        } else {
            b->err = -1000;
        }
        ksock_put(ls);
    } else {
        b->err = -1001;
    }
    b->done = true;
    thread_exit(0);
}

struct bench_client {
    struct netaddr addr;
    uint64_t window_ns;  /* send for this long from the connection, then shut down */
    uint64_t took_ns;    /* from the connection to the shutdown, measured by the client */
    uint32_t sent;
    int err;
    volatile bool done;
};

static void bench_client_main(void *arg)
{
    struct bench_client *cl = arg;
    struct socket *c;
    uint8_t *buf = kmalloc(16384, 0);
    if (buf && ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0) {
        int rc = ksock_connect(c, &cl->addr);
        if (rc == 0) {
            /* The window starts here, on this thread, and is read with the
             * deadline helpers: a client that connected late still gets its
             * whole window, and a migration cannot shorten it. */
            uint64_t t0 = clock_now_ns(), until = clock_deadline_ns(cl->window_ns);
            while (!clock_deadline_passed(until)) {
                int64_t w = ksock_sendto(c, buf, 16384, NULL);
                if (w <= 0) {
                    cl->err = (int)w;
                    break;
                }
                cl->sent += (uint32_t)w;
            }
            cl->took_ns = clock_since_ns(t0);
            ksock_shutdown(c, COSMO_SHUT_WR);
        } else {
            cl->err = rc;
        }
        ksock_put(c);
    }
    kfree(buf);
    cl->done = true;
    thread_exit(0);
}

/* What one net-bench round lost time to (the owner's net-bench question,
 * 2026-10-11): TCP's retransmission timeouts and their backoff, fast
 * retransmits, the time senders waited on a retransmission timer, each
 * netrx worker's receive-queue drops and loopback's own drops, and the
 * round's wall time. Taken before and after each round. */
#define BENCH_CPUS CONFIG_MAX_CPUS
struct bench_snap {
    struct tcp_stats t;
    uint64_t cpu_drop[BENCH_CPUS];
    uint64_t lo_drop;
    uint64_t at_ns;
};

static void bench_snap(struct bench_snap *s)
{
    tcp_get_stats(&s->t);
    for (unsigned c = 0; c < BENCH_CPUS; c++) {
        struct net_cpu_stats cs;
        s->cpu_drop[c] = netif_cpu_stats(c, &cs) ? cs.rx_dropped : 0;
    }
    struct netif *lo = netif_loopback();
    s->lo_drop = lo ? lo->stats.rx_dropped + lo->stats.tx_dropped : 0;
    if (lo)
        netif_put(lo);
    s->at_ns = clock_now_ns();
}

static void bench_report(const char *what, const struct bench_snap *a, const struct bench_snap *b)
{
    char drops[BENCH_CPUS * 8];
    size_t off = 0;
    drops[0] = '\0';
    for (unsigned c = 0; c < BENCH_CPUS && c < cpu_count(); c++)
        off += (size_t)ksnprintf(drops + off, sizeof(drops) - off, "%s%llu", c ? "," : "",
                                 (unsigned long long)(b->cpu_drop[c] - a->cpu_drop[c]));
    kinfo("net-bench: %s: %llu ms; rto fires %llu (max backoff %llu), fast retransmits %llu, retransmits %llu, "
          "rto wait %llu ms; netrx drops [%s], loopback drops %llu",
          what, (unsigned long long)((b->at_ns - a->at_ns) / 1000000),
          (unsigned long long)(b->t.rto_fires - a->t.rto_fires), (unsigned long long)b->t.rto_backoff_max,
          (unsigned long long)(b->t.fast_retransmits - a->t.fast_retransmits),
          (unsigned long long)(b->t.retransmits - a->t.retransmits),
          (unsigned long long)((b->t.rto_wait_ns - a->t.rto_wait_ns) / 1000000), drops,
          (unsigned long long)(b->lo_drop - a->lo_drop));
}

/* `flows` concurrent transfers for BENCH_TCP_WINDOW_NS each, every one of
 * which must arrive whole; returns MiB/s in total -- each flow's bytes over
 * its own window, summed -- or 0 on failure. */
static unsigned bench_tcp(unsigned flows, uint16_t port)
{
    struct bench_sink sinks[2];
    struct bench_client clients[2];
    struct thread *ts[2], *tc[2];
    if (flows > 2)
        flows = 2;
    for (unsigned i = 0; i < flows; i++) {
        memset(&sinks[i], 0, sizeof(sinks[i]));
        sinks[i].addr = v4addr(INADDR_LOOPBACK_N, (uint16_t)(port + i));
        ts[i] = thread_create(bench_sink_main, &sinks[i], "bench-sink", SCHED_PRIO_DEFAULT);
    }
    thread_sleep_ms(20);
    static struct bench_snap sa, sb; /* a CPU array each: off the stack (one bench at a time) */
#if CONFIG_SELFTEST
    tcp_test_reset_rto_backoff_max();
#endif
    bench_snap(&sa);
    struct tcp_stats tb;
    tcp_get_stats(&tb);
    uint64_t qb = 0;
    for (unsigned c = 0; c < BENCH_CPUS; c++) {
        struct net_cpu_stats cs;
        if (netif_cpu_stats(c, &cs))
            qb += cs.rx_dropped;
    }
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < flows; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].addr = sinks[i].addr;
        clients[i].window_ns = BENCH_TCP_WINDOW_NS;
        tc[i] = thread_create(bench_client_main, &clients[i], "bench-client", SCHED_PRIO_DEFAULT);
    }
    uint64_t rate = 0;
    bool whole = true;
    for (unsigned i = 0; i < flows; i++) {
        if (tc[i])
            thread_join(tc[i]);
        if (ts[i])
            thread_join(ts[i]);
        /* Each flow on its own: no error, something sent, and exactly that
         * received -- a failed or empty flow beside a good one is a failure,
         * not a lower rate. */
        if (tc[i] == NULL || ts[i] == NULL || clients[i].err != 0 || clients[i].sent == 0 ||
            sinks[i].bytes != clients[i].sent || clients[i].took_ns == 0)
            whole = false;
        else
            rate += (uint64_t)clients[i].sent * 1000000000ull / clients[i].took_ns;
    }
    uint64_t dt = clock_since_ns(t0);
    bench_snap(&sb);
    {
        char what[24];
        ksnprintf(what, sizeof(what), "tcp %u flow(s)", flows);
        bench_report(what, &sa, &sb);
    }
    /* What a slow run lost (docs/testing/flakes.md, 2026-10-10: some CI
     * boots run one flow at 1 MiB/s): the TCP counters and the receive
     * queues' drops over this run. */
    struct tcp_stats ta;
    tcp_get_stats(&ta);
    uint64_t qa = 0;
    for (unsigned c = 0; c < BENCH_CPUS; c++) {
        struct net_cpu_stats cs;
        if (netif_cpu_stats(c, &cs))
            qa += cs.rx_dropped;
    }
    kinfo("net-bench: tcp %u flow(s) in %llu ms: segs in %llu out %llu, retransmits %llu, timeouts %llu, "
          "out of order %llu (queued %llu dropped %llu), receive-queue drops %llu",
          flows, (unsigned long long)(dt / 1000000), (unsigned long long)(ta.segs_in - tb.segs_in),
          (unsigned long long)(ta.segs_out - tb.segs_out), (unsigned long long)(ta.retransmits - tb.retransmits),
          (unsigned long long)(ta.timeouts - tb.timeouts), (unsigned long long)(ta.out_of_order - tb.out_of_order),
          (unsigned long long)(ta.ooo_queued - tb.ooo_queued), (unsigned long long)(ta.ooo_dropped - tb.ooo_dropped),
          (unsigned long long)(qa - qb));
    if (!whole || dt == 0) {
        for (unsigned i = 0; i < flows; i++)
            kwarn("net-bench: flow %u: sink got %u (err %d), client sent %u (err %d)", i, sinks[i].bytes, sinks[i].err,
                  clients[i].sent, clients[i].err);
        return 0;
    }
    return (unsigned)(rate / (1024ull * 1024ull));
}

struct bench_udp_rx {
    struct socket *s;
    uint32_t got;
    volatile bool stop, done;
};

static void bench_udp_rx_main(void *arg)
{
    struct bench_udp_rx *r = arg;
    uint8_t buf[128];
    while (!r->stop) {
        int64_t n = ksock_recvfrom(r->s, buf, sizeof(buf), NULL);
        if (n <= 0)
            break;
        r->got++;
    }
    r->done = true;
    thread_exit(0);
}

/* Datagrams to a loopback receiver for BENCH_UDP_WINDOW_NS: the send rate in
 * datagrams per second (0 on failure) and how many the receiver got (the
 * socket queue holds UDP_RXQ_MAX; the rest were dropped there). */
static unsigned bench_udp(uint16_t port, uint32_t *delivered, uint32_t *sent_out)
{
    *delivered = 0;
    *sent_out = 0;
    struct netaddr addr = v4addr(INADDR_LOOPBACK_N, port);
    struct socket *rx, *tx;
    if (nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &rx) != 0)
        return 0;
    if (ksock_bind(rx, &addr) != 0 || nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &tx) != 0) {
        nt_ksock_put(rx);
        return 0;
    }
    struct bench_udp_rx r = { .s = rx };
    struct thread *t = thread_create(bench_udp_rx_main, &r, "bench-udp", SCHED_PRIO_DEFAULT);
    uint8_t payload[64] = { 0 };
    static struct bench_snap sa, sb; /* a CPU array each: off the stack (one bench at a time) */
#if CONFIG_SELFTEST
    tcp_test_reset_rto_backoff_max();   /* per round: a TCP round's peak is not this one's */
#endif
    bench_snap(&sa);
    uint64_t t0 = clock_now_ns(), until = clock_deadline_ns(BENCH_UDP_WINDOW_NS);
    uint32_t sent = 0;
    for (unsigned i = 0; !clock_deadline_passed(until); i++) {
        if (ksock_sendto(tx, payload, sizeof(payload), &addr) == (int64_t)sizeof(payload))
            sent++;
        if ((i & 63) == 63)
            sched_yield();   /* let the receiver drain: the socket queue is short */
    }
    uint64_t dt = clock_since_ns(t0);
    for (unsigned i = 0; i < 50 && r.got < sent; i++)
        thread_sleep_ms(2);
    bench_snap(&sb);
    bench_report("udp", &sa, &sb);
    *delivered = r.got;
    *sent_out = sent;
    r.stop = true;
    ksock_set_nonblock(rx, true);   /* the receiver's blocked recv returns */
    ksock_shutdown(rx, COSMO_SHUT_RD);
    if (t)
        thread_join(t);
    nt_ksock_put(tx);
    nt_ksock_put(rx);
    if (dt == 0)
        return 0;
    return (unsigned)(((uint64_t)sent * 1000000000ull) / dt);
}

bool selftest_net_bench(const char **reason)
{
    (void)reason;
    unsigned ncpu = cpu_count();
    for (unsigned steer = 0; steer < 2; steer++) {
        nt_netif_set_steering(steer != 0);
        /* Fresh ports per round: the previous round's connections may still
         * be in TIME_WAIT on theirs. */
        uint16_t base = (uint16_t)(6100 + 40 * steer);
        uint32_t delivered = 0, sent = 0;
        unsigned one = bench_tcp(1, base), two = bench_tcp(2, (uint16_t)(base + 10));
        unsigned pps = bench_udp((uint16_t)(base + 20), &delivered, &sent);
        kinfo("net-bench: steer=%u cpus=%u: tcp 1 flow %u MiB/s, 2 flows %u MiB/s total, udp %u sends/s (%u of %u delivered)",
              steer, ncpu, one, two, pps, delivered, sent);
    }
    nt_netif_set_steering(true);
    return true;
}

/* The first other non-loopback interface, referenced (net-nicbench's second
 * interface; nettest_nat.cee's net-second-nic has its own). */
static struct netif *find_other_interface(struct netif *not_this)
{
    for (unsigned i = 0; i < 16; i++) {
        char name[8];
        ksnprintf(name, sizeof(name), "eth%u", i);
        struct netif *n = netif_find(name);
        if (n == NULL)
            continue;
        if (n != not_this && !(n->flags & NETIF_LOOPBACK))
            return n;
        netif_put(n);
    }
    return NULL;
}

/* --- the NIC-path benchmark (design.md, "The NIC-path benchmark") ----------------
 *
 * Traffic that leaves the machine, per interface: ARP round trips
 * counted at the driver boundary, UDP sends through the whole stack,
 * and the software checksum's share of a send -- the number that gates
 * a driver's transmit offload.
 */
#define NICBENCH_ARP     2000u
#define NICBENCH_WINDOW  64u      /* requests in flight: the receive queue is short, and an open loop overruns it */
#define NICBENCH_UDP     10000u   /* checksum samples (the UDP phase itself is a window: NICBENCH_UDP_WINDOW_NS) */
#define NICBENCH_UDP_LEN 1024u
#define NICBENCH_PORT    33434u   /* nobody listens on the host; the send is the measurement */

struct arp_frame {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];
    uint8_t spa[4];
    uint8_t tha[ETH_ALEN];
    uint8_t tpa[4];
} __packed;

struct nicbench_hook {
    struct netif *nif;
    uint32_t gateway;   /* the reply we asked for is about this address */
    uint32_t sent;      /* requests out so far: replies are never counted past it */
    uint32_t replies;   /* atomic: the worker of whichever CPU the flow hashes to */
};

/* At the driver boundary, before any protocol layer: a reply to *us*
 * about the *gateway*, from the interface under test, is counted and
 * taken. Anything else -- another interface, an unsolicited reply, a
 * reply about some other host -- goes on to the ARP layer untouched. */
static bool nicbench_rx_hook(struct netif *nif, struct mbuf *m, void *arg)
{
    struct nicbench_hook *h = arg;
    if (nif != h->nif || m->pkt.len < ETH_HLEN + sizeof(struct arp_frame))
        return true;
    uint8_t hdr[ETH_HLEN + sizeof(struct arp_frame)];
    if (!m_copydata(m, 0, sizeof(hdr), hdr))
        return true;
    uint16_t type = (uint16_t)((hdr[12] << 8) | hdr[13]);
    const struct arp_frame *a = (const struct arp_frame *)(hdr + ETH_HLEN);
    if (type != ETH_P_ARP || a->op != htons(2))
        return true;
    if (memcmp(a->spa, &h->gateway, 4) != 0 || memcmp(a->tha, nif->mac, ETH_ALEN) != 0)
        return true;   /* not the reply this benchmark asked for */
    /* Never past what was sent: a duplicate cannot make the window
     * arithmetic go negative or the count exceed the requests. */
    uint32_t seen = __atomic_load_n(&h->replies, __ATOMIC_RELAXED);
    if (seen >= __atomic_load_n(&h->sent, __ATOMIC_ACQUIRE))
        return true;
    __atomic_fetch_add(&h->replies, 1u, __ATOMIC_RELAXED);
    m_freem(m);
    return false;
}

/* Receive-queue drops across every CPU's worker: where an open-loop
 * sender's replies go when they arrive faster than they are taken. */
static uint64_t rxq_drops_total(void)
{
    uint64_t total = 0;
    for (unsigned cpu = 0; cpu < cpu_count(); cpu++) {
        struct net_cpu_stats st;
        if (netif_cpu_stats(cpu, &st))
            total += st.rx_dropped;
    }
    return total;
}

/*
 * Closed loop: at most NICBENCH_WINDOW requests outstanding. The first
 * version sent all 2000 at once; the driver received every reply and
 * the receive queue kept 512 of them, which measured the queue's depth
 * and nothing about the NIC. A round trip is only a round trip if the
 * reply is waited for.
 */
static bool nicbench_arp(const char **reason, struct netif *nif, unsigned *rt_per_s, uint64_t *ns_per_rt)
{
    static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    /* One context per round, on this frame: netif_set_rx_hook(NULL)
     * returns only after a grace period, so no worker is still inside
     * the hook when the round ends -- a reply from this interface's
     * round cannot be counted into the next interface's. */
    static struct nicbench_hook ctx;   /* the hook's: static, so a return before its removal leaves nothing dangling */
    memset(&ctx, 0, sizeof(ctx));
    ctx.nif = nif;
    ctx.gateway = nif->ip4.gateway;
    struct nicbench_hook *h = &ctx;
    uint64_t rx0 = nif->stats.rx_packets, drops0 = rxq_drops_total();
    nt_netif_set_rx_hook(nicbench_rx_hook, h);
    uint64_t t0 = clock_now_ns();
    unsigned sent = 0;
    for (unsigned i = 0; i < NICBENCH_ARP; i++) {
        /* Wait for the window to open; give up on this round if it never does. */
        uint64_t wait_until = clock_now_ns() + 200ull * 1000000ull;
        unsigned spins = 0;
        while (sent - __atomic_load_n(&h->replies, __ATOMIC_RELAXED) >= NICBENCH_WINDOW) {
            if (clock_now_ns() > wait_until)
                goto stop;
            if (++spins < 64)
                sched_yield();
            else
                thread_sleep_ms(1);
        }
        struct mbuf *m = m_getcl();
        if (m == NULL)
            break;
        m->data = m->buf + 64;   /* headroom for the Ethernet header */
        struct arp_frame *a = (struct arp_frame *)m->data;
        a->htype = htons(1);
        a->ptype = htons(ETH_P_IP);
        a->hlen = ETH_ALEN;
        a->plen = 4;
        a->op = htons(1);
        memcpy(a->sha, nif->mac, ETH_ALEN);
        memcpy(a->spa, &nif->ip4.addr, 4);
        memset(a->tha, 0, ETH_ALEN);
        memcpy(a->tpa, &nif->ip4.gateway, 4);
        m->len = m->pkt.len = sizeof(*a);
        if (ether_output(nif, m, bcast, ETH_P_ARP) != 0)
            break;
        sent++;
        __atomic_store_n(&h->sent, sent, __ATOMIC_RELEASE);
    }
stop:;
    /* The clock stops when the replies have caught up, or when it is
     * clear they are not going to. */
    uint64_t deadline = clock_now_ns() + 500ull * 1000000ull;
    while (__atomic_load_n(&h->replies, __ATOMIC_RELAXED) < sent && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    uint64_t dt = clock_since_ns(t0);
    nt_netif_set_rx_hook(NULL, NULL);
    unsigned got = __atomic_load_n(&h->replies, __ATOMIC_RELAXED);
    kinfo("selftest: net-nicbench: %s: %u ARP requests sent, %u replies counted at the boundary, %llu frames received by the driver, %llu dropped at the receive queue",
          nif->name, sent, got, (unsigned long long)(nif->stats.rx_packets - rx0),
          (unsigned long long)(rxq_drops_total() - drops0));
    CHECK(sent > 0);
#if CONFIG_SCHED_CHAOS
    /* A benchmark's rate is not a claim the migrator must keep: a sender
     * moved behind the receive worker its own traffic keeps busy sends a
     * fraction of its window on a slow host (CI's chaos boot: 64 sent,
     * none back in time, 505 dropped at that worker's queue). Reported,
     * not asserted, in this build; the plain boot asserts it. */
    if (got * 2 < sent)
        kinfo("selftest: net-nicbench: %s: fewer than half the replies back in the window under the chaos migrator (%u of %u): reported, not asserted",
              nif->name, got, sent);
#else
    CHECK(got * 2 >= sent);   /* fewer than half back is a broken path, not a slow one */
#endif
    *rt_per_s = dt ? (unsigned)(((uint64_t)got * 1000000000ull) / dt) : 0;
    *ns_per_rt = got ? dt / got : 0;
    return true;
}

/*
 * The UDP phase is a window, not a count. It used to send 10,000
 * datagrams and time them; its duration was then 10,000 times a cost
 * the guest does not set -- the backend's per-datagram work on the host
 * (QEMU's user-mode network sends each one through an unconnected host
 * socket; see docs/testing/flakes.md, "`net-nicbench`'s UDP rate") -- and
 * a slow host put the whole test over its budget while every number it
 * printed was correct. A fixed window measures the same rate and bounds
 * the time. The histogram and the split below are the diagnosis a slow
 * reading now carries with it.
 */
#define NICBENCH_UDP_WINDOW_NS (500ull * 1000000ull)
#define NICBENCH_HIST 12u            /* log2 buckets: under 16 us, [16, 32) ... [8, 16) ms, and 16 ms and up */

struct nicbench_udp_stats {
    unsigned attempts, accepted, refused, failed;   /* refused: the driver's ring was full (-ENOBUFS) */
    uint64_t frames;                                 /* the driver's own count of frames out */
    uint64_t dt_ns;
    uint32_t hist[NICBENCH_HIST];
    uint64_t max_ns;
    uint64_t slow_n, slow_ns;                        /* sends of 1 ms or more, and the time they held */
    uint64_t driver_ns, driver_max_ns, driver_calls; /* inside the driver's transmit (netif_tx_probe) */
    unsigned ring_max, ring_cap;                     /* transmit descriptors in flight, sampled after each send */
    uint64_t switches;                               /* times the sender was switched out and back */
    unsigned cpu_moves;
    bool gw_before, gw_after;                        /* the gateway's ARP entry reachable */
    uint64_t gw_wait_ns;                             /* how long the warm-up waited for it */
    struct arp_stats arp0, arp1;
};

static unsigned nicbench_bucket(uint64_t ns)
{
    uint64_t us = ns / 1000;
    if (us < 16)
        return 0;
    unsigned b = 63u - (unsigned)__builtin_clzll(us) - 3u;   /* [16, 32) us is bucket 1 */
    return b < NICBENCH_HIST ? b : NICBENCH_HIST - 1;
}

static bool nicbench_udp(const char **reason, struct netif *nif, struct nicbench_udp_stats *st)
{
    struct socket *tx;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &tx) == 0);
    struct netaddr to = v4addr(nif->ip4.gateway, NICBENCH_PORT);
    static uint8_t payload[NICBENCH_UDP_LEN];
    memset(st, 0, sizeof(*st));
    /* The ARP counters from before the warm-up, so the report can say
     * whether resolving the gateway sent a request at all (+0 with the
     * entry still incomplete means an entry already there, on which
     * arp_resolve sends nothing). */
    arp_get_stats(&st->arp0);
    /* Warm up: the first send resolves the gateway and parks behind it.
     * Then wait for the entry: a window measured against an incomplete
     * entry sends nothing (every datagram parks and evicts the one before)
     * and the rate it would report is of frames that never left -- 5 of
     * 325 x86-64 second-interface rounds in the two weeks before this
     * wait existed read `0 of 10000 frames left the driver` under a
     * normal-looking rate (docs/testing/flakes.md, "`net-nicbench`'s
     * second interface reported a rate for frames that never left"). The
     * bound covers one ARP retry (ARP_RETRY_NS, 1 s); past it the test
     * fails (N25 made the entry this interface's own). */
    for (unsigned i = 0; i < 8; i++)
        (void)ksock_sendto(tx, payload, sizeof(payload), &to);
    uint8_t mac[ETH_ALEN];
    uint64_t t_gw = clock_now_ns();
    while (!arp_lookup(nif, nif->ip4.gateway, mac) && clock_since_ns(t_gw) < 1500ull * 1000000ull)
        thread_sleep_ms(1);
    st->gw_wait_ns = clock_since_ns(t_gw);
    st->gw_before = arp_lookup(nif, nif->ip4.gateway, mac);
    if (!st->gw_before) {
        /* A failure, since N25: the entry is this interface's own, it was
         * resolved before the ARP phase (nicbench_resolve_gateway), the
         * phase just proved 2,000 round trips on this link, and the wait
         * covers a retry -- so an entry still incomplete here is a lost
         * reply twice over or a defect, not another interface's state.
         * The line says which way the request count points. */
        struct arp_stats now;
        arp_get_stats(&now);
        kerror("selftest: net-nicbench: %s: the gateway's ARP entry is still incomplete after %llu ms "
               "(+%llu requests sent since the warm-up began, %llu pending dropped)",
               nif->name, (unsigned long long)(st->gw_wait_ns / 1000000),
               (unsigned long long)(now.requests_sent - st->arp0.requests_sent),
               (unsigned long long)(now.pending_dropped - st->arp0.pending_dropped));
        nt_ksock_put(tx);
        *reason = "the gateway's ARP entry did not resolve within 1.5 s on this interface";
        return false;
    }
    thread_sleep_ms(20);
    struct thread *self = thread_current();
    uint64_t switches0 = self->switches;
    int cpu = (int)raw_cpu_id();   /* a statistic: where the sender was, to count its moves */
    uint64_t tx0 = nif->stats.tx_packets;
    netif_tx_probe_set(nif);
    struct netif_tx_probe pr0, pr1;
    netif_tx_probe_read(&pr0);
    uint64_t t0 = clock_now_ns(), t_end = t0 + NICBENCH_UDP_WINDOW_NS;
    for (;;) {
        uint64_t a = clock_now_ns();
        if (a >= t_end)
            break;
        int64_t rc = ksock_sendto(tx, payload, sizeof(payload), &to);
        uint64_t dt = clock_since_ns(a);
        netif_tx_probe_read(&pr1);
        st->attempts++;
        if (rc == (int64_t)sizeof(payload))
            st->accepted++;
        else if (rc == -ENOBUFS)
            st->refused++;
        else
            st->failed++;
        st->hist[nicbench_bucket(dt)]++;
        if (dt > st->max_ns)
            st->max_ns = dt;
        if (dt >= 1000000ull) {
            st->slow_n++;
            st->slow_ns += dt;
        }
        uint64_t d = pr1.ns_total - pr0.ns_total;
        if (d > st->driver_max_ns)
            st->driver_max_ns = d;
        pr0 = pr1;
        unsigned pending, cap;
        if (netif_tx_pending(nif, &pending, &cap)) {
            st->ring_cap = cap;
            if (pending > st->ring_max)
                st->ring_max = pending;
        }
        int now_cpu = (int)raw_cpu_id();   /* a statistic */
        if (now_cpu != cpu) {
            st->cpu_moves++;
            cpu = now_cpu;
        }
        if ((st->attempts & 63) == 0)
            sched_yield();
    }
    st->dt_ns = clock_since_ns(t0);
    netif_tx_probe_read(&pr1);
    netif_tx_probe_set(NULL);
    st->driver_ns = pr1.ns_total;
    st->driver_calls = pr1.calls;
    st->switches = self->switches - switches0;
    thread_sleep_ms(20);   /* the driver's completions and counters settle */
    st->frames = nif->stats.tx_packets - tx0;
    st->gw_after = arp_lookup(nif, nif->ip4.gateway, mac);
    arp_get_stats(&st->arp1);
    nt_ksock_put(tx);
    CHECK(st->attempts > 0);
    return true;
}

/* Four lines, each under the log's 256-byte line. */
static void nicbench_udp_report(const struct netif *nif, const struct nicbench_udp_stats *st)
{
    kinfo("selftest: net-nicbench: %s: udp window %llu ms: %u sends attempted, %u accepted, %u refused by the driver (ring full), "
          "%u failed otherwise, %llu frames left the driver",
          nif->name, (unsigned long long)(st->dt_ns / 1000000), st->attempts, st->accepted, st->refused, st->failed,
          (unsigned long long)st->frames);
    kinfo("selftest: net-nicbench: %s: udp ring occupancy max %u of %u; sender switched out %llu times, moved CPU %u times; "
          "gateway arp %s (after %llu ms) -> %s, +%llu requests, +%llu pending dropped",
          nif->name, st->ring_max, st->ring_cap, (unsigned long long)st->switches, st->cpu_moves,
          st->gw_before ? "reachable" : "incomplete", (unsigned long long)(st->gw_wait_ns / 1000000),
          st->gw_after ? "reachable" : "incomplete",
          (unsigned long long)(st->arp1.requests_sent - st->arp0.requests_sent),
          (unsigned long long)(st->arp1.pending_dropped - st->arp0.pending_dropped));
    char hist[160];
    size_t n = 0;
    for (unsigned b = 0; b < NICBENCH_HIST && n < sizeof(hist); b++) {
        unsigned lo = 8u << b;   /* bucket b covers [lo, 2 lo) us; bucket 0 everything under 16 */
        if (b + 1 == NICBENCH_HIST)
            n += (size_t)ksnprintf(hist + n, sizeof(hist) - n, " >=%u:%u", lo, st->hist[b]);
        else
            n += (size_t)ksnprintf(hist + n, sizeof(hist) - n, " <%u:%u", lo * 2, st->hist[b]);
    }
    kinfo("selftest: net-nicbench: %s: udp us per send:%s; max %llu", nif->name, hist,
          (unsigned long long)(st->max_ns / 1000));
    unsigned driver_pct = st->dt_ns ? (unsigned)((st->driver_ns * 100) / st->dt_ns) : 0;
    kinfo("selftest: net-nicbench: %s: udp %llu sends of 1 ms or more held %llu of %llu ms; "
          "in the driver %u%% of the window over %llu transmits (max %llu us)",
          nif->name, (unsigned long long)st->slow_n, (unsigned long long)(st->slow_ns / 1000000),
          (unsigned long long)(st->dt_ns / 1000000), driver_pct, (unsigned long long)st->driver_calls,
          (unsigned long long)(st->driver_max_ns / 1000));
}

/* in_cksum over a datagram's worth, by itself: what a transmit checksum
 * offload could save per packet, and no more. */
static uint64_t nicbench_cksum_ns(void)
{
    static uint8_t buf[NICBENCH_UDP_LEN];
    volatile uint32_t sink = 0;
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < NICBENCH_UDP; i++) {
        buf[i & (NICBENCH_UDP_LEN - 1)] = (uint8_t)i;   /* defeat a hoisted result */
        sink += in_cksum(buf, sizeof(buf));
    }
    return (clock_since_ns(t0)) / NICBENCH_UDP;
}

/*
 * The gateway resolved on this interface before the ARP phase installs
 * its hook. The hook takes every reply from the gateway to us -- it cannot
 * tell its own from the stack's -- so a resolution the stack starts
 * during the phase (any off-link send: the default route is this
 * interface once the other is down) has its reply eaten and its entry
 * left incomplete until the next retry, which ARP_RETRY_NS spacing can
 * put past the UDP warm-up's bound. Seen 2026-10-09: a segment from a
 * connection net-hostinput left behind, on an eth1 the e1000e rebind had
 * left with no cached entry (docs/testing/flakes.md). With the entry
 * reachable first, the phase and the warm-up depend on neither. The bound
 * covers the first request and both retries (ARP_MAX_TRIES).
 */
static bool nicbench_resolve_gateway(const char **reason, struct netif *nif)
{
    uint8_t mac[ETH_ALEN];
    if (arp_lookup(nif, nif->ip4.gateway, mac))
        return true;
    struct socket *tx;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &tx) == 0);
    struct netaddr to = v4addr(nif->ip4.gateway, NICBENCH_PORT);
    static const uint8_t one[1] = { 0 };
    (void)ksock_sendto(tx, one, sizeof(one), &to);
    uint64_t t0 = clock_now_ns();
    while (!arp_lookup(nif, nif->ip4.gateway, mac) && clock_since_ns(t0) < 4500ull * 1000000ull)
        thread_sleep_ms(1);
    bool resolved = arp_lookup(nif, nif->ip4.gateway, mac);
    nt_ksock_put(tx);
    kinfo("selftest: net-nicbench: %s: gateway %s before the benchmark (%llu ms)", nif->name,
          resolved ? "resolved" : "NOT resolved", (unsigned long long)(clock_since_ns(t0) / 1000000));
    if (!resolved)
        *reason = "the gateway's ARP entry did not resolve within 4.5 s, three requests, on this interface";
    return resolved;
}

static bool nicbench_one(const char **reason, struct netif *nif, uint64_t cksum_ns)
{
    unsigned rt_s = 0;
    uint64_t ns_rt = 0;
    static struct nicbench_udp_stats st;   /* one at a time: the runner serialises tests */
    if (!nicbench_resolve_gateway(reason, nif))
        return false;
    if (!nicbench_arp(reason, nif, &rt_s, &ns_rt))
        return false;
    if (!nicbench_udp(reason, nif, &st))
        return false;
    nicbench_udp_report(nif, &st);
    unsigned accepted = st.accepted;
    uint64_t frames = st.frames;
    unsigned sends_s = st.dt_ns ? (unsigned)(((uint64_t)accepted * 1000000000ull) / st.dt_ns) : 0;
    uint64_t ns_send = accepted ? st.dt_ns / accepted : 0;
    /* Let this interface's echoes come home before the next interface's
     * round: the UDP phase sends thousands of datagrams whose replies are
     * still arriving for most of a second on a slow host, and the next
     * interface's ARP replies then queue behind them on the same receive
     * worker -- CI's guard-capable boot saw 64 requests sent, none
     * counted back in the window and 2,659 frames dropped at the queue.
     * Wait for this interface's receive count to hold still; the count is
     * the receive path's atomic (netif.c rx_common), read the same way. A
     * stream still moving after three seconds is not quiet, and the bench
     * says so rather than starting the next round over it. */
    {
        uint64_t last = __atomic_load_n(&nif->stats.rx_packets, __ATOMIC_RELAXED), quiet_ns = 0;
        uint64_t t_q = clock_deadline_ns(3000ull * 1000000ull);
        while (quiet_ns < 30ull * 1000000) {
            if (clock_deadline_passed(t_q)) {
                kinfo("selftest: net-nicbench: %s: receive stream still moving after 3 s (rx %llu)",
                      nif->name, (unsigned long long)last);
                *reason = "the receive stream did not go quiet between interfaces";
                return false;
            }
            thread_sleep_ms(5);
            uint64_t now = __atomic_load_n(&nif->stats.rx_packets, __ATOMIC_RELAXED);
            if (now != last) {
                last = now;
                quiet_ns = 0;
            } else {
                quiet_ns += 5000000;
            }
        }
    }
    unsigned share = ns_send ? (unsigned)((cksum_ns * 100) / ns_send) : 0;
    kinfo("selftest: net-nicbench: %s (caps 0x%x): arp %u rt/s (%llu ns per round trip); udp %u sends/s (%llu ns per send, "
          "%llu of %u frames left the driver); sw checksum of 1 KiB %llu ns = %u%% of a send",
          nif->name, nif->caps, rt_s, (unsigned long long)ns_rt, sends_s, (unsigned long long)ns_send,
          (unsigned long long)frames, accepted, (unsigned long long)cksum_ns, share);
    return true;
}

/* --- the socket's pending error (invariant N21), the receive hook's grace ----
 *
 * net-sockerr-udp, net-sockerr-spoof, net-sockerr-accept,
 * net-sockerr-locking and net-rxhook-grace are CeeSafe
 * (nettest_sockerr.cee). The values it writes as its own constants: */
_Static_assert(EAGAIN == 11 && ENETUNREACH == 101 && ECONNABORTED == 103 && ECONNREFUSED == 111 &&
               EHOSTUNREACH == 113 && COSMO_IO_ERROR == 8, "nettest_sockerr.cee's errno.h and syscall.h values");
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_DEST_UNREACH == 3 &&
               ICMP_UNREACH_PORT == 3 && ICMP_UNREACH_HOST == 1 && ETH_P_IP == 0x0800 && sizeof(struct ipv4_hdr) == 20,
               "nettest_sockerr.cee's ip.h and ether.h values");

/* --- an interface against the receive worker ------------------------------
 *
 * netif-remove-worker and netif-rxq-count are CeeSafe (nettest_worker.cee),
 * with fake interfaces of their own. Both need the debug build's worker
 * hooks; without them they pass silently, as the C did. The values the port
 * writes as its own constants:
 */
#if !CONFIG_DEBUG
bool selftest_netif_remove_worker(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_netif_rxq_count(const char **reason)
{
    (void)reason;
    return true;
}
#endif
_Static_assert(NETIF_UP == 1 && NETIF_NODEFAULT == 0x10 && ETH_HLEN == 14 && SCHED_PRIO_DEFAULT == 32 &&
                   NS_PER_SEC == 1000000000ull,
               "nettest_worker.cee's netif.h, ether.h, thread.h and time values");

bool selftest_net_nicbench(const char **reason)
{
    struct netif *first = netif_default();
    if (first == NULL) {
        kinfo("selftest: net-nicbench: no ethernet interface; skipping");
        return true;
    }
    uint64_t cksum_ns = nicbench_cksum_ns();
    bool ok = nicbench_one(reason, first, cksum_ns);
    struct netif *second = ok ? find_other_interface(first) : NULL;
    if (second != NULL) {
        /* The same numbers over the other driver, on the same host and
         * the same kind of backend: bring the default down so the stack
         * routes through the second, as net-second-nic does. */
        netif_set_up(first, false);   /* down flushes the first's neighbours (N25); the second resolves its own */
        ok = nicbench_one(reason, second, cksum_ns);
        netif_set_up(first, true);
        netif_put(second);
    }
    netif_put(first);
    return ok;
}

/* --- the tap tests' frames; tap, route, forwarding ----------------------
 *
 * selftest_tap, net-route and net-forward are CeeSafe (nettest_tap.cee),
 * with the frame builders every tap test uses (nettest_wrap,
 * nettest_mk_udp, nettest_mk_tcp, nettest_mk_icmp, nettest_l4cksum,
 * nettest_l4_ok, nettest_seed_arp, nettest_build_udp, nettest_recv_ip,
 * nettest_mk_dns; nettest.h), which the tests below still call. The
 * values it writes as its own constants: */
_Static_assert(ETH_HLEN == 14 && ICMP_TIME_EXCEEDED == 11 && ICMP_TIMXCEED_INTRANS == 0 && TAP_TXQ_MAX == 64 &&
               DHCP_DISCOVER == 1 && DHCP_OFFER == 2 && DHCP_REQUEST == 3 && DHCP_ACK == 5 && DHCP_NAK == 6 &&
               INADDR_BROADCAST_N == 0xffffffffu && NS_PER_SEC == 1000000000ull && sizeof(struct udp_hdr) == 8 &&
               sizeof(struct eth_hdr) == 14, "nettest_tap.cee's ether.h, ip.h, tap.h and dhcp.h values");

/* --- net-nat and net-second-nic: CeeSafe (nettest_nat.cee) -------------
 *
 * The values it writes as its own constants: */
_Static_assert(IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO_REPLY == 0 &&
                   ICMP_DEST_UNREACH == 3 && ICMP_UNREACH_PORT == 3 && ICMP_ECHO == 8 && TH_SYN == 0x02 &&
                   TH_ACK == 0x10 && ETH_HLEN == 14 && sizeof(struct udp_hdr) == 8 && sizeof(struct tcp_hdr) == 20 &&
                   sizeof(struct icmp_hdr) == 8 && sizeof(struct ipv4_hdr) == 20,
               "nettest_nat.cee's ip.h, tcp.h and ether.h values");
_Static_assert(NAT_PORT_MIN == 40000 && NAT_PORT_MAX == 49151 && NAT_TABLE_SIZE == 256 && NAT_QUOTA_PER_GUEST == 32 &&
                   NAT_TIMEOUT_UDP_NS == 30000000000ull && NETIF_LOOPBACK == 2 && EINPROGRESS == 115 &&
                   sizeof(struct nat_stats) <= 512 && sizeof(struct ip_stats) <= 512,
               "nettest_nat.cee's nat.h, netif.h and errno.h values, and its rooms");

/* --- tap-filter, net-dhcp, net-dns: CeeSafe (nettest_tap.cee) ------------ */

/* --- inbound port forwarding, many guests, the forwarding firewall ----------
 *
 * net-pf-clear, net-dnat, net-multiguest and net-firewall are CeeSafe
 * (nettest_fwd.cee), with the firewall tests' guest helpers
 * (nettest_fwt_send, nettest_fwt_recv, nettest_fwt_mk_icmp; nettest.h) the
 * chain tests below call. The values it writes as its own constants: */
_Static_assert(ENOENT == 2 && EEXIST == 17 && ENOSPC == 28 && ENOTSUP == 95 && NAT_TABLE_SIZE == 256 &&
               NAT_PF_MAX == 16 && NAT_QUOTA_PER_GUEST == 32 && NAT_TIMEOUT_TCP_NS == 30ull * 1000000000ull &&
               NAT_KIND_DNAT == 1, "nettest_fwd.cee's errno.h and nat.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_TO_GUEST == 2 && FW_DIR_TO_HOST == 3 &&
               FW_ICMP_TYPE_ANY == 0xffff && FW_MAX_GUESTS == 8 && FW_RULES_PER_GUEST == 32 &&
               NETIF_FORWARD == (1u << 5) && COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800,
               "nettest_fwd.cee's fw.h, netif.h and syscall.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 && COSMO_NETCTL_FILTER_DEL == 4 &&
               COSMO_NETCTL_FILTER_POLICY == 5 && COSMO_NETCTL_PROTO_ICMP == 1 && COSMO_NETCTL_PROTO_TCP == 6 &&
               COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_DIR_TO_GUEST == 2 && COSMO_NETCTL_VERDICT_DROP == 0 &&
               COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_SNAPSHOT_MAX <= 65536,
               "nettest_fwd.cee's netctl.h values, and its snapshot room");

/* The NAT and firewall state a test changed, cleared however it returns:
 * a forward or a translation left behind would decide the next test's
 * frames. */
static char g_nat_state_marker;

static void nt_rel_nat_state(void *arg)
{
    (void)arg;
    nat_pf_clear();
    nat_flush();
    fw_flush();
}

bool nettest_defer_nat_state(void) { return selftest_defer(nt_rel_nat_state, &g_nat_state_marker); }

void nettest_release_nat_state(void)
{
    if (!selftest_release(&g_nat_state_marker))
        nt_rel_nat_state(NULL);
}

int nettest_vfs_open(const char *path, unsigned flags, struct file **out)
{
    return nt_vfs_open(NULL, path, flags, 0, out);
}

void nettest_file_put(struct file *f) { nt_file_put(f); }

/* --- the runtime network control channel (/dev/net/tapctl) ----------------
 *
 * net-tapctl is CeeSafe (nettest_ctl.cee), with the walk of a snapshot's
 * sections (nettest_netctl_flows_off, nettest_netctl_snapshot_len;
 * nettest.h) the flow-listing tests below use. The values it writes as
 * its own constants: */
_Static_assert(EMSGSIZE == 90 && COSMO_NETCTL_FORWARD_ADD == 1 && COSMO_NETCTL_FORWARD_DEL == 2 &&
               COSMO_NETCTL_IF_UP == 0x1 && COSMO_NETCTL_IF_LOOPBACK == 0x2 && COSMO_NETCTL_FLOW_NAT == 1 &&
               COSMO_NETCTL_FLOW_DNAT == 2 && COSMO_NETCTL_FLOW_ESTABLISHED == 0x01 &&
               NAT_TIMEOUT_TCP_NS / 1000000u == 30000 && sizeof(((struct cosmo_netctl_if *)0)->name) == 8,
               "nettest_ctl.cee's errno.h, netctl.h and nat.h values");

/* --- the INPUT chain (docs/audit/next-subsystem-input-chain.md) ----------
 *
 * net-input is CeeSafe (nettest_input.cee), with the checksummed ICMP
 * messages the host chains test with (nettest_fwt_ones_sum,
 * nettest_fwt_mk_icmp_ck, nettest_fwt_mk_needfrag,
 * nettest_fwt_recv_echo_reply; nettest.h). The values it writes as its own
 * constants: */
_Static_assert(ICMP_DEST_UNREACH == 3 && FW_DIR_ANY == 0 && COSMO_NETCTL_DIR_ANY == 0 &&
               COSMO_NETCTL_DIR_TO_HOST == 3 && COSMO_NETCTL_ICMP_TYPE_ANY == 0xffff,
               "nettest_input.cee's ip.h, fw.h and netctl.h values");

/* A verdict is taken on the network worker after file_write returns, so a
 * counter is awaited, not read at once. True once `st.field` exceeds `base`. */
#define FWT_RISES(fn, st, field, base) ({                                   \
    bool r_ = false;                                                        \
    for (unsigned i_ = 0; i_ < 40 && !r_; i_++) {                           \
        fn(&(st));                                                          \
        if ((st).field > (base)) r_ = true; else thread_sleep_ms(10);       \
    }                                                                       \
    r_; })

/* --- the host chain (docs/audit/next-subsystem-host-input.md) ------------- */

/* How long a *positive* wait polls: the loop returns as soon as what it
 * waits for arrives, so being patient costs nothing when the stack is
 * working and keeps a loaded machine (CI's shared runners, the GIC-variant
 * boot) from failing an assertion that would have passed. Negative waits --
 * "nothing arrives" -- keep their own short budgets. */
#define HIN_TRIES 200u

/* The host's own outbound connection, on its own thread (connect blocks
 * until the peer's SYN+ACK is admitted). */
struct hin_conn {
    struct netaddr peer;
    struct socket *s;
    int rc;
    volatile bool done;
    struct thread *th;
    unsigned owner;   /* who puts `s`: HIN_RUNNING, then HIN_DONE (the test) or HIN_ABANDONED (the thread) */
};
enum { HIN_RUNNING, HIN_DONE, HIN_ABANDONED };

static void hin_connect_thread(void *arg)
{
    struct hin_conn *c = arg;
    c->rc = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c->s);
    if (c->rc == 0)
        c->rc = ksock_connect(c->s, &c->peer);
    c->done = true;
    /* Abandoned by a failed test while it connected: the socket is its to put. */
    if (__atomic_exchange_n(&c->owner, HIN_DONE, __ATOMIC_ACQ_REL) == HIN_ABANDONED) {
        /* Hand the socket to the runner, which puts it at its next census:
         * nothing the census reads changes on this thread. */
        struct socket *sock = c->s;
        c->s = NULL;
        arch_irq_state_t st = spin_lock_irqsave(&g_nt_reap_lock);
        bool finished = g_nt_finished;
        bool queued = !finished && g_nt_nreap < NT_REAP_MAX;
        if (queued)
            g_nt_reap[g_nt_nreap++] = sock;
        spin_unlock_irqrestore(&g_nt_reap_lock, st);
        if (finished && sock)
            ksock_put(sock);   /* after the last census: nothing counts it now, so it is put here */
        else if (!queued && sock)   /* more at once than the list holds: kept, and still excused, so the counts agree */
            kwarn("nettest: an abandoned connect's socket is kept: the reap list is full");
    }
    thread_exit(0);
}

/*
 * Its release, however the test returned (the net-leftover unit). A
 * connect waits only for the TCP state to leave SYN_SENT -- nothing but
 * the retransmit timeout ends it -- so the release gives the thread two
 * seconds, then either joins it and puts its socket, or lets it go and
 * leaves the socket to it. One exchange decides, so exactly one side puts.
 * The storage is static at every call site: the thread may outlive the
 * frame.
 */
static void hin_conn_release(void *arg)
{
    struct hin_conn *c = arg;
    for (unsigned i = 0; i < 200 && !c->done; i++)
        thread_sleep_ms(10);
    if (__atomic_exchange_n(&c->owner, HIN_ABANDONED, __ATOMIC_ACQ_REL) == HIN_DONE) {
        thread_join(c->th);
        if (c->s)
            ksock_put(c->s);
        c->s = NULL;
    } else {
        g_nt_abandoned++;    /* this runner thread's count: the census subtracts it */
        thread_put(c->th);   /* it hands its socket to the runner when its connect returns */
    }
    c->th = NULL;
}

static bool hin_conn_start(struct hin_conn *c, const char *name)
{
    c->owner = HIN_RUNNING;
    c->th = thread_create(hin_connect_thread, c, name, SCHED_PRIO_DEFAULT);
    if (c->th == NULL)
        return false;
    if (!selftest_defer(hin_conn_release, c)) {
        hin_conn_release(c);
        return false;
    }
    return true;
}

/* The success path, once the test has seen `done`: the thread is joined,
 * and its socket becomes an ordinary one of the test's, released by its
 * put or by the runner. */
static void hin_conn_join(struct hin_conn *c)
{
    selftest_forget(c);
    thread_join(c->th);
    c->th = NULL;
    if (c->s != NULL && !selftest_defer(nt_rel_sock, c->s)) {
        ksock_put(c->s);
        c->s = NULL;
    }
}

/*
 * net-hostinput is CeeSafe (nettest_hostin.cee), with the world side of the
 * uplink tap the host tests here drive a connection with (nettest_hin_*;
 * nettest.h). Its outbound connect stays here: the thread may outlive the
 * test, and its release is this file's. One at a time, in static storage.
 */
static struct hin_conn g_hin_cn;

bool nettest_hin_connect(uint32_t ip, uint16_t port, const char *name)
{
    g_hin_cn = (struct hin_conn){ .peer = v4addr(ip, port) };
    return hin_conn_start(&g_hin_cn, name);
}

bool nettest_hin_connect_done(int *rc)
{
    *rc = g_hin_cn.rc;
    return g_hin_cn.done;
}

struct socket *nettest_hin_connect_join(void)
{
    hin_conn_join(&g_hin_cn);
    return g_hin_cn.s;
}

/* The values nettest_hostin.cee writes as its own constants, and struct
 * hin_seg's payload where it writes it: */
_Static_assert(ENOENT == 2 && EAGAIN == 11 && EINVAL == 22 && ECONNRESET == 104 && ETH_HLEN == 14 &&
               IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO_REPLY == 0 &&
               ICMP_DEST_UNREACH == 3 && ICMP_ECHO == 8 && TH_FIN == 0x01 && TH_SYN == 0x02 && TH_RST == 0x04 &&
               TH_PSH == 0x08 && TH_ACK == 0x10 && TCP_CHALLENGE_PER_SEC == 100,
               "nettest_hostin.cee's errno.h, ip.h and tcp.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_ANY == 0 && FW_DIR_TO_HOST == 3 && FW_DIR_FROM_UPLINK == 4 &&
               FW_HOST_GUEST_IP == 0 && COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 &&
               COSMO_NETCTL_FILTER_DEL == 4 && COSMO_NETCTL_PROTO_TCP == 6 && COSMO_NETCTL_DIR_TO_HOST == 3 &&
               COSMO_NETCTL_DIR_FROM_UPLINK == 4 && COSMO_NETCTL_VERDICT_DROP == 0 &&
               COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_HOST_ADDR == 0 && COSMO_O_RDWR == 0x0002 &&
               COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2 && COSMO_SOCK_STREAM == 1 &&
               COSMO_SOCK_DGRAM == 2 && HIN_TRIES == 200,
               "nettest_hostin.cee's fw.h, netctl.h and socket values");
_Static_assert(__builtin_offsetof(struct hin_seg, pay) == 28 && sizeof(struct hin_seg) <= 128 &&
                   _Alignof(struct hin_seg) <= 8,
               "nettest_hostin.cee writes hin_seg's payload at 28, in a 128-byte room");

/* --- the host's own state (docs/audit/next-subsystem-host-state.md) ------- */

/*
 * net-zero-window-probe, net-tcp-sws, net-fin-acks-last-data and
 * net-hoststate are CeeSafe (nettest_hoststate.cee). A pcb's retransmit
 * count, which the network worker writes, read atomically as the C did: */
uint64_t nettest_tcp_retransmits(struct tcp_pcb *pcb)
{
    return __atomic_load_n(&pcb->retransmits, __ATOMIC_RELAXED);
}

/* The values nettest_hoststate.cee writes as its own constants (net-dns's
 * upstream answers with the 93.184.216.34 it checks): */
_Static_assert(EMSGSIZE == 90 && ICMP_UNREACH_PORT == 3 && ICMP_UNREACH_NEEDFRAG == 4 && FW_DIR_TO_GUEST == 2 &&
                   FW_FLOW_QUOTA_HOST == 64,
               "nettest_hoststate.cee's errno.h, ip.h and fw.h values");

/* --- the operator's flow listing (docs/audit/next-subsystem-net-flows.md) --- */

/*
 * net-flows-nat and net-flows-fw are CeeSafe (nettest_flows.cee), with the
 * read of the whole snapshot. The values it writes as its own constants, and
 * the room it reads the snapshot and the listings into:
 */
_Static_assert(IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && TH_SYN == 0x02 && TH_ACK == 0x10 && NAT_TABLE_SIZE == 256 &&
                   NAT_QUOTA_PER_GUEST == 32 && NAT_PORT_MIN == 40000 && NAT_PORT_MAX == 49151 &&
                   NAT_TIMEOUT_UDP_NS == 30000000000ull && NAT_KIND_MASQ == 0 && FW_ACCEPT == 1 &&
                   FW_DIR_TO_GUEST == 2 && FW_HOST_GUEST_IP == 0 && FW_FLOW_QUOTA_PER_GUEST == 32 &&
                   FW_FLOW_QUOTA_HOST == 64 && FW_FLOW_MAX == 320,
               "nettest_flows.cee's ip.h, tcp.h, nat.h and fw.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FLOW_NAT == 1 && COSMO_NETCTL_FLOW_FW == 2 &&
                   COSMO_NETCTL_FLOW_MASQ == 1 && COSMO_NETCTL_FLOW_GUEST == 3 && COSMO_NETCTL_FLOW_HOST == 4 &&
                   COSMO_NETCTL_FLOW_ESTABLISHED == 0x01 && COSMO_NETCTL_PROTO_TCP == 6 &&
                   COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_HOST_ADDR == 0 && COSMO_O_RDWR == 0x0002 &&
                   COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2 && HIN_TRIES == 200,
               "nettest_flows.cee's netctl.h and socket values");
_Static_assert(COSMO_NETCTL_SNAPSHOT_MAX <= 65536 && NAT_TABLE_SIZE * sizeof(struct nat_flow) <= 10240 &&
                   FW_FLOW_MAX * sizeof(struct fw_flow_info) <= 10240 &&
                   sizeof(struct cosmo_netctl_flow_list) <= 192 && sizeof(struct cosmo_netctl_flow) <= 192 &&
                   sizeof(struct nat_stats) <= 1024 && sizeof(struct fw_stats) <= 1024 && sizeof(struct fw_rule) <= 64,
               "nettest_flows.cee's rooms");

/* --- the OUTPUT chain (docs/audit/next-subsystem-output-chain.md) --------- */

/*
 * net-output and net-tcpverdict are CeeSafe (nettest_output.cee), with the
 * OUTPUT rule they build and the drain of a guest's tap file. The values it
 * writes as its own constants, and the rooms it builds and reads into:
 */
_Static_assert(EPERM == 1 && EAGAIN == 11 && EINVAL == 22 && EMSGSIZE == 90 && ENOTSUP == 95 && EINPROGRESS == 115 &&
                   ETH_HLEN == 14 && IPPROTO_ICMP == 1 && IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && ICMP_ECHO == 8 &&
                   TH_SYN == 0x02 && TH_PSH == 0x08 && TH_ACK == 0x10 && TCP_ESTABLISHED == 4,
               "nettest_output.cee's errno.h, ip.h and tcp.h values");
_Static_assert(FW_DROP == 0 && FW_ACCEPT == 1 && FW_DIR_TO_HOST == 3 && FW_DIR_FROM_UPLINK == 4 && FW_DIR_OUTPUT == 5 &&
                   FW_SCOPE_ANY == 0 && FW_SCOPE_WORLD == 1 && FW_SCOPE_GUEST == 2 && FW_HOST_GUEST_IP == 0,
               "nettest_output.cee's fw.h values");
_Static_assert(COSMO_NETCTL_VERSION == 8 && COSMO_NETCTL_FILTER_ADD == 3 && COSMO_NETCTL_FILTER_DEL == 4 &&
                   COSMO_NETCTL_PROTO_UDP == 17 && COSMO_NETCTL_DIR_OUTPUT == 5 && COSMO_NETCTL_VERDICT_DROP == 0 &&
                   COSMO_NETCTL_VERDICT_ACCEPT == 1 && COSMO_NETCTL_SCOPE_GUEST == 2 && COSMO_NETCTL_HOST_ADDR == 0 &&
                   COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && COSMO_AF_INET == 2 &&
                   COSMO_SOCK_STREAM == 1 && COSMO_SOCK_DGRAM == 2 && COSMO_IO_ERROR == 8 && HIN_TRIES == 200,
               "nettest_output.cee's netctl.h, socket and poll values");
_Static_assert(sizeof(struct cosmo_netctl_filter_guest) == 12, "the v5 policy record is 12 bytes");
_Static_assert(sizeof(struct fw_rule) <= 64 && sizeof(struct cosmo_netctl_filter) <= 64 &&
                   sizeof(struct cosmo_netctl_list) <= 64 && sizeof(struct cosmo_netctl_filter_list) <= 64 &&
                   sizeof(struct cosmo_netctl_filter_guest) <= 64 && sizeof(struct cosmo_netctl_filter_rule) <= 64 &&
                   sizeof(struct fw_stats) <= 1024 && sizeof(struct ip_stats) <= 1024 &&
                   sizeof(struct tcp_stats) <= 1024 && COSMO_NETCTL_SNAPSHOT_MAX <= 65536,
               "nettest_output.cee's rooms");

/* --- tap-ready and net-dhcp-client --------------------------------------- *
 *
 * CeeSafe (nettest_tapdev.cee): the tap's file waited on, and the kernel's
 * DHCP client against a fixture interface. The latter needs the client's
 * test seams; without them, the C's skip stands. The values the port writes
 * as its own constants, and the rooms it reads into:
 */
_Static_assert(COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && COSMO_IO_READABLE == 1 &&
                   COSMO_IO_WRITABLE == 2 && COSMO_SIGKILL == 9 && ETH_HLEN == 14 && ETH_P_IP == 0x0800 &&
                   ETH_P_ARP == 0x0806 && IPPROTO_UDP == 17 && INADDR_BROADCAST_N == 0xffffffffu &&
                   NETIF_UP == 1 && NETIF_NODEFAULT == 0x10,
               "nettest_tapdev.cee's syscall.h, ether.h, ip.h and netif.h values");
#if CONFIG_SELFTEST
_Static_assert(BOOTP_REQUEST == 1 && BOOTP_REPLY == 2 && DHCP_HTYPE_ETHER == 1 && DHCP_DISCOVER == 1 &&
                   DHCP_OFFER == 2 && DHCP_REQUEST == 3 && DHCP_ACK == 5 && DHCP_NAK == 6 && OPT_SUBNET == 1 &&
                   OPT_ROUTER == 3 && OPT_DNS == 6 && OPT_REQIP == 50 && OPT_LEASE == 51 && OPT_MSGTYPE == 53 &&
                   OPT_SERVERID == 54 && OPT_END == 255 && DHCP_SPORT == 67 && DHCP_CPORT == 68 && BOOTP_LEN == 236 &&
                   DHCP_FIXED == 240 && BOOTP_OFF_OP == 0 && BOOTP_OFF_HTYPE == 1 && BOOTP_OFF_HLEN == 2 &&
                   BOOTP_OFF_XID == 4 && BOOTP_OFF_CIADDR == 12 && BOOTP_OFF_YIADDR == 16 && BOOTP_OFF_CHADDR == 28 &&
                   DHCP_MAGIC == 0x63825363u && DHCP_MAX_CLIENTS == 4 && DHCP_S_SELECTING == 2 &&
                   DHCP_S_REQUESTING == 3 && DHCP_S_BOUND == 4 && DHCP_S_RENEWING == 5 && DHCP_S_REBINDING == 6 &&
                   DHCP_TEST_RETX == 0 && DHCP_TEST_T1 == 1 && DHCP_TEST_T2 == 2 && DHCP_TEST_EXPIRE == 3,
               "nettest_tapdev.cee's dhcp.h values");
_Static_assert(DHCP_MAX_CLIENTS * sizeof(struct dhcp_lease_info) <= 1024 && sizeof(struct dhcp_stats) <= 256,
               "nettest_tapdev.cee's rooms");
#else
bool selftest_net_dhcp_client(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-dhcp-client: needs the client's test seams (CONFIG_SELFTEST); skipping");
    return true;
}
#endif

/* --- three tests held at a seam ---------------------------------------------
 *
 * net-netif-flags, net-tap-release-order and net-syncookie-seeded are
 * CeeSafe (nettest_seam.cee). The first two need the debug build's park and
 * release seams, the third the self-test build's cookie seam; without them
 * the C's skips stand here. tap.h's after-purge seam is two C variables,
 * which CeeSafe cannot assign: nettest_tap_set_after_purge sets them. The
 * values the port writes as its own constants:
 */
#if CONFIG_DEBUG
void nettest_tap_set_after_purge(void (*fn)(void *), void *arg)
{
    if (fn == NULL) {
        tap_test_after_purge = NULL;
        tap_test_after_purge_arg = NULL;
    } else {
        tap_test_after_purge_arg = arg;
        tap_test_after_purge = fn;
    }
}
#else
bool selftest_net_netif_flags(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-netif-flags: needs the debug park hook; skipping");
    return true;
}

bool selftest_net_tap_release_order(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-tap-release-order: no test hooks in this build; skipping");
    return true;
}
#endif
#if !CONFIG_SELFTEST
bool selftest_net_syncookie_seeded(const char **reason)
{
    (void)reason;
    kinfo("selftest: net-syncookie-seeded: needs CONFIG_SELFTEST; skipping");
    return true;
}
#endif
_Static_assert(COSMO_O_RDWR == 0x0002 && COSMO_O_NONBLOCK == 0x0800 && IPPROTO_UDP == 17 && ETH_HLEN == 14 &&
                   sizeof(struct ipv4_hdr) == 20 && __builtin_offsetof(struct ipv4_hdr, src) == 12 &&
                   NAT_TABLE_SIZE == 256 && FW_FLOW_MAX == 320 && SCHED_PRIO_DEFAULT == 32,
               "nettest_seam.cee's syscall.h, ip.h, ether.h, nat.h, fw.h and thread.h values");
_Static_assert(sizeof(struct nat_stats) <= 512 && sizeof(struct ip_stats) <= 512 &&
                   NAT_TABLE_SIZE * sizeof(struct nat_flow) <= 8192 && FW_FLOW_MAX * sizeof(struct fw_flow_info) <= 12288,
               "nettest_seam.cee's rooms");
