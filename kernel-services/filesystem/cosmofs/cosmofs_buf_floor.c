/*
 * cosmofs_buf_floor.c - The C under cosmofs_buf.cee (roadmap 2.0 SA7): the
 * metadata MAC, whose domain is a C char -- a parameter CeeSafe cannot
 * declare -- and whose tag is a keyed BLAKE2b over a struct blake2b on the
 * stack; and the header's and the superblock's seal and check under it,
 * for the port to call. The seals, the checks and the buffer cache are
 * CeeSafe (cosmofs_buf.cee, generated into cosmofs_buf.c).
 */

#include <kernel/blake2b.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

_Static_assert(offsetof(struct cfs_mhdr, crc) == 24 && offsetof(struct cfs_super, crc) == 160,
               "cosmofs_buf.cee seals a header and a superblock at these offsets");

/*
 * The version-14 tag (design.md, "The metadata MAC"): a 64-bit keyed
 * BLAKE2b over the domain byte and the block, with the eight bytes at
 * `off` -- where the CRC and the word after it were -- taken as zero.
 */
void cfs_mac_tag(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off, uint8_t tag[CFS_MAC_SIZE])
{
    static const uint8_t zero8[CFS_MAC_SIZE] = { 0 };
    uint8_t d = (uint8_t)domain;
    struct blake2b s;
    blake2b_init_key(&s, CFS_MAC_SIZE, m->key, sizeof(m->key));
    blake2b_update(&s, &d, 1);
    blake2b_update(&s, block, off);
    blake2b_update(&s, zero8, CFS_MAC_SIZE);
    blake2b_update(&s, block + off + CFS_MAC_SIZE, CFS_BLOCK - off - CFS_MAC_SIZE);
    blake2b_final(&s, tag);
}

/* Constant time: a comparison that stops early says how much of a
 * forgery was right. */
bool cfs_mac_ok(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off)
{
    uint8_t want[CFS_MAC_SIZE];
    cfs_mac_tag(m, domain, block, off, want);
    uint8_t diff = 0;
    for (unsigned i = 0; i < CFS_MAC_SIZE; i++)
        diff |= (uint8_t)(want[i] ^ block[off + i]);
    memset(want, 0, sizeof(want));
    return diff == 0;
}

void cfs_mac_seal(const struct cfs_mac *m, char domain, uint8_t *block, size_t off)
{
    uint8_t tag[CFS_MAC_SIZE];
    cfs_mac_tag(m, domain, block, off, tag);
    memcpy(block + off, tag, CFS_MAC_SIZE);
}

void cfs_buf_floor_seal_hdr(const struct cfs_mac *m, uint8_t *block);
bool cfs_buf_floor_hdr_ok(const struct cfs_mac *m, const uint8_t *block);
void cfs_buf_floor_seal_super(const struct cfs_mac *m, uint8_t *block);
bool cfs_buf_floor_super_ok(const struct cfs_mac *m, const uint8_t *block);

void cfs_buf_floor_seal_hdr(const struct cfs_mac *m, uint8_t *block)
{
    cfs_mac_seal(m, CFS_MAC_HDR, block, offsetof(struct cfs_mhdr, crc));
}

bool cfs_buf_floor_hdr_ok(const struct cfs_mac *m, const uint8_t *block)
{
    return cfs_mac_ok(m, CFS_MAC_HDR, block, offsetof(struct cfs_mhdr, crc));
}

void cfs_buf_floor_seal_super(const struct cfs_mac *m, uint8_t *block)
{
    cfs_mac_seal(m, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc));
}

bool cfs_buf_floor_super_ok(const struct cfs_mac *m, const uint8_t *block)
{
    return cfs_mac_ok(m, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc));
}
