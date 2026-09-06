/*
 * cksum.c - RFC 1071 Internet checksum.
 */

#include <kernel/net/cksum.h>
#include <kernel/string.h>

uint32_t cksum_partial(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t cksum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    uint16_t host = (uint16_t)~sum;
    return htons(host);   /* stored in network byte order */
}

uint16_t in_cksum(const void *data, size_t len)
{
    return cksum_fold(cksum_partial(data, len, 0));
}

/* Byte-parity aware chain sum: an odd buffer length shifts the pairing. */
uint32_t m_cksum_partial(const struct mbuf *m, uint32_t off, uint32_t len, uint32_t sum)
{
    for (; m && off >= m->len; m = m->next)
        off -= m->len;
    bool odd = false;
    while (len > 0 && m) {
        uint32_t take = m->len - off < len ? m->len - off : len;
        const uint8_t *p = m->data + off;
        uint32_t i = 0;
        if (odd && take > 0) {
            sum += p[0];
            i = 1;
            odd = false;
        }
        for (; i + 1 < take; i += 2)
            sum += (uint32_t)((p[i] << 8) | p[i + 1]);
        if (i < take) {
            sum += (uint32_t)(p[i] << 8);
            odd = true;
        }
        len -= take;
        off = 0;
        m = m->next;
    }
    return sum;
}

uint32_t cksum_pseudo4(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    uint8_t ph[12];
    memcpy(ph, &src, 4);
    memcpy(ph + 4, &dst, 4);
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = (uint8_t)(len >> 8);
    ph[11] = (uint8_t)len;
    return cksum_partial(ph, sizeof(ph), 0);
}

uint32_t cksum_pseudo6(const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto, uint32_t len)
{
    uint8_t ph[40];
    memcpy(ph, src->s6_addr, 16);
    memcpy(ph + 16, dst->s6_addr, 16);
    ph[32] = (uint8_t)(len >> 24);
    ph[33] = (uint8_t)(len >> 16);
    ph[34] = (uint8_t)(len >> 8);
    ph[35] = (uint8_t)len;
    ph[36] = ph[37] = ph[38] = 0;
    ph[39] = proto;
    return cksum_partial(ph, sizeof(ph), 0);
}

bool m_csum_complete(struct mbuf *m)
{
    uint32_t start = m->pkt.csum_start, field = start + m->pkt.csum_offset;
    if (start >= m->pkt.len || field + 2 > m->len)
        return false;
    uint16_t c = cksum_fold(m_cksum_partial(m, start, m->pkt.len - start, 0));
    if ((m->pkt.csum_flags & NET_CSUM_UDP) && c == 0)
        c = 0xffff;
    memcpy(m->data + field, &c, sizeof(c));
    m->pkt.csum_flags &= (uint16_t)~NET_CSUM_TX;
    return true;
}

/* Module ABI exports (docs/kernel/module/api.md): the virtio-net module
 * finishes NEEDS_CSUM frames with m_csum_complete (unit 11). */
#include <kernel/module.h>
EXPORT_SYMBOL(m_csum_complete);
