/*
 * fskey -- an encrypted pool's key through /dev/fsctl's KEY_* operations
 * (roadmap 1.2 B5; docs/kernel-services/filesystem/cosmofs/api.md): for
 * cosmo-key, which unlocks a pool and changes its slots, and cosmo-install,
 * which asks whether a pool is one.
 *
 * Every call returns 0, or -1 with errno set from the kernel's answer.
 * A pool is named by its device (`dev`), or by a mount's id when `dev` is
 * NULL. A command that carries the master key is wiped once written.
 */
#ifndef FSKEY_H
#define FSKEY_H

#include <stdint.h>

#include <uapi/cosmo/fsctl.h>

int fskey_info(const char *dev, uint64_t mount_id, struct cosmo_fsctl_keyinfo *out);
int fskey_load(const char *dev, const uint8_t master[COSMO_KEY_SIZE]);
int fskey_unload(const char *dev);
int fskey_slot(uint64_t mount_id, unsigned slot, const uint8_t master[COSMO_KEY_SIZE], const struct cosmo_keyslot *rec);

#endif
