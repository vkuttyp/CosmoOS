/*
 * cosmofs_crypt_floor.c - The C under cosmofs_crypt.cee (roadmap 2.0 SA7):
 * the one call whose prototype CeeSafe cannot write, cfs_mac_ok, whose
 * domain is a C char, and the checks that a /dev/fsctl key slot is an
 * on-disk one. The keys are CeeSafe (cosmofs_crypt.cee, generated into
 * cosmofs_crypt.c).
 */

#include <uapi/cosmo/fsctl.h>

#include "cosmofs_internal.h"

_Static_assert(sizeof(struct cosmo_keyslot) == sizeof(struct cfs_keyslot), "a uapi slot is an on-disk slot");
_Static_assert(offsetof(struct cosmo_keyslot, wrapped) == offsetof(struct cfs_keyslot, wrapped), "slot layout");
_Static_assert(offsetof(struct cosmo_keyslot, created_ns) == offsetof(struct cfs_keyslot, created_ns), "slot layout");
_Static_assert(COSMO_KEY_SLOTS == CFS_KEY_SLOTS && COSMO_KEY_SIZE == CHACHA20_KEY_SIZE, "slot count, key size");

bool cfs_crypt_floor_super_ok(const struct cfs_mac *m, const uint8_t *block);

bool cfs_crypt_floor_super_ok(const struct cfs_mac *m, const uint8_t *block)
{
    return cfs_mac_ok(m, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc));
}
