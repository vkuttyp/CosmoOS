/*
 * inet.h - Byte order, addresses, and the family-tagged address the
 * stack uses internally.
 */

#ifndef KERNEL_NET_INET_H
#define KERNEL_NET_INET_H

#include <kernel/types.h>

#include <uapi/cosmo/syscall.h>

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

struct in6_addr {
    uint8_t s6_addr[16];
};

/* Internal address: family, port in host order, address in network order. */
struct netaddr {
    uint16_t family;      /* COSMO_AF_INET / COSMO_AF_INET6, 0 = unspecified */
    uint16_t port;
    union {
        uint32_t v4;      /* network byte order */
        struct in6_addr v6;
    };
};

#define IPV4_ADDR(a, b, c, d) htonl(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define INADDR_ANY_N       0u
#define INADDR_BROADCAST_N 0xffffffffu
#define INADDR_LOOPBACK_N  IPV4_ADDR(127, 0, 0, 1)

static inline bool in6_is_unspecified(const struct in6_addr *a)
{
    for (unsigned i = 0; i < 16; i++)
        if (a->s6_addr[i])
            return false;
    return true;
}
static inline bool in6_is_loopback(const struct in6_addr *a)
{
    for (unsigned i = 0; i < 15; i++)
        if (a->s6_addr[i])
            return false;
    return a->s6_addr[15] == 1;
}
static inline bool in6_is_linklocal(const struct in6_addr *a) { return a->s6_addr[0] == 0xfe && (a->s6_addr[1] & 0xc0) == 0x80; }
static inline bool in6_is_multicast(const struct in6_addr *a) { return a->s6_addr[0] == 0xff; }
static inline bool in6_equal(const struct in6_addr *a, const struct in6_addr *b)
{
    for (unsigned i = 0; i < 16; i++)
        if (a->s6_addr[i] != b->s6_addr[i])
            return false;
    return true;
}

bool netaddr_equal(const struct netaddr *a, const struct netaddr *b);   /* family, port and address */
bool netaddr_addr_equal(const struct netaddr *a, const struct netaddr *b);
bool netaddr_is_unspecified(const struct netaddr *a);
/* Conversions with the UAPI shape; -EAFNOSUPPORT/-EINVAL on bad input. */
int netaddr_from_user_shape(struct netaddr *out, const struct cosmo_sockaddr *in);
void netaddr_to_user_shape(struct cosmo_sockaddr *out, const struct netaddr *in);
/* "a.b.c.d:port" or "[v6]:port" for logs, into buf (>= 64 bytes). */
const char *netaddr_str(const struct netaddr *a, char *buf, size_t len);

#endif /* KERNEL_NET_INET_H */
