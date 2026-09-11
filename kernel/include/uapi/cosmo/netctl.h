/*
 * netctl.h - The /dev/net/tapctl control ABI (docs/audit/next-subsystem-netctl.md).
 *
 * A privileged owner configures the guest's networking at runtime by writing
 * one fixed-layout command (struct cosmo_netctl) to /dev/net/tapctl, and
 * reads the live rules back as a versioned snapshot (struct cosmo_netctl_list
 * followed by that many struct cosmo_netctl_rule). Ports are host byte order;
 * addresses are network byte order (an IPv4 address as stored on the wire).
 */
#ifndef UAPI_COSMO_NETCTL_H
#define UAPI_COSMO_NETCTL_H

#include <stdint.h>

#define COSMO_NETCTL_VERSION 1

/* Opcodes for struct cosmo_netctl.op. */
#define COSMO_NETCTL_FORWARD_ADD 1   /* add a port-forward (DNAT) rule */
#define COSMO_NETCTL_FORWARD_DEL 2   /* remove the rule bound to (proto, host_port) */

/* Protocol values (the IANA IP protocol numbers, stable). */
#define COSMO_NETCTL_PROTO_TCP 6
#define COSMO_NETCTL_PROTO_UDP 17

/* One control command, written whole to /dev/net/tapctl. */
struct cosmo_netctl {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t op;           /* COSMO_NETCTL_FORWARD_* */
    uint8_t  proto;        /* COSMO_NETCTL_PROTO_TCP / UDP */
    uint8_t  reserved;     /* must be 0 */
    uint16_t host_port;    /* host byte order */
    uint16_t guest_port;   /* host byte order (ignored by DEL) */
    uint16_t reserved2;    /* must be 0 */
    uint32_t guest_addr;   /* network byte order (ignored by DEL) */
};

/* The read snapshot: this header, then `count` rules. */
struct cosmo_netctl_list {
    uint16_t version;      /* COSMO_NETCTL_VERSION */
    uint16_t count;        /* number of struct cosmo_netctl_rule following */
};

struct cosmo_netctl_rule {
    uint8_t  proto;
    uint8_t  reserved;
    uint16_t host_port;    /* host byte order */
    uint16_t guest_port;   /* host byte order */
    uint16_t reserved2;
    uint32_t guest_addr;   /* network byte order */
};

#endif /* UAPI_COSMO_NETCTL_H */
