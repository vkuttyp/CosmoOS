/*
 * netioctl.c - Linux's interface ioctls on a socket (shell completeness S1;
 * docs/compat/linux/design.md, "Interface ioctls").
 *
 * BusyBox's ifconfig, route and arp learn about interfaces through
 * netdevice(7)'s SIOCGIF* requests on any socket, and configure them
 * through SIOCSIF*. The getters translate the stack's interface listing
 * (netif_info_by_name, netif_list), so they show exactly what /proc/net,
 * netctl and the packet paths see. The setters are PRIV_NET's, as netctl's
 * configuration is (roadmap M5), and checked before anything else, as
 * Linux checks CAP_NET_ADMIN.
 *
 * Two setters exist only to answer: SIOCSIFMTU and SIOCSIFHWADDR take
 * the value the interface already has and refuse any other with
 * -EOPNOTSUPP. IPv4 output never fragments and TCP's segment size is a
 * constant, so a smaller MTU would fail every full-sized send rather than
 * shrink them; and the address a NIC filters on is the device's.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/netif.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include "linux_abi.h"
#include "linux_internal.h"

_Static_assert(sizeof(struct lx_ifreq) == 40, "struct ifreq is 40 bytes on both 64-bit ABIs");
_Static_assert(sizeof(struct lx_ifconf) == 16, "struct ifconf is 16 bytes on both 64-bit ABIs");


/* The flags Linux would report for this interface. RUNNING (the carrier)
 * follows UP: no driver here reports a link state of its own. */
static int16_t lx_flags(const struct netif_info *f)
{
    unsigned v = 0;
    if (f->flags & NETIF_UP)
        v |= LX_IFF_UP | LX_IFF_RUNNING;
    if (f->flags & NETIF_LOOPBACK)
        v |= LX_IFF_LOOPBACK;
    else
        v |= LX_IFF_BROADCAST | LX_IFF_MULTICAST;
    if (f->flags & NETIF_NOARP)
        v |= LX_IFF_NOARP;
    return (int16_t)v;
}

/* The broadcast address a configured interface has (loopback has none). */
static uint32_t brd_of(const struct netif_info *f)
{
    return (f->flags & NETIF_LOOPBACK) ? 0 : (f->addr | ~f->mask);
}

/* A class's natural mask, which Linux gives an address SIOCSIFADDR sets. */
static uint32_t class_mask(uint32_t addr)
{
    uint32_t a = ntohl(addr);
    return htonl(a < 0x80000000u ? 0xff000000u : a < 0xc0000000u ? 0xffff0000u : 0xffffff00u);
}

static bool mask_contiguous(uint32_t mask)
{
    uint32_t m = ntohl(mask);
    return (m & (~m >> 1)) == 0;   /* ones, then zeros */
}

static void put_addr(struct lx_ifreq *r, uint32_t addr)
{
    memset(&r->ifr_addr, 0, sizeof(r->ifr_addr));
    r->ifr_addr.family = LX_AF_INET;
    r->ifr_addr.addr = addr;
}

/* SIOCGIFCONF: an AF_INET entry per interface with an IPv4 address, as
 * Linux lists them; with no buffer, the length a full listing needs. */
static int64_t ifconf(uint64_t arg)
{
    struct lx_ifconf c;
    if (copy_from_user(&c, arg, sizeof(c)))
        return -EFAULT;
    struct netif_info *ifs = kmalloc(NETIF_MAX * sizeof(*ifs), 0);
    if (ifs == NULL)
        return -ENOMEM;
    unsigned n = netif_list(ifs, NETIF_MAX);   /* every interface: the registry holds no more */
    int32_t used = 0;
    int64_t rc = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ifs[i].addr == 0)
            continue;
        if (c.ifc_buf != 0) {
            if (c.ifc_len < 0 || (size_t)used + sizeof(struct lx_ifreq) > (size_t)c.ifc_len)
                break;
            struct lx_ifreq r;
            memset(&r, 0, sizeof(r));
            strlcpy(r.ifr_name, ifs[i].name, sizeof(r.ifr_name));
            put_addr(&r, ifs[i].addr);
            if (copy_to_user(c.ifc_buf + (uint64_t)used, &r, sizeof(r))) {
                rc = -EFAULT;
                break;
            }
        }
        used += (int32_t)sizeof(struct lx_ifreq);
    }
    kfree(ifs);
    if (rc)
        return rc;
    c.ifc_len = used;
    return copy_to_user(arg, &c, sizeof(c)) ? -EFAULT : 0;
}

/* A new IPv4 configuration, as the DHCP client applies one: a change of
 * address, mask or gateway also drops the interface's neighbours, which
 * were learned under the old one (a reused address or a router kept on the
 * link must be resolved afresh, not sent to a remembered MAC). */
static void set_ipv4(struct netif *nif, const struct netif_info *f, uint32_t a, uint32_t m, uint32_t g)
{
    if (a == f->addr && m == f->mask && g == f->gateway)
        return;
    netif_set_ipv4(nif, a, m, g);
    arp_flush(nif);
}

/* SIOCSIF{FLAGS,ADDR,NETMASK,BRDADDR,MTU,HWADDR}, PRIV_NET checked. */
static int64_t set_one(unsigned req, const struct lx_ifreq *r, const struct netif_info *f)
{
    switch (req) {
    case LX_SIOCSIFMTU:
        return (uint32_t)r->ifr_ivalue == f->mtu ? 0 : -EOPNOTSUPP;
    case LX_SIOCSIFHWADDR:
        return memcmp(r->ifr_hwaddr.data, f->mac, sizeof(f->mac)) == 0 ? 0 : -EOPNOTSUPP;
    case LX_SIOCSIFBRDADDR:
        /* There is no broadcast of its own to set: it is the address's. */
        if (f->addr == 0)
            return -EADDRNOTAVAIL;
        return r->ifr_addr.addr == brd_of(f) ? 0 : -EINVAL;
    default:
        break;
    }
    struct netif *nif = netif_find(f->name);
    if (nif == NULL)
        return -ENODEV;
    int64_t rc = 0;
    switch (req) {
    case LX_SIOCSIFFLAGS: {
        /* UP is the one flag a caller sets; RUNNING follows it (Linux
         * ignores it too). Asking for anything else -- PROMISC, NOARP --
         * is refused rather than pretended. */
        int16_t have = lx_flags(f), want = r->ifr_flags;
        if ((have ^ want) & ~(LX_IFF_UP | LX_IFF_RUNNING)) {
            rc = -EINVAL;
            break;
        }
        bool up = (want & LX_IFF_UP) != 0;
        if (up != ((f->flags & NETIF_UP) != 0))
            netif_set_up(nif, up);
        break;
    }
    case LX_SIOCSIFADDR: {
        if (r->ifr_addr.family != LX_AF_INET) {
            rc = -EINVAL;
            break;
        }
        uint32_t a = r->ifr_addr.addr;
        uint32_t m = a == 0 ? 0 : class_mask(a);   /* Linux: address 0 removes it */
        /* The gateway stays while it is still on the link. */
        uint32_t g = a != 0 && ((f->gateway ^ a) & m) == 0 ? f->gateway : 0;
        set_ipv4(nif, f, a, m, g);
        break;
    }
    case LX_SIOCSIFNETMASK: {
        uint32_t m = r->ifr_addr.addr;
        if (f->addr == 0) {
            rc = -EADDRNOTAVAIL;
            break;
        }
        if (!mask_contiguous(m)) {
            rc = -EINVAL;
            break;
        }
        uint32_t g = ((f->gateway ^ f->addr) & m) == 0 ? f->gateway : 0;
        set_ipv4(nif, f, f->addr, m, g);
        break;
    }
    default:
        rc = -ENOTTY;
    }
    netif_put(nif);
    return rc;
}

int64_t lx_ioctl_net(unsigned req, uint64_t arg)
{
    if (req == LX_SIOCGIFCONF)
        return ifconf(arg);
    bool set = req == LX_SIOCSIFFLAGS || req == LX_SIOCSIFADDR || req == LX_SIOCSIFNETMASK ||
               req == LX_SIOCSIFBRDADDR || req == LX_SIOCSIFMTU || req == LX_SIOCSIFHWADDR;
    bool get = req == LX_SIOCGIFNAME || req == LX_SIOCGIFFLAGS || req == LX_SIOCGIFADDR ||
               req == LX_SIOCGIFBRDADDR || req == LX_SIOCGIFNETMASK || req == LX_SIOCGIFMETRIC ||
               req == LX_SIOCGIFMTU || req == LX_SIOCGIFHWADDR || req == LX_SIOCGIFINDEX;
    if (!set && !get)
        return -ENOTTY;
    if (set && !cred_has(cred_current(), PRIV_NET))
        return -EPERM;
    struct lx_ifreq r;
    if (copy_from_user(&r, arg, sizeof(r)))
        return -EFAULT;
    r.ifr_name[LX_IFNAMSIZ - 1] = '\0';
    struct netif_info f;
    bool found = req == LX_SIOCGIFNAME ? netif_info_by_index((unsigned)r.ifr_ivalue, &f)
                                       : netif_info_by_name(r.ifr_name, &f);
    if (!found)
        return -ENODEV;
    if (set)
        return set_one(req, &r, &f);
    switch (req) {
    case LX_SIOCGIFNAME:
        memset(r.ifr_name, 0, sizeof(r.ifr_name));
        strlcpy(r.ifr_name, f.name, sizeof(r.ifr_name));
        break;
    case LX_SIOCGIFINDEX:
        r.ifr_ivalue = (int32_t)f.index;
        break;
    case LX_SIOCGIFFLAGS:
        r.ifr_flags = lx_flags(&f);
        break;
    case LX_SIOCGIFMETRIC:
        r.ifr_ivalue = 0;   /* Linux's is always 0 */
        break;
    case LX_SIOCGIFMTU:
        r.ifr_ivalue = (int32_t)f.mtu;
        break;
    case LX_SIOCGIFHWADDR:
        memset(&r.ifr_hwaddr, 0, sizeof(r.ifr_hwaddr));
        r.ifr_hwaddr.family = (f.flags & NETIF_LOOPBACK) ? LX_ARPHRD_LOOPBACK : LX_ARPHRD_ETHER;
        memcpy(r.ifr_hwaddr.data, f.mac, sizeof(f.mac));
        break;
    case LX_SIOCGIFADDR:
    case LX_SIOCGIFNETMASK:
    case LX_SIOCGIFBRDADDR:
        if (f.addr == 0)
            return -EADDRNOTAVAIL;
        put_addr(&r, req == LX_SIOCGIFADDR ? f.addr : req == LX_SIOCGIFNETMASK ? f.mask : brd_of(&f));
        break;
    }
    return copy_to_user(arg, &r, sizeof(r)) ? -EFAULT : 0;
}
