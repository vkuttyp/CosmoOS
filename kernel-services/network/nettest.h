/*
 * nettest.h - What nettest.c shares with its CeeSafe batches (roadmap 2.0
 * SA6, nettest): the loopback test servers' state, the release framework's
 * wrappers a batch calls, and the bulk transfer nettest_lo.cee defines for
 * the C tests still here. Private to kernel-services/network.
 */
#ifndef KERNEL_SERVICES_NETWORK_NETTEST_H
#define KERNEL_SERVICES_NETWORK_NETTEST_H

#include <kernel/net/inet.h>
#include <kernel/netif.h>
#include <kernel/socket.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>

/* A loopback test's server thread. Its storage outlives any frame (the
 * thread may run after the test returns), and nettest_tcp_server_start
 * registers its release the moment it exists. */
struct tcp_server {
    struct netaddr addr;
    uint32_t bytes_seen;
    uint32_t bytes_echoed;
    int result;
    volatile bool done;
    bool echo;               /* echo mode for the harness */
    unsigned param;          /* a per-thread parameter (net-tcp-nagle-peer: the reader's delay in ms) */
    volatile bool stop;
    /* For its release (the net-leftover unit): the thread, and the
     * sockets it is blocked on, published under `lock` so the release can
     * take a reference and shut them without racing the thread's put. */
    struct thread *th;
    spinlock_t lock;
    struct socket *ls, *c;
};

/* nettest.c's release framework (the nt_* wrappers), for the CeeSafe batches. */
bool nettest_tcp_server_start(struct tcp_server *srv, void (*fn)(void *), const char *name, int prio);
/* Publish the server's listener (or its connection) for the release, under its lock. */
void nettest_tcp_server_publish(struct tcp_server *srv, bool listener, struct socket *sock);
int nettest_ksock_create(int family, int type, uint32_t uid, struct socket **out);
int nettest_ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer);
void nettest_ksock_put(struct socket *s);
void nettest_loopback_set_filter(lo_filter_fn fn, void *arg);
/* net-lo-tcp's and net-accept-order's listen backlog (nettest.c). */
bool nettest_lo_tcp_backlog(const char **reason, uint16_t port, bool force, bool *accepted_first);

/* nettest_lo.cee: move `bytes` of a pattern to a sink thread on `addr` and
 * check the connection's end, lingering `linger_ms` past it (net-lo-tcp,
 * net-lo-tcp-loss, net-rx-dup, net-tcp-reorder). */
bool nettest_tcp_transfer(const char **reason, const struct netaddr *addr, uint32_t bytes, uint32_t linger_ms);

#endif
