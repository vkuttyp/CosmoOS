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

/* --- a fake interface --------------------------------------------------------
 *
 * One whose transmit counts and frees and whose release counts: the
 * receive-worker tests below register it, in debug builds only
 * (nettest_lifetime.cee has its own).
 */
#if CONFIG_DEBUG
struct fake_nif {
    struct netif nif;
    unsigned transmits;
    unsigned releases;
};

static int fake_nif_transmit(struct netif *nif, struct mbuf *m)
{
    struct fake_nif *f = nif->priv;
    f->transmits++;
    m_freem(m);
    return 0;
}

static void fake_nif_release(struct netif *nif)
{
    struct fake_nif *f = nif->priv;
    f->releases++;
}
#endif

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

/* --- accept against a racing peer ------------------------------------------------
 *
 * The audit's accept race: a child dequeued by tcp_accept had neither
 * listener nor socket until the caller attached one, so a reset in that
 * window freed the pcb under the accepting thread. tcp_accept now attaches
 * the owner under the TCP lock. The check below is the invariant (the pcb
 * names its socket when accept returns) plus a stress: clients that
 * connect and drop the connection at once while the server accepts.
 */
struct race_client {
    unsigned rounds;
    unsigned failures;
    struct netaddr server;
    volatile bool stop;      /* its release: finish the round and leave */
    struct thread *th;
};

static void race_client_release(void *arg)
{
    struct race_client *rc = arg;
    rc->stop = true;
    if (rc->th)
        thread_join(rc->th);
    rc->th = NULL;
}

static void race_client_main(void *arg)
{
    struct race_client *rc = arg;
    for (unsigned i = 0; i < rc->rounds && !rc->stop; i++) {
        struct socket *c;
        if (ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) != 0) {
            rc->failures++;
            continue;
        }
        if (ksock_connect(c, &rc->server) != 0)
            rc->failures++;
        else if (i & 1)
            ksock_shutdown(c, 2);   /* FIN before the server accepts */
        ksock_put(c);               /* close: FIN or, with unread data, RST */
    }
}

bool selftest_net_accept_race(const char **reason)
{
    struct socket *ls;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr any = v4addr(INADDR_LOOPBACK_N, 0);
    CHECK(ksock_bind(ls, &any) == 0 && ksock_listen(ls, 8) == 0);
    static struct race_client rc;   /* the client may outlive this frame */
    memset(&rc, 0, sizeof(rc));
    rc.rounds = 64;
    CHECK(ksock_getsockname(ls, &rc.server) == 0);

    rc.th = thread_create(race_client_main, &rc, "raceclient", SCHED_PRIO_DEFAULT);
    CHECK(rc.th != NULL);
    if (!selftest_defer(race_client_release, &rc)) {
        race_client_release(&rc);
        *reason = "its release list was full";
        return false;
    }
    unsigned accepted = 0;
    for (unsigned i = 0; i < rc.rounds; i++) {
        struct socket *c;
        struct netaddr peer;
        int rc2 = nt_ksock_accept(ls, &c, &peer);
        CHECK(rc2 == 0);
        CHECK(c->tcp != NULL && c->tcp->sock == c);   /* attached under the lock */
        CHECK(peer.family == COSMO_AF_INET && peer.port != 0);
        accepted++;
        nt_ksock_put(c);
    }
    selftest_release(&rc);   /* joined */
    CHECK(rc.failures == 0);
    nt_ksock_put(ls);
    kinfo("selftest: net-accept-race: %u connections accepted against a dropping peer", accepted);
    return true;
}

/* --- the census against a worker's wake reference ----------------------------------
 *
 * net-accept-race failed on CI (run 37679564267, x86-64, chaos migrator)
 * with every check passing: "left the network changed: ... sockets 0 ->
 * 1". A worker that wakes a socket holds a reference from under the pcb
 * lock until after the wake; a test that puts every socket of its while
 * one is held returns with the count one high, and the census blamed it.
 * The seam holds a worker there: the census must not return until it
 * lets go, and must then find nothing left. The census runs on a thread
 * of its own so the test can see whether it returned while the worker
 * held on -- without the fix it does, at once, with the count one high.
 */
#if CONFIG_DEBUG
struct census_job {
    struct nettest_census out;
    struct completion done;
};

static void census_job_main(void *arg)
{
    struct census_job *j = arg;
    nettest_census(&j->out);
    complete(&j->done);
}

static char g_wake_hold_marker;   /* the seam's release, by address */

static void wake_hold_release_rel(void *arg)
{
    (void)arg;
    (void)sock_test_wake_hold_release();
}

#endif /* CONFIG_DEBUG */

bool selftest_net_census_wake_ref(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: net-census-wake-ref: no test hooks in this build; skipping");
    return true;
#else
    static struct nettest_census before;
    nettest_census(&before);
    struct socket *ls, *c, *s;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr any = v4addr(INADDR_LOOPBACK_N, 0), srv, peer;
    CHECK(ksock_bind(ls, &any) == 0 && ksock_listen(ls, 1) == 0);
    CHECK(ksock_getsockname(ls, &srv) == 0);
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c) == 0);
    CHECK(ksock_connect(c, &srv) == 0);
    CHECK(nt_ksock_accept(ls, &s, &peer) == 0);

    /* Registered after the sockets, so run before them (LIFO): a failed
     * check never leaves a worker held while they are put. */
    CHECK(selftest_defer(wake_hold_release_rel, &g_wake_hold_marker));
    sock_test_wake_hold_arm(s);
    CHECK(ksock_sendto(c, "x", 1, NULL) == 1);   /* its input wakes s */
    CHECK(sock_test_wake_hold_wait(5ull * 1000 * 1000 * 1000));
    unsigned refs = socket_wake_refs();
    CHECK(refs >= 1);

    /* Every reference of the test's: s lives on in the worker's hand only. */
    nt_ksock_put(s);
    nt_ksock_put(c);
    nt_ksock_put(ls);

    /* No check from here to the release: the census thread may be waiting
     * on the held worker, and a return would put nothing that frees it. */
    static struct census_job job;
    memset(&job, 0, sizeof(job));
    completion_init(&job.done, "census-job");
    struct thread *t = thread_create(census_job_main, &job, "censusjob", SCHED_PRIO_DEFAULT);
    /* Long enough for a census that does not wait to have returned. */
    bool early = t != NULL && wait_for_completion_timeout(&job.done, 200ull * 1000 * 1000);
    bool hold_ok = sock_test_wake_hold_release();
    selftest_forget(&g_wake_hold_marker);
    if (t != NULL) {
        wait_for_completion(&job.done);
        thread_join(t);
    }
    CHECK(t != NULL);
    CHECK(hold_ok);    /* the worker was still held when it was let go */
    CHECK(!early);     /* the census waited for the worker's put */
    CHECK(nettest_census_equal(&before, &job.out));
    kinfo("selftest: net-census-wake-ref: a worker held %u wake reference(s) past the test's last put; "
          "the census waited for it and found sockets %u -> %u", refs, before.sockets, job.out.sockets);
    return true;
#endif
}

/* --- milestone 8: hardening -------------------------------------------------------- *
 *
 * net-tcp-syncache, net-tcp-rfc5961, net-tcp-reorder, net-tcp-keepalive,
 * net-icmp-limit, net-raw-icmp and net-nonblock are CeeSafe
 * (nettest_tcp.cee), with their injected segment, filters, holding server
 * and predicates; wait_until stays here for the tests still C, and
 * nettest_tcp.cee has its own under the same contract. The values it
 * writes as its own constants: */
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

/*
 * Wait until `pred(arg)` holds, or `budget_ms` passes
 * (docs/audit/next-subsystem-suite-waits.md).
 *
 * The `settle(N)` this replaced waited for *time*; this waits for the *property*,
 * which is the difference between a test that asserts something about the
 * stack and one that asserts something about the host. There is no
 * relationship between 100 ms and 300 packets, and on a machine that has
 * been building and booting for hours there never was.
 *
 * **The budget is generous on purpose** -- seconds where the sleep was
 * tens of milliseconds. A deadline that is never reached on a healthy
 * host costs nothing, because the wait returns as soon as the predicate
 * holds; when it *is* reached the caller's `CHECK` fails on the wait
 * rather than on an arithmetic mismatch that reads like a protocol bug.
 *
 * **A predicate may not require a coherent view of more than one
 * field -- unless every field it reads is monotonic and the predicate is
 * monotone in them.** `tcp_get_stats` is `*out = g_stats;` with no lock
 * and `ksock_ready` reads socket state outside the socket mutex, so a
 * predicate over two fields can see them from either side of an update.
 * The danger is a torn read that *satisfies* the predicate early. With
 * monotonic fields and a monotone predicate a torn read can only
 * under-count, which delays termination and never causes it, so such a
 * predicate is safe (`syn_answered` is the one instance here, and says
 * so). Anything else waits on a single field, and the multi-field
 * *assertion* stays after the wait, where it is the test's claim and not
 * the wait's termination condition. A first version of this contract was
 * the absolute rule alone, and the file broke it two functions later.
 *
 * **The result must be CHECKed** (`warn_unused_result`, and the kernel
 * builds with -Werror): a wait whose expiry is dropped is `settle` in a
 * different hat. **A wait that used more than half its budget is
 * reported**: a test that habitually takes 1.9 s of a 2 s budget is a
 * finding -- a slowdown, or a budget that a widened deadline is hiding --
 * and the line is what makes it visible before it becomes a failure.
 */
static bool wait_until(bool (*pred)(void *), void *arg, unsigned budget_ms)
    __attribute__((warn_unused_result));
static bool wait_until(bool (*pred)(void *), void *arg, unsigned budget_ms)
{
    uint64_t t0 = clock_now_ns();
    uint64_t deadline = t0 + (uint64_t)budget_ms * 1000000ull;
    while (!pred(arg)) {
        if (clock_now_ns() > deadline)
            return false;
        thread_sleep_ms(1);
        sched_watchdog_kick();
    }
    unsigned waited_ms = (unsigned)((clock_since_ns(t0)) / 1000000ull);
    if (waited_ms * 2 > budget_ms)
        kinfo("selftest: wait_until: waited %u ms of a %u ms budget", waited_ms, budget_ms);
    return true;
}

/* A socket's readiness word has every bit in `mask` set. This is the
 * shape six hand-written waits in this file shared -- a header loop on
 * `ksock_ready` with `i < 100` standing in for a second. */
struct ready_target { struct socket *s; unsigned mask; };
static bool ready_has(void *arg)
{
    const struct ready_target *t = arg;
    return (ksock_ready(t->s) & t->mask) == t->mask;
}

/* --- the listen backlog, in either accept order ----------------------------------
 *
 * A listener with backlog 2 that has not accepted still completes the
 * handshake for two clients; the test accepts one, exchanges `hi` with it,
 * and closing the listener resets the other.
 *
 * **Which one it accepts is not the client that connected first.** A
 * connection joins the accept queue when the listener processes that
 * client's final ACK, on the network worker the ACK's flow hashes to --
 * after the client's connect() has returned. The two clients' ACKs are
 * usually on different workers, so if the first's worker runs late the
 * second is queued, and accepted, first. This step used to receive `hi`
 * on `c1` regardless, and in that order it waited for data nobody would
 * send: a hang with every CPU idle
 * (docs/audit/next-subsystem-accept-order.md). It now asks which client
 * it accepted, and bounds the receive, so a wrong answer fails in seconds.
 *
 * `force` makes the second client first, deterministically: the first's
 * final ACK is held in tcp_input until the second is queued, then
 * delivered. `net-lo-tcp` runs this unforced and `net-accept-order`
 * forced -- one step, both orders.
 */
#if CONFIG_DEBUG
struct backlog_forced { struct socket *ls; };
static bool backlog_second_queued(void *arg)
{
    /* Two fields, both monotone until the test acts: the ACK stays held
     * until it is delivered below, and nothing accepts meanwhile, so the
     * queue only grows (wait_until's contract). */
    const struct backlog_forced *f = arg;
    return tcp_test_ack_held() && (ksock_ready(f->ls) & COSMO_IO_READABLE);
}

static void backlog_release_hold(void *arg)
{
    (void)arg;
    tcp_test_deliver_held_ack();   /* disarms, and a held ACK goes back in: nothing is left half-open */
}
#endif

static bool lo_tcp_backlog(const char **reason, uint16_t port, bool force, bool *accepted_first)
{
    const char *who = force ? "net-accept-order" : "net-lo-tcp";
    struct socket *ls;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &ls) == 0);
    struct netaddr la = v4addr(INADDR_LOOPBACK_N, port);
    CHECK(ksock_bind(ls, &la) == 0 && ksock_listen(ls, 2) == 0);
    CHECK(ksock_listen(ls, 2) == -EINVAL);
    if (force) {
#if CONFIG_DEBUG
        tcp_test_hold_ack(port, (uint16_t)(port + 1));   /* c1 is bound to port + 1 below */
        if (!selftest_defer(backlog_release_hold, NULL)) {
            backlog_release_hold(NULL);
            *reason = "its release list was full";
            return false;
        }
#else
        *reason = "forcing the order needs the debug build's hold";
        return false;
#endif
    }
    struct socket *c1, *c2;
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c1) == 0);
    if (force) {   /* a known port, so the hold takes c1's ACK and no other */
        struct netaddr ca = v4addr(INADDR_LOOPBACK_N, (uint16_t)(port + 1));
        CHECK(ksock_bind(c1, &ca) == 0);
    }
    CHECK(ksock_connect(c1, &la) == 0);
    CHECK(nt_ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &c2) == 0 && ksock_connect(c2, &la) == 0);
#if CONFIG_DEBUG
    if (force) {
        struct backlog_forced f = { .ls = ls };
        bool second_first = wait_until(backlog_second_queued, &f, 2000);
        bool delivered = tcp_test_deliver_held_ack();   /* either way: a failed wait must not strand c1 */
        if (!second_first || !delivered) {
            kerror("selftest: %s: the first client's ACK %s, the second %s", who, delivered ? "was held" : "was not held",
                   second_first ? "was queued" : "was not queued in time");
            *reason = "the forced order did not happen";
            return false;
        }
    }
#endif
    struct socket *a1;
    struct netaddr peer, n1, n2;
    CHECK(nt_ksock_accept(ls, &a1, &peer) == 0);
    CHECK(ksock_getsockname(c1, &n1) == 0 && ksock_getsockname(c2, &n2) == 0);
    CHECK(n2.port >= NET_EPHEMERAL_LO && (force ? n1.port == port + 1 : n1.port >= NET_EPHEMERAL_LO));
    CHECK(peer.port == n1.port || peer.port == n2.port);
    *accepted_first = peer.port == n1.port;
    struct socket *mine = *accepted_first ? c1 : c2, *other = *accepted_first ? c2 : c1;
    kinfo("selftest: %s: accepted the %s client", who, *accepted_first ? "first" : "second");
    CHECK(ksock_sendto(a1, "hi", 2, NULL) == 2);
    /* Bounded: a step that picked the wrong client fails here, not at the
     * harness's timeout. */
    struct ready_target rt = { .s = mine, .mask = COSMO_IO_READABLE };
    CHECK(wait_until(ready_has, &rt, 2000));
    char b[4];
    CHECK(ksock_recvfrom(mine, b, 4, NULL) == 2 && memcmp(b, "hi", 2) == 0);
    /* Closing the listener resets the still-queued connection. */
    nt_ksock_put(ls);
    thread_sleep_ms(20);
    CHECK(ksock_recvfrom(other, b, 4, NULL) < 0 || ksock_sendto(other, "x", 1, NULL) < 0);
    nt_ksock_put(a1);
    nt_ksock_put(c1);
    nt_ksock_put(c2);
    return true;
}

/* net-lo-tcp's backlog (nettest_lo.cee). */
bool nettest_lo_tcp_backlog(const char **reason, uint16_t port, bool force, bool *accepted_first)
{
    return lo_tcp_backlog(reason, port, force, accepted_first);
}

/* The backlog step in the order that hung it: the second client's
 * handshake completes first, and the accept follows completion, not
 * connect(). Ports 6004 (the listener) and 6005 (the first client). */
bool selftest_net_accept_order(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: net-accept-order: no test hooks in this build; skipping");
    return true;
#else
    unsigned socks0 = socket_count();
    bool accepted_first;
    if (!lo_tcp_backlog(reason, 6004, true, &accepted_first))
        return false;
    CHECK(!accepted_first);   /* completion order: the held client is accepted second */
    thread_sleep_ms(50);
    CHECK(socket_count() == socks0);
    return true;
#endif
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

/* --- a second interface (docs/drivers/e1000e/testing.md) -----------------------
 *
 * The half of the two-interface question a driver can be checked on:
 * when the default interface goes down, the other becomes the default
 * and carries traffic through its own rings. Written against no driver
 * in particular -- it finds whatever second non-loopback interface the
 * machine has -- because that is the claim being tested.
 */
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

static bool second_nic_body(const char **reason, struct netif *second)
{
    /* The other takes over as soon as the first is down. */
    struct netif *now = netif_default();
    CHECK(now == second);
    netif_put(now);

    /* The table is keyed by (interface, address) since N25, so the first
     * interface's entry for the shared gateway address cannot satisfy
     * this; only the second's own could, if an earlier test resolved it.
     * Remove that one, so a frame must cross the second interface. */
    uint8_t mac[ETH_ALEN];
    uint32_t gw = second->ip4.gateway;
    CHECK(gw != 0);
    arp_delete(second, gw);
    CHECK(!arp_lookup(second, gw, mac));

    uint64_t rx0 = second->stats.rx_packets, tx0 = second->stats.tx_packets;
    struct mbuf *m = m_getcl();
    CHECK(m != NULL);
    m->len = m->pkt.len = 20;
    int rc = arp_resolve(second, gw, mac, m);   /* the request leaves through `second` */
    CHECK(rc == -EINPROGRESS || rc == 0);
    bool resolved = rc == 0;
    for (unsigned waited = 0; waited < 2000 && !resolved; waited += 10) {
        thread_sleep_ms(10);
        resolved = arp_lookup(second, gw, mac);
    }
    CHECK(resolved);   /* the reply came back: a frame out and a frame in through its rings */
    CHECK(second->stats.tx_packets > tx0);
    CHECK(second->stats.rx_packets > rx0);
    return true;
}

bool selftest_net_second_nic(const char **reason)
{
    struct netif *first = netif_default();
    if (first == NULL) {
        kinfo("selftest: net-second-nic: no ethernet interface; skipping");
        return true;
    }
    struct netif *second = find_other_interface(first);
    if (second == NULL) {
        netif_put(first);
        kinfo("selftest: net-second-nic: one interface; skipping");
        return true;
    }
    netif_set_up(first, false);
    bool ok = second_nic_body(reason, second);
    /* Whatever happened, the machine leaves with its default interface
     * back: a later test that finds it down would fail for this test's
     * reason and say nothing about its own. */
    netif_set_up(first, true);
    struct netif *again = netif_default();
    if (ok) {
        ok = again == first;
        if (!ok)
            *reason = "the first interface did not become the default again";
    }
    if (again)
        netif_put(again);
    if (ok)
        kinfo("selftest: net-second-nic: %s took over from %s and resolved the gateway through its own rings",
              second->name, first->name);
    netif_put(second);
    netif_put(first);
    return ok;
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

/* Park a worker after its quiesce read section, then remove its interface.
 * Reaching the explicit barrier seam proves unregister has passed the
 * grace-period wait and queue purge; it must still wait for this input_one. */
#if CONFIG_DEBUG
struct netif_remove_worker_state {
    struct fake_nif *fake;
    struct completion input, release, at_barrier, removed;
};

static void netif_remove_worker_input(struct netif *nif, void *arg)
{
    struct netif_remove_worker_state *st = arg;
    if (nif != &st->fake->nif)
        return;
    complete(&st->input);
    wait_for_completion(&st->release);
}

static void netif_remove_worker_barrier(struct netif *nif, void *arg)
{
    struct netif_remove_worker_state *st = arg;
    if (nif == &st->fake->nif)
        complete(&st->at_barrier);
}

static void netif_remove_worker_main(void *arg)
{
    struct netif_remove_worker_state *st = arg;
    netif_unregister(&st->fake->nif);
    complete(&st->removed);
}
#endif

bool selftest_netif_remove_worker(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    return true;
#else
    static struct fake_nif f;
    static const struct netif_ops ops = { .transmit = fake_nif_transmit, .release = fake_nif_release };
    static struct netif_remove_worker_state st;
    memset(&f, 0, sizeof(f));
    memset(&st, 0, sizeof(st));
    st.fake = &f;
    completion_init(&st.input, "netif-remove-input");
    completion_init(&st.release, "netif-remove-release");
    completion_init(&st.at_barrier, "netif-remove-barrier");
    completion_init(&st.removed, "netif-remove-done");
    strlcpy(f.nif.name, "rmworker0", sizeof(f.nif.name));
    f.nif.mtu = 1500;
    f.nif.ops = &ops;
    f.nif.priv = &f;
    f.nif.flags = NETIF_UP | NETIF_NODEFAULT;
    f.nif.ip4.addr = htonl(0x0a4b0001);
    f.nif.ip4.mask = htonl(0xffffff00);

    bool registered = netif_register(&f.nif) == 0;
    if (!registered) {
        *reason = "netif-remove-worker: could not register the synthetic interface";
        return false;
    }
    netif_test_worker_hooks_set(netif_remove_worker_input, netif_remove_worker_barrier, &st);
    struct mbuf *m = m_getcl();
    if (m == NULL) {
        *reason = "netif-remove-worker: could not allocate the synthetic frame";
        goto cleanup;
    }
    m->data = m->buf + 64;
    m->len = m->pkt.len = ETH_HLEN;
    memset(m->data, 0, ETH_HLEN);   /* unknown EtherType: freed after the worker is released */
    netif_rx_on(&f.nif, m, 0);

    bool entered = wait_for_completion_timeout(&st.input, 2ull * 1000000000ull);
    struct thread *remover = entered ? thread_create(netif_remove_worker_main, &st, "netif-remove", SCHED_PRIO_DEFAULT)
                                     : NULL;
    bool at_barrier = remover != NULL && wait_for_completion_timeout(&st.at_barrier, 2ull * 1000000000ull);
    bool blocked = at_barrier && !completion_done(&st.removed);
    complete(&st.release);
    if (remover != NULL) {
        wait_for_completion(&st.removed);
        thread_join(remover);
    } else if (registered) {
        netif_unregister(&f.nif);
    }
    netif_test_worker_hooks_set(NULL, NULL, NULL);
    netif_put(&f.nif);
    bool ok = entered && at_barrier && blocked && f.releases == 1;
    kinfo("NETIF-REMOVE-WORKER: %s entered=%u barrier=%u waited=%u release=%u", ok ? "PASS" : "FAIL", entered,
          at_barrier, blocked, f.releases);
    if (!ok)
        *reason = "netif-remove-worker: unregister did not wait for active input_one; see the log";
    return ok;

cleanup:
    complete(&st.release);
    if (registered)
        netif_unregister(&f.nif);
    netif_test_worker_hooks_set(NULL, NULL, NULL);
    netif_put(&f.nif);
    return false;
#endif
}

/* netif_rxq_count counts one interface's packets. CPU 0's worker is parked
 * on the decoy's first packet, so its second stays queued there; the
 * other interface, which received nothing, must count 0 and the decoy 1.
 * A count of every queue (net-netif-lifetime's CI failure, the https
 * unit) gives the second interface 1 every time. */
#if CONFIG_DEBUG
static void netif_rxq_count_input(struct netif *nif, void *arg)
{
    struct netif_remove_worker_state *st = arg;
    if (nif != &st->fake->nif || completion_done(&st->input))
        return;
    complete(&st->input);
    wait_for_completion(&st->release);
}
#endif

bool selftest_netif_rxq_count(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    return true;
#else
    static struct fake_nif decoy, other;
    static const struct netif_ops ops = { .transmit = fake_nif_transmit, .release = fake_nif_release };
    static struct netif_remove_worker_state st;
    struct fake_nif *fakes[2] = { &decoy, &other };
    const char *names[2] = { "rxqcnt0", "rxqcnt1" };
    bool registered[2] = { false, false };
    bool ok = false;
    unsigned n_decoy = 0, n_other = 0;
    memset(&st, 0, sizeof(st));
    st.fake = &decoy;
    completion_init(&st.input, "rxq-count-input");
    completion_init(&st.release, "rxq-count-release");
    for (int i = 0; i < 2; i++) {
        memset(fakes[i], 0, sizeof(*fakes[i]));
        strlcpy(fakes[i]->nif.name, names[i], sizeof(fakes[i]->nif.name));
        fakes[i]->nif.mtu = 1500;
        fakes[i]->nif.ops = &ops;
        fakes[i]->nif.priv = fakes[i];
        fakes[i]->nif.flags = NETIF_UP | NETIF_NODEFAULT;
        registered[i] = netif_register(&fakes[i]->nif) == 0;
        if (!registered[i]) {
            *reason = "netif-rxq-count: could not register a synthetic interface";
            goto out;
        }
    }
    netif_test_worker_hooks_set(netif_rxq_count_input, NULL, &st);
    for (int i = 0; i < 2; i++) {
        struct mbuf *m = m_getcl();
        if (m == NULL) {
            *reason = "netif-rxq-count: could not allocate a synthetic frame";
            goto out;
        }
        m->data = m->buf + 64;
        m->len = m->pkt.len = ETH_HLEN;
        memset(m->data, 0, ETH_HLEN);   /* unknown EtherType: input frees it */
        netif_rx_on(&decoy.nif, m, 0);
        /* The first one parks CPU 0's worker; the second then waits behind it. */
        if (i == 0 && !wait_for_completion_timeout(&st.input, 2ull * 1000000000ull)) {
            *reason = "netif-rxq-count: CPU 0's worker did not take the first frame";
            goto out;
        }
    }
    n_decoy = netif_rxq_count(&decoy.nif);
    n_other = netif_rxq_count(&other.nif);
    ok = n_decoy == 1 && n_other == 0;
    if (!ok)
        *reason = "netif-rxq-count: a count includes another interface's packets; see the log";
out:
    complete(&st.release);
    for (int i = 0; i < 2; i++)
        if (registered[i])
            netif_unregister(&fakes[i]->nif);
    netif_test_worker_hooks_set(NULL, NULL, NULL);
    for (int i = 0; i < 2; i++)
        if (registered[i])
            netif_put(&fakes[i]->nif);
    kinfo("NETIF-RXQ-COUNT: %s decoy=%u other=%u (want 1 and 0)", ok ? "PASS" : "FAIL", n_decoy, n_other);
    return ok;
#endif
}

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

/* --- net-netif-flags: one reading of the flag word per packet (N26) ------
 *
 * ipv4_input reads the ingress interface's nif->flags once, and
 * ipv4_forward the egress's once, and every check of the packet -- the
 * anti-spoof, the forwarding firewall's direction, whether to masquerade
 * -- is made from those readings. Until 2026-10-07 each check read the
 * word again, so a toggle of masquerade landing between two of them gave
 * one packet half of each state: anti-spoofed as a plain forwarder (any
 * source on the subnet passes), then masqueraded (a source the masquerade
 * anti-spoof would have refused, translated). The park hook holds a packet
 * exactly there while the test flips the flag; a stream under a toggling
 * thread checks the same rule without the hook.
 */
#if CONFIG_DEBUG   /* the park hook exists in debug builds; the test skips without it */
struct flags_toggler {
    struct netif *nif;
    volatile bool stop;
    unsigned flips;
};

static void flags_toggler_thread(void *arg)
{
    struct flags_toggler *t = arg;
    bool on = false;
    while (!t->stop) {
        on = !on;
        netif_set_masquerade(t->nif, on);
        t->flips++;
        thread_sleep_ms(1);
    }
    netif_set_masquerade(t->nif, false);
    thread_exit(0);
}

/* Inject a datagram from `src` on `g`, parked in ipv4_forward between the
 * anti-spoof and the masquerade decision; flip masquerade to `masq` while
 * it is parked; release it. True when it was parked. */
static bool flags_inject_parked(struct tap *g, const uint8_t gmac[6], const uint8_t smac[6], uint32_t src,
                                uint32_t dst, bool masq)
{
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, frame[128];
    ipv4_test_hold_forward(true);
    uint32_t len = nettest_build_udp(frame, gmac, smac, src, dst, 64, 4100, 5100, payload, sizeof(payload));
    if (tap_inject(g, frame, len) != 0) {
        ipv4_test_hold_forward(false);
        return false;
    }
    bool parked = false;
    for (unsigned i = 0; i < 1000 && !parked; i++) {
        parked = ipv4_test_forward_parked();
        if (!parked)
            thread_sleep_ms(1);
    }
    netif_set_masquerade(tap_netif(g), masq);
    ipv4_test_release_forward();
    ipv4_test_hold_forward(false);
    return parked;
}

/* The source address of the next IPv4 frame on `u`, or 0 for none. */
static uint32_t flags_recv_src(struct tap *u)
{
    struct mbuf *out = nettest_recv_ip(u);
    if (out == NULL)
        return 0;
    uint8_t hdr[ETH_HLEN + sizeof(struct ipv4_hdr)];
    uint32_t src = 0;
    if (m_copydata(out, 0, sizeof(hdr), hdr))
        src = ((struct ipv4_hdr *)(hdr + ETH_HLEN))->src;
    m_freem(out);
    return src;
}
#endif

bool selftest_net_netif_flags(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: net-netif-flags: needs the debug park hook; skipping");
    return true;
#else
    static const uint8_t g_mac[6]     = { 0x52, 0x54, 0x00, 0x05, 0x00, 0x01 };
    static const uint8_t u_mac[6]     = { 0x52, 0x54, 0x00, 0x06, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x05, 0x00, 0x0f };
    static const uint8_t other_mac[6] = { 0x52, 0x54, 0x00, 0x05, 0x00, 0x14 };
    static const uint8_t peer_mac[6]  = { 0x52, 0x54, 0x00, 0x06, 0x00, 0x63 };
    uint32_t mask = htonl(0xffffff00u);
    uint32_t g_ip = IPV4_ADDR(10, 77, 5, 1), guest = IPV4_ADDR(10, 77, 5, 15), other = IPV4_ADDR(10, 77, 5, 20);
    uint32_t u_ip = IPV4_ADDR(10, 77, 6, 1), peer = IPV4_ADDR(10, 77, 6, 99);

    struct tap *g = nt_tap_create("flgg", g_ip, mask, g_mac);
    CHECK(g != NULL);
    struct tap *u = nt_tap_create("flgu", u_ip, mask, u_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);   /* a plain forwarder to begin with: masquerade off */
    nettest_seed_arp(tap_netif(u), peer, peer_mac);
    nettest_seed_arp(tap_netif(g), guest, guest_mac);
    nettest_seed_arp(tap_netif(g), other, other_mac);
    nat_flush();
    struct nat_stats n0, n1;
    nat_get_stats(&n0);

    /* 1. Masquerade turned on while a datagram from .20 -- on the subnet,
     * so the plain forwarder's anti-spoof passed it -- is parked before
     * the masquerade decision. One reading of the word: forwarded as the
     * forwarder it entered under, its source intact, no flow made. Two
     * readings: masqueraded, a source the masquerade's anti-spoof refuses
     * translated and given a NAT flow. */
    CHECK(flags_inject_parked(g, g_mac, other_mac, other, peer, true));
    uint32_t src = flags_recv_src(u);
    CHECK(src != 0);
    CHECK(src == other);   /* two readings masquerade it: src == u_ip */
    nat_get_stats(&n1);
    CHECK(n1.out_new == n0.out_new);

    /* 2. The mirror: masquerade turned off while the guest's (.15) datagram
     * is parked. One reading: masqueraded, the state it entered under. */
    CHECK(flags_inject_parked(g, g_mac, guest_mac, guest, peer, false));
    src = flags_recv_src(u);
    CHECK(src == u_ip);
    nat_get_stats(&n1);
    CHECK(n1.out_new == n0.out_new + 1);
    netif_set_masquerade(tap_netif(g), false);

    /* 3. A stream from .20 while another thread flips masquerade every
     * millisecond, no hook. Each datagram is either forwarded with its own
     * source (the forwarder's treatment) or refused by the masquerade's
     * anti-spoof (the masquerader's); none is masqueraded, and no flow is
     * made for .20. */
    struct flags_toggler tg = { .nif = tap_netif(g) };
    struct thread *th = thread_create(flags_toggler_thread, &tg, "flags-toggle", SCHED_PRIO_DEFAULT);
    CHECK(th != NULL);
    uint8_t payload[8] = { 9, 8, 7, 6, 5, 4, 3, 2 }, frame[128];
    unsigned sent = 0, got = 0, masqueraded = 0, foreign = 0;
    struct ip_stats i0, i1;
    ipv4_get_stats(&i0);
    for (unsigned i = 0; i < 300; i++) {
        uint32_t len = nettest_build_udp(frame, g_mac, other_mac, other, peer, 64, (uint16_t)(4200 + (i & 63)), 5100,
                                         payload, sizeof(payload));
        if (tap_inject(g, frame, len) == 0)
            sent++;
        struct mbuf *m;
        while ((m = tap_recv(u)) != NULL) {
            uint8_t hdr[ETH_HLEN + sizeof(struct ipv4_hdr)];
            if (m_copydata(m, 0, sizeof(hdr), hdr) && hdr[12] == 0x08 && hdr[13] == 0x00) {
                uint32_t s = ((struct ipv4_hdr *)(hdr + ETH_HLEN))->src;
                got++;
                if (s == u_ip)
                    masqueraded++;
                else if (s != other)
                    foreign++;
            }
            m_freem(m);
        }
        if ((i & 7) == 7)
            thread_sleep_ms(1);
    }
    tg.stop = true;
    thread_join(th);
    for (unsigned i = 0; i < 30; i++) {   /* what the worker forwarded after the last inject */
        struct mbuf *m;
        while ((m = tap_recv(u)) != NULL) {
            uint8_t hdr[ETH_HLEN + sizeof(struct ipv4_hdr)];
            if (m_copydata(m, 0, sizeof(hdr), hdr) && hdr[12] == 0x08 && hdr[13] == 0x00) {
                uint32_t s = ((struct ipv4_hdr *)(hdr + ETH_HLEN))->src;
                got++;
                if (s == u_ip)
                    masqueraded++;
                else if (s != other)
                    foreign++;
            }
            m_freem(m);
        }
        thread_sleep_ms(10);
    }
    ipv4_get_stats(&i1);
    nat_get_stats(&n1);
    CHECK(sent == 300);
    CHECK(masqueraded == 0);
    CHECK(foreign == 0);
    CHECK(got + (unsigned)(i1.fwd_spoofed - i0.fwd_spoofed) == sent);   /* every datagram: forwarded intact or refused */
    CHECK(n1.out_new == n0.out_new + 1);                               /* still only the guest's flow */
    CHECK(got >= 1 && i1.fwd_spoofed > i0.fwd_spoofed);                /* both treatments occurred */

    kinfo("selftest: net-netif-flags: a toggle parked between two checks follows one reading of the word; "
          "%u datagrams under %u flips: %u forwarded intact, %llu refused, none masqueraded",
          sent, tg.flips, got, (unsigned long long)(i1.fwd_spoofed - i0.fwd_spoofed));
    return true;
#endif
}

bool selftest_net_nat(const char **reason)
{
    static const uint8_t g_mac[6]     = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x01 };
    static const uint8_t u_mac[6]     = { 0x52, 0x54, 0x00, 0x04, 0x00, 0x01 };
    static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x03, 0x00, 0x0f };
    static const uint8_t peer_mac[6]  = { 0x52, 0x54, 0x00, 0x04, 0x00, 0x63 };
    uint32_t mask = htonl(0xffffff00u);
    uint32_t g_ip = IPV4_ADDR(10, 77, 3, 1), guest = IPV4_ADDR(10, 77, 3, 15);
    uint32_t u_ip = IPV4_ADDR(10, 77, 4, 1), peer = IPV4_ADDR(10, 77, 4, 99);

    struct tap *g = nt_tap_create("natg", g_ip, mask, g_mac);
    CHECK(g != NULL);
    struct tap *u = nt_tap_create("natu", u_ip, mask, u_mac);
    CHECK(u != NULL);
    netif_set_forward(tap_netif(g), true);
    netif_set_masquerade(tap_netif(g), true);   /* masquerade flows forwarded from the guest tap */
    nettest_seed_arp(tap_netif(u), peer, peer_mac);
    nettest_seed_arp(tap_netif(g), guest, guest_mac);
    nat_flush();

    uint8_t payload[12];
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(0x40 + i);
    uint8_t frame[256], l4[128], hdr[ETH_HLEN + 20 + 40];

    /* (1) UDP round trip: out masqueraded, reply un-masqueraded. */
    uint16_t l4len = nettest_mk_udp(l4, guest, peer, 6001, 7001, payload, sizeof(payload));
    uint32_t flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    struct mbuf *out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    struct ipv4_hdr *oi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(oi->src == u_ip && oi->dst == peer && oi->ttl == 63);     /* source masqueraded */
    uint8_t *ol4 = hdr + ETH_HLEN + 20;
    uint16_t nat_port = (uint16_t)(ol4[0] << 8 | ol4[1]);           /* the lent source port */
    CHECK(nat_port >= NAT_PORT_MIN && nat_port <= NAT_PORT_MAX);
    CHECK((uint16_t)(ol4[2] << 8 | ol4[3]) == 7001);               /* dest port unchanged */
    CHECK(nettest_l4_ok(u_ip, peer, IPPROTO_UDP, ol4, (uint16_t)(sizeof(struct udp_hdr) + sizeof(payload))));
    m_freem(out);

    l4len = nettest_mk_udp(l4, peer, u_ip, 7001, nat_port, payload, sizeof(payload));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    struct mbuf *back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    struct ipv4_hdr *bi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(bi->src == peer && bi->dst == guest && bi->ttl == 63);    /* delivered to the guest */
    uint8_t *bl4 = hdr + ETH_HLEN + 20;
    CHECK((uint16_t)(bl4[2] << 8 | bl4[3]) == 6001);               /* original guest port restored */
    CHECK(nettest_l4_ok(peer, guest, IPPROTO_UDP, bl4, (uint16_t)(sizeof(struct udp_hdr) + sizeof(payload))));
    CHECK(memcmp(bl4 + sizeof(struct udp_hdr), payload, sizeof(payload)) == 0);
    m_freem(back);

    /* (2) TCP SYN out, SYN-ACK back. */
    l4len = nettest_mk_tcp(l4, guest, peer, 6002, 80, TH_SYN);
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + 20, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t tcp_nat = (uint16_t)(ol4[0] << 8 | ol4[1]);
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->src == u_ip);
    CHECK(nettest_l4_ok(u_ip, peer, IPPROTO_TCP, ol4, sizeof(struct tcp_hdr)));
    m_freem(out);

    l4len = nettest_mk_tcp(l4, peer, u_ip, 80, tcp_nat, TH_SYN | TH_ACK);
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_TCP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + 20, hdr));
    bl4 = hdr + ETH_HLEN + 20;
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->dst == guest);
    CHECK((uint16_t)(bl4[2] << 8 | bl4[3]) == 6002);
    CHECK(nettest_l4_ok(peer, guest, IPPROTO_TCP, bl4, sizeof(struct tcp_hdr)));
    m_freem(back);

    /* (3) ICMP echo out, echo reply back (the id is the NAT identifier). */
    l4len = nettest_mk_icmp(l4, ICMP_ECHO, 0x4321, 1, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_ICMP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t icmp_nat = (uint16_t)(ol4[4] << 8 | ol4[5]);
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->src == u_ip && ol4[0] == ICMP_ECHO);
    CHECK(in_cksum(ol4, (uint16_t)(sizeof(struct icmp_hdr) + sizeof(payload))) == 0);
    m_freem(out);

    l4len = nettest_mk_icmp(l4, ICMP_ECHO_REPLY, icmp_nat, 1, payload, sizeof(payload));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, l4, l4len);
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + (int)sizeof(payload) + 8, hdr));
    bl4 = hdr + ETH_HLEN + 20;
    CHECK(((struct ipv4_hdr *)(hdr + ETH_HLEN))->dst == guest && bl4[0] == ICMP_ECHO_REPLY);
    CHECK((uint16_t)(bl4[4] << 8 | bl4[5]) == 0x4321);            /* original id restored */
    CHECK(in_cksum(bl4, (uint16_t)(sizeof(struct icmp_hdr) + sizeof(payload))) == 0);
    m_freem(back);

    /* (4) ICMP error quoting a NAT'd packet is translated back to the guest.
     * First open a UDP flow, then deliver a dest-unreach whose quote is the
     * packet we sent out. */
    nat_flush();
    l4len = nettest_mk_udp(l4, guest, peer, 6100, 53, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    out = nettest_recv_ip(u);
    CHECK(out != NULL);
    CHECK(m_copydata(out, 0, ETH_HLEN + 20 + 8, hdr));
    ol4 = hdr + ETH_HLEN + 20;
    uint16_t err_nat = (uint16_t)(ol4[0] << 8 | ol4[1]);
    m_freem(out);
    /* Build the ICMP error: [icmp hdr][quoted IP: u_ip->peer UDP][8 bytes of
     * that UDP: sport=err_nat, dport=53]. */
    uint8_t icmperr[8 + 20 + 8];
    memset(icmperr, 0, sizeof(icmperr));
    icmperr[0] = ICMP_DEST_UNREACH; icmperr[1] = ICMP_UNREACH_PORT;
    struct ipv4_hdr *q = (struct ipv4_hdr *)(icmperr + 8);
    q->vhl = 0x45; q->len = htons(20 + 8); q->ttl = 63; q->proto = IPPROTO_UDP;
    q->src = u_ip; q->dst = peer; q->cksum = in_cksum(q, 20);
    uint8_t *qudp = icmperr + 8 + 20;
    qudp[0] = (uint8_t)(err_nat >> 8); qudp[1] = (uint8_t)err_nat;   /* sport = the lent port */
    qudp[2] = 0; qudp[3] = 53;                                        /* dport */
    uint16_t *ecs = (uint16_t *)(icmperr + 2);
    *ecs = 0; *ecs = in_cksum(icmperr, sizeof(icmperr));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, icmperr, sizeof(icmperr));
    CHECK(tap_inject(u, frame, flen) == 0);
    back = nettest_recv_ip(g);
    CHECK(back != NULL);
    CHECK(m_copydata(back, 0, ETH_HLEN + 20 + 8 + 20 + 8, hdr));
    bi = (struct ipv4_hdr *)(hdr + ETH_HLEN);
    CHECK(bi->dst == guest && bi->proto == IPPROTO_ICMP);
    struct ipv4_hdr *iq = (struct ipv4_hdr *)(hdr + ETH_HLEN + 20 + 8);
    CHECK(iq->src == guest);                                         /* inner src un-NAT'd */
    uint8_t *iu = hdr + ETH_HLEN + 20 + 8 + 20;
    CHECK((uint16_t)(iu[0] << 8 | iu[1]) == 6100);                  /* inner source port restored */
    m_freem(back);

    /* (4b) Anti-spoof: a guest frame sourced from the uplink subnet (not the
     * guest tap's own) is dropped on forwarding, never masqueraded or
     * emitted with the forged source. */
    struct ip_stats is0, is1;
    ipv4_get_stats(&is0);
    l4len = nettest_mk_udp(l4, IPV4_ADDR(10, 77, 4, 5), peer, 6200, 53, payload, sizeof(payload));
    flen = nettest_wrap(frame, g_mac, guest_mac, IPV4_ADDR(10, 77, 4, 5), peer, 64, IPPROTO_UDP, l4, l4len);
    CHECK(tap_inject(g, frame, flen) == 0);
    CHECK(nettest_recv_ip(u) == NULL);                             /* forged source never leaves */
    ipv4_get_stats(&is1);
    CHECK(is1.fwd_spoofed > is0.fwd_spoofed);

    /* (4c) An ICMP error with a bad checksum is not translated (the normal
     * receive path would drop it; nat_in must not launder it). Reuse the
     * error frame but corrupt the ICMP checksum. */
    *ecs = (uint16_t)(*ecs ^ htons(0x1));
    flen = nettest_wrap(frame, u_mac, peer_mac, peer, u_ip, 64, IPPROTO_ICMP, icmperr, sizeof(icmperr));
    CHECK(tap_inject(u, frame, flen) == 0);
    CHECK(nettest_recv_ip(g) == NULL);                             /* corrupt error not forwarded */

    /* (5) Share exhaustion: distinct flows fill this guest's quota;
     * further ones are dropped without consuming another guest's slots. */
    nat_flush();
    struct nat_stats ns0, ns1;
    nat_get_stats(&ns0);
    const unsigned injected = NAT_TABLE_SIZE + 8;
    for (unsigned i = 0; i < injected; i++) {
        l4len = nettest_mk_udp(l4, guest, peer, (uint16_t)(10000 + i), 9, payload, 4);
        flen = nettest_wrap(frame, g_mac, guest_mac, guest, peer, 64, IPPROTO_UDP, l4, l4len);
        CHECK(tap_inject(g, frame, flen) == 0);
        if ((i & 31) == 31) {                 /* keep the uplink queue drained */
            struct mbuf *d;
            while ((d = tap_recv(u)) != NULL)
                m_freem(d);
        }
    }
    /* nat_flush clears entries, not lifetime counters. Count only this
     * flood's outcomes: earlier round trips must not let us age the table
     * while tail packets can still create entries behind that sweep. */
    for (unsigned i = 0; i < 200; i++) {
        struct mbuf *d;
        while ((d = tap_recv(u)) != NULL)
            m_freem(d);
        nat_get_stats(&ns1);
        if ((ns1.out_new - ns0.out_new) + (ns1.out_drop_share - ns0.out_drop_share) >= injected)
            break;
        thread_sleep_ms(10);
    }
    nat_get_stats(&ns1);
    CHECK((ns1.out_new - ns0.out_new) + (ns1.out_drop_share - ns0.out_drop_share) == injected);
    CHECK(ns1.entries == NAT_QUOTA_PER_GUEST);            /* one guest is capped at its quota */
    CHECK(ns1.out_drop_share > ns0.out_drop_share);      /* new flows dropped once the share is full */
    CHECK(ns1.out_drop_table == ns0.out_drop_table);      /* the share, not the table: 32 of 256 in use */

    /* (6) Expiry: aging past the timeout reclaims the entries. */
    nat_get_stats(&ns0);
    CHECK(ns0.entries > 0);
    nat_age(clock_now_ns() + 2ull * NAT_TIMEOUT_UDP_NS);
    nat_get_stats(&ns1);
    CHECK(ns1.entries == 0 && ns1.expired > ns0.expired);

    nat_flush();
    nt_tap_destroy(u);
    nt_tap_destroy(g);
    kinfo("selftest: net-nat: UDP/TCP/ICMP round trips masqueraded and restored (checksums valid), "
          "an ICMP error translated back, the guest capped at %u entries and the flood accounted for before expiry",
          NAT_QUOTA_PER_GUEST);
    return true;
}

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

/* --- a tap's release against a frame still in flight --------------------------------
 *
 * tap_chr_release purged the guest's NAT and firewall state and then
 * destroyed the tap; a frame the guest wrote before its last close, still
 * on a worker's queue, was masqueraded in between and its translation
 * outlived the tap -- the next tap given the subnet inherited it
 * (fuzz_net_config). The seam delivers such a frame through the tap's
 * interface just after the purges and waits for the workers: nothing may
 * name the guest when the close returns.
 */
#if CONFIG_DEBUG
struct tro_ctx {
    struct netif *nif;      /* held across the close */
    uint8_t frame[128];
    uint32_t flen;
    int delivered;          /* netif_rx was offered the frame */
};

static void tro_after_purge(void *arg)
{
    struct tro_ctx *c = arg;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    memcpy(m->data, c->frame, c->flen);
    m->len = m->pkt.len = c->flen;
    netif_rx(c->nif, m);    /* the tap's own path in (tap_inject) */
    c->delivered++;
    net_workers_barrier();  /* whatever the stack makes of it is made */
}

static void tro_disarm(void *arg)
{
    (void)arg;
    tap_test_after_purge = NULL;
    tap_test_after_purge_arg = NULL;
}

static unsigned tro_naming(uint32_t guest)
{
    static struct nat_flow nf[NAT_TABLE_SIZE];
    static struct fw_flow_info ff[FW_FLOW_MAX];
    unsigned n = 0;
    unsigned nn = nat_flow_list(nf, NAT_TABLE_SIZE, clock_now_ns());
    for (unsigned i = 0; i < nn; i++)
        n += nf[i].orig_ip == guest;
    unsigned nfl = fw_flow_list(ff, FW_FLOW_MAX, clock_now_ns());
    for (unsigned i = 0; i < nfl; i++)
        n += ff[i].guest_ip == guest || ff[i].a_ip == guest || ff[i].b_ip == guest;
    return n;
}

#endif /* CONFIG_DEBUG */

bool selftest_net_tap_release_order(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: net-tap-release-order: no test hooks in this build; skipping");
    return true;
#else
    fw_flush();
    nat_flush();
    CHECK(selftest_defer(nt_rel_nat_state, &g_nat_state_marker));
    struct file *fa = NULL;
    CHECK(nt_vfs_open(NULL, "/dev/net/tap", COSMO_O_RDWR | COSMO_O_NONBLOCK, 0, &fa) == 0 && fa != NULL);
    uint32_t ga = IPV4_ADDR(10, 0, 3, 15), world = IPV4_ADDR(10, 0, 2, 2);
    static const uint8_t amac[6] = { 0x52, 0x54, 0x00, 0x0d, 0x01, 0x0a };
    static const uint8_t tap0mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };
    static struct tro_ctx c;
    memset(&c, 0, sizeof(c));
    static struct nt_netif_ref held;
    c.nif = netif_find("tap0");
    CHECK(c.nif != NULL && nt_netif_hold(&held, c.nif));
    CHECK(c.nif->ip4.addr == IPV4_ADDR(10, 0, 3, 1));
    nettest_seed_arp(c.nif, ga, amac);

    /* The control: the same frame before the close is masqueraded. */
    uint8_t pl[4] = { 1, 2, 3, 4 }, l4[32];
    uint16_t l4len = nettest_mk_udp(l4, ga, world, 7100, 53, pl, sizeof(pl));
    c.flen = nettest_wrap(c.frame, tap0mac, amac, ga, world, 64, IPPROTO_UDP, l4, l4len);
    CHECK(nettest_fwt_send(fa, tap0mac, amac, ga, world, IPPROTO_UDP, l4, l4len));
    net_workers_barrier();
    unsigned before = tro_naming(ga);
    CHECK(before > 0);      /* a translation (and a flow) for the guest: the frame is one that makes state */
    nat_flush();
    fw_flush();
    CHECK(tro_naming(ga) == 0);

    CHECK(selftest_defer(tro_disarm, &c));
    tap_test_after_purge_arg = &c;
    tap_test_after_purge = tro_after_purge;
    nt_file_put(fa);        /* the last close: the release, and the seam inside it */
    tro_disarm(NULL);
    selftest_forget(&c);
    CHECK(c.delivered == 1);
    unsigned left = tro_naming(ga);
    if (left != 0)
        kerror("selftest: net-tap-release-order: %u NAT entr(ies)/flow(s) name the released guest", left);
    CHECK(left == 0);
    if (!selftest_release(&g_nat_state_marker))
        nt_rel_nat_state(NULL);
    nt_netif_ref_put(&held);
    kinfo("selftest: net-tap-release-order: a frame offered after the purges made nothing (the control made %u)",
          before);
    return true;
#endif
}

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

/* --- net-syncookie-seeded (NET-6) --------------------------------------------- *
 *
 * tcp_init draws the SYN-cookie secret, and on a machine whose pool seeds
 * after it -- no firmware seed, no CPU random instruction -- that secret
 * came from an unseeded pool, predictable in principle, and was kept for
 * the life of the boot. A cookie made once the pool is seeded must use a
 * secret drawn from it. The seam marks the current secret as drawn
 * unseeded, the case such a machine starts in.
 */
bool selftest_net_syncookie_seeded(const char **reason)
{
#if !CONFIG_SELFTEST
    (void)reason;
    kinfo("selftest: net-syncookie-seeded: needs CONFIG_SELFTEST; skipping");
    return true;
#else
    if (!random_ready()) {
        kinfo("selftest: net-syncookie-seeded: the pool never seeded (S19 warned); skipping");
        return true;
    }
    tcp_test_cookie_mark_unseeded();
    (void)tcp_test_cookie_make();
    CHECK(tcp_test_cookie_seeded());   /* main keeps tcp_init's secret, seeded or not */
    kinfo("selftest: net-syncookie-seeded: a cookie made on a seeded pool used a secret drawn from it");
    return true;
#endif
}
