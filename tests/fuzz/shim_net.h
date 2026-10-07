/*
 * shim_net.h - The host build of the network stack, for the fuzz targets
 * (docs/verification/design.md, "Fuzzing"; the network testing notes,
 * "Host fuzzing").
 *
 * The protocol layers (ether.c, arp.c, ipv4.c, ipv6.c, udp.c, tcp.c, nat.c,
 * fw.c, mbuf.c, cksum.c, inet.c) compile unchanged. What they need from the
 * rest of the kernel -- the interface registry (netif.c), the worker's work
 * queue, timers, the clock, the allocator, kobjects, sockets to wake -- is
 * supplied here in a shape a single-threaded fuzz target controls: timers
 * fire when the target advances the clock, queued work runs when the target
 * says so, and every transmitted frame is captured on its interface.
 */
#ifndef COSMO_FUZZ_SHIM_NET_H
#define COSMO_FUZZ_SHIM_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/mbuf.h>
#include <kernel/netif.h>

/* An interface the target owns: a netif whose transmit captures the frame. */
#define FZ_CAPTURE_MAX 2048u

struct fz_netif {
    struct netif nif;
    unsigned transmits;            /* frames the stack sent out of it */
    unsigned oversize;             /* frames longer than mtu + ETH_HLEN (an oracle) */
    unsigned runts;                /* frames shorter than 60 bytes */
    uint8_t last[FZ_CAPTURE_MAX];  /* the most recent frame, whole */
    uint32_t last_len;
    /* The target's per-frame observer (checksums, lengths), or NULL. */
    void (*on_frame)(struct fz_netif *f, const uint8_t *frame, uint32_t len);
};

/* Once per process: mbufs, ARP, ND, UDP and TCP (what net_init does, less
 * the workers and loopback). The loopback interface the routing code asks
 * for is a static one here that drops what it is given. */
void fz_net_init(void);

/* Register an interface under `name` with `mac`, `addr/mask` (network
 * order) and `flags` (NETIF_UP is added). Index and the IPv6 link-local
 * address are assigned as netif_register does. At most 4. */
void fz_netif_register(struct fz_netif *f, const char *name, const uint8_t mac[6], uint32_t addr, uint32_t mask,
                       uint32_t gateway, unsigned flags);
void fz_netif_unregister_all(void);

/* Deliver a frame as the worker would after netif_rx: a cluster mbuf
 * holding `len` bytes, rcvif and rx_ns stamped, then ether_input. */
void fz_deliver(struct fz_netif *f, const void *frame, uint32_t len);

/* The clock the stack reads (clock_now_ns). Advancing it fires every timer
 * due by the new time, in expiry order, running queued work after each,
 * exactly as the kernel's timer interrupt hands off to the network worker.
 * fz_run_work runs work queued without a timer (none today: the stack
 * queues work only from timers). */
uint64_t fz_now(void);
void fz_clock_advance(uint64_t ns);
void fz_run_work(void);
unsigned fz_timers_pending(void);
/* Fire the earliest pending timer, moving the clock to it, at most
 * `max_fires` timers one at a time, never past now + `max_ns`; the number
 * fired. (A pending timer started again is a panic, as in the kernel.) */
unsigned fz_fire_until(uint64_t max_ns, unsigned max_fires);

/* The allocator's live-object count (kmalloc, kzalloc and every slab cache):
 * the leak oracle, read before setup and after teardown. */
size_t fz_allocs_live(void);
void fz_dump_live(void);   /* every live object's address and size, to stderr */
void *fz_live_object_of_size(size_t size);   /* a live object of exactly this size, or NULL */
void fz_dump_timers(void);   /* every pending timer, to stderr */

/* How many times the stack woke a socket or set an error on one. */
unsigned fz_sock_wakes(void);
/* Wake references held (sock_wake_ref less sock_wake_unref): zero whenever
 * the stack has returned to the target. */
unsigned fz_sock_wake_refs(void);

/* Reseed the stack's random source (random_u64) so an input replays. */
void fz_random_seed(uint64_t seed);

#endif /* COSMO_FUZZ_SHIM_NET_H */
