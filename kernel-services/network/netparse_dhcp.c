/*
 * netparse_dhcp.c - The DHCP and DNS decoders (kernel/net/netparse.h), the C
 * that SA5's DHCP-and-DNS family replaces with netparse.cee once the
 * recorded digest (tests/host/test_netparse.c, "dhcp digest") agrees. The
 * checks are dhcp.h's dhcp_opt, tapsvc.c's dhcp_filter and dns_parse_ip,
 * and dhcp.c's dhcp_client_input, bind_lease and take_hostname, moved here
 * unchanged.
 */

#include <kernel/net/dhcp.h>
#include <kernel/net/netparse.h>
#include <kernel/string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int32_t netparse_dhcp_opt(const uint8_t *opts, uint32_t len, uint8_t code, uint8_t *olen)
{
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = opts[i++];
        if (c == OPT_END)
            break;
        if (c == OPT_PAD)
            continue;
        if (i >= len)
            break;
        uint8_t l = opts[i++];
        if (i + l > len)
            break;
        if (c == code) {
            *olen = l;
            return (int32_t)i;
        }
        i += l;
    }
    return -1;
}

/* Option `code` of exactly `want` bytes, copied to `out`. */
static bool opt_exact(const uint8_t *opts, uint32_t optlen, uint8_t code, uint8_t want, uint8_t *out)
{
    uint8_t l;
    int32_t at = netparse_dhcp_opt(opts, optlen, code, &l);
    if (at < 0 || l != want)
        return false;
    memcpy(out, opts + at, want);
    return true;
}

uint32_t netparse_dhcp(const uint8_t *dh, uint32_t dhlen, uint8_t op, struct netparse_dhcp *out)
{
    if (dhlen < DHCP_FIXED)
        return NETPARSE_BAD;
    if (dh[BOOTP_OFF_OP] != op || dh[BOOTP_OFF_HTYPE] != DHCP_HTYPE_ETHER || dh[BOOTP_OFF_HLEN] != 6 ||
        be32(dh + BOOTP_LEN) != DHCP_MAGIC)
        return NETPARSE_BAD;
    const uint8_t *opts = dh + DHCP_FIXED;
    uint32_t optlen = dhlen - DHCP_FIXED;
    uint8_t type;
    if (!opt_exact(opts, optlen, OPT_MSGTYPE, 1, &type))
        return NETPARSE_BAD;
    out->msg_type = type;
    memcpy(out->xid, dh + BOOTP_OFF_XID, 4);
    out->flags = be16(dh + BOOTP_OFF_FLAGS);
    memcpy(out->ciaddr, dh + BOOTP_OFF_CIADDR, 4);
    memcpy(out->yiaddr, dh + BOOTP_OFF_YIADDR, 4);
    memcpy(out->chaddr, dh + BOOTP_OFF_CHADDR, 6);
    out->has_server = opt_exact(opts, optlen, OPT_SERVERID, 4, out->server);
    if (!out->has_server)
        memset(out->server, 0, 4);
    out->has_reqip = opt_exact(opts, optlen, OPT_REQIP, 4, out->reqip);
    if (!out->has_reqip)
        memset(out->reqip, 0, 4);
    return NETPARSE_OK;
}

/* Option 12, kept only when it is a name sethostname and a file can take
 * as it is: 1-63 letters, digits, '-' and '.', no NUL (RFC 1123's host name
 * characters; RFC 2132 allows a trailing NUL, which is dropped). */
static void take_hostname(const uint8_t *v, uint8_t l, uint8_t *name)
{
    name[0] = '\0';
    if (l > 0 && v[l - 1] == '\0')
        l--;
    if (l == 0 || l >= DHCP_HOSTNAME_MAX)
        return;
    for (unsigned i = 0; i < l; i++) {
        uint8_t ch = v[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                  ch == '.';
        if (!ok)
            return;
    }
    memcpy(name, v, l);
    name[l] = '\0';
}

bool netparse_dhcp_lease(const uint8_t *dh, uint32_t dhlen, struct netparse_dhcp_lease *out)
{
    const uint8_t *opts = dh + DHCP_FIXED;
    uint32_t optlen = dhlen - DHCP_FIXED;
    memset(out, 0, sizeof(*out));
    memcpy(out->addr, dh + BOOTP_OFF_YIADDR, 4);
    uint32_t yi = be32(out->addr);
    uint8_t lease[4];
    if (yi == 0 || yi == 0xffffffffu || !opt_exact(opts, optlen, OPT_LEASE, 4, lease))
        return false;   /* an ACK to a REQUEST must carry the lease time (RFC 2131 table 3) */
    out->lease = be32(lease);
    if (out->lease == 0)
        return false;
    out->has_mask = opt_exact(opts, optlen, OPT_SUBNET, 4, out->mask);
    uint8_t l;
    int32_t at = netparse_dhcp_opt(opts, optlen, OPT_ROUTER, &l);
    if (at >= 0 && l >= 4) {
        memcpy(out->router, opts + at, 4);   /* the first router */
        out->has_router = true;
    }
    at = netparse_dhcp_opt(opts, optlen, OPT_DNS, &l);
    if (at >= 0)
        for (unsigned i = 0; i + 4 <= l && out->ndns < DHCP_MAX_DNS; i += 4)
            memcpy(out->dns + 4 * out->ndns++, opts + at + i, 4);
    at = netparse_dhcp_opt(opts, optlen, OPT_HOSTNAME, &l);
    if (at >= 0)
        take_hostname(opts + at, l, out->hostname);
    uint8_t t[4];
    if (opt_exact(opts, optlen, OPT_T1, 4, t))
        out->t1 = be32(t);
    if (opt_exact(opts, optlen, OPT_T2, 4, t))
        out->t2 = be32(t);
    return true;
}

bool netparse_dhcp_frame(const uint8_t *f, uint32_t len, uint32_t *dh_off)
{
    if (len < 14 + 20 + 8)
        return false;
    if (!(f[12] == 0x08 && f[13] == 0x00))   /* IPv4 */
        return false;
    uint32_t ihl = (uint32_t)(f[14] & 0xf) * 4u;
    if ((f[14] >> 4) != 4 || ihl < 20 || f[14 + 9] != 17)   /* UDP */
        return false;
    if (14 + ihl + 8 > len)
        return false;
    if (be16(f + 14 + ihl + 2) != DHCP_SPORT)
        return false;   /* not DHCP-to-server: the stack's */
    *dh_off = 14 + ihl + 8;
    return true;
}

uint16_t netparse_dns_id(const uint8_t *msg, uint32_t len)
{
    (void)len;
    return be16(msg);
}

bool netparse_ipv4_text(const char *s, uint32_t len, uint8_t *out)
{
    uint32_t o[4] = { 0 }, v = 0;
    int part = 0, digits = 0;
    for (uint32_t i = 0;; i++) {
        char c = i < len ? s[i] : '\0';
        if (c >= '0' && c <= '9') {
            v = v * 10 + (uint32_t)(c - '0');
            digits++;
            if (v > 255)
                return false;
        } else if (c == '.' || c == '\0') {
            if (!digits || part > 3)
                return false;
            o[part++] = v;
            v = 0;
            digits = 0;
            if (c == '\0')
                break;
        } else {
            return false;
        }
    }
    if (part != 4)
        return false;
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)o[i];
    return true;
}
