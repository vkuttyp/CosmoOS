/*
 * tap.h - A tap interface: a netif whose far end is a userland process
 * (docs/audit/next-subsystem-tap.md).
 *
 * The stack treats a tap as an ordinary interface -- it ARPs on it, routes
 * to its subnet, answers what is addressed to its IP. The difference is
 * where the frames go: a frame the stack transmits out a tap is queued for
 * a reader (tap_recv) rather than put on a wire, and a frame a writer
 * injects (tap_inject) enters the stack as netif_rx would from a driver. A
 * VM owner bridges its guest's virtio-net to a tap; the tests drive one
 * directly.
 */
#ifndef KERNEL_NET_TAP_H
#define KERNEL_NET_TAP_H

#include <kernel/mbuf.h>
#include <kernel/types.h>

#define TAP_TXQ_MAX 64u   /* frames the stack may queue for the reader; drops when full */

struct netif;
struct tap;

/* Create and register an up tap interface `name` with IPv4 `ip`/`mask` and
 * link address `mac`. NULL on no memory or a duplicate name. */
struct tap *tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t mac[6]);
/* Unregister and free it (no transmit or receive after this returns). */
void tap_destroy(struct tap *t);

/* Inject one Ethernet frame from the far end into the stack (netif_rx).
 * 0, or -EMSGSIZE / -ENOMEM. */
int tap_inject(struct tap *t, const void *frame, uint32_t len);
/* The next frame the stack transmitted out the tap (the far end's to read),
 * or NULL when none waits. The caller owns it and frees it (m_freem). */
struct mbuf *tap_recv(struct tap *t);

struct netif *tap_netif(struct tap *t);   /* for tests and lookups */

#endif /* KERNEL_NET_TAP_H */
