/*
 * netparse_tcp.c - The TCP segment decoders (kernel/net/netparse.h), the C
 * that SA5's segment family replaces with netparse.cee once the recorded
 * digest (tests/host/test_netparse.c, "tcp digest") agrees. The checks are
 * tcp_input's and parse_mss's, moved here unchanged.
 */

#include <kernel/net/netparse.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t netparse_tcp(const uint8_t *hdr, uint32_t seg_len, struct netparse_tcp *out)
{
    uint32_t hlen = (uint32_t)(hdr[12] >> 4) * 4u;
    if (hlen < 20 || hlen > seg_len)
        return NETPARSE_BAD;
    out->hlen = hlen;
    out->sport = be16(hdr);
    out->dport = be16(hdr + 2);
    out->seq = be32(hdr + 4);
    out->ack = be32(hdr + 8);
    out->flags = hdr[13];
    out->win = be16(hdr + 14);
    return NETPARSE_OK;
}

uint16_t netparse_tcp_mss(const uint8_t *opts, uint32_t optlen, uint16_t dflt)
{
    uint32_t i = 0;
    while (i < optlen) {
        uint8_t kind = opts[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= optlen)
            break;
        uint8_t len = opts[i + 1];
        if (len < 2 || i + len > optlen)
            break;
        if (kind == 2 && len == 4) {
            uint16_t mss = be16(opts + i + 2);
            return mss < 536 ? 536 : mss;
        }
        i += len;
    }
    return dflt;
}
