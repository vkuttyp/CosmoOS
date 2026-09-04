/*
 * inet.c - Address helpers.
 */

#include <kernel/errno.h>
#include <kernel/net/inet.h>
#include <kernel/printf.h>
#include <kernel/string.h>

bool netaddr_addr_equal(const struct netaddr *a, const struct netaddr *b)
{
    if (a->family != b->family)
        return false;
    if (a->family == COSMO_AF_INET)
        return a->v4 == b->v4;
    return in6_equal(&a->v6, &b->v6);
}

bool netaddr_equal(const struct netaddr *a, const struct netaddr *b)
{
    return a->port == b->port && netaddr_addr_equal(a, b);
}

bool netaddr_is_unspecified(const struct netaddr *a)
{
    if (a->family == COSMO_AF_INET)
        return a->v4 == 0;
    if (a->family == COSMO_AF_INET6)
        return in6_is_unspecified(&a->v6);
    return true;
}

int netaddr_from_user_shape(struct netaddr *out, const struct cosmo_sockaddr *in)
{
    memset(out, 0, sizeof(*out));
    if (in->family == COSMO_AF_INET) {
        out->family = COSMO_AF_INET;
        memcpy(&out->v4, in->addr, 4);
    } else if (in->family == COSMO_AF_INET6) {
        out->family = COSMO_AF_INET6;
        memcpy(out->v6.s6_addr, in->addr, 16);
    } else {
        return -EAFNOSUPPORT;
    }
    out->port = in->port;
    return 0;
}

void netaddr_to_user_shape(struct cosmo_sockaddr *out, const struct netaddr *in)
{
    memset(out, 0, sizeof(*out));
    out->family = in->family;
    out->port = in->port;
    if (in->family == COSMO_AF_INET)
        memcpy(out->addr, &in->v4, 4);
    else if (in->family == COSMO_AF_INET6)
        memcpy(out->addr, in->v6.s6_addr, 16);
}

const char *netaddr_str(const struct netaddr *a, char *buf, size_t len)
{
    if (a->family == COSMO_AF_INET) {
        const uint8_t *b = (const uint8_t *)&a->v4;
        ksnprintf(buf, len, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], a->port);
    } else if (a->family == COSMO_AF_INET6) {
        size_t n = 0;
        n += (size_t)ksnprintf(buf + n, len - n, "[");
        for (unsigned i = 0; i < 16 && n < len; i += 2)
            n += (size_t)ksnprintf(buf + n, len - n, "%x%s", (unsigned)((a->v6.s6_addr[i] << 8) | a->v6.s6_addr[i + 1]),
                                   i < 14 ? ":" : "");
        if (n < len)
            ksnprintf(buf + n, len - n, "]:%u", a->port);
    } else {
        ksnprintf(buf, len, "<unspec>:%u", a->port);
    }
    return buf;
}
