/* shim_fs.h - The memory image behind the host storage pool (tests/fuzz/shim_fs.c). */

#ifndef COSMO_FUZZ_SHIM_FS_H
#define COSMO_FUZZ_SHIM_FS_H

#include <stddef.h>
#include <stdint.h>

/* Replace the image: `nblocks` 4 KiB blocks, the first `size` bytes from `data`, the rest zero. */
void shim_image_set(const uint8_t *data, size_t size, uint64_t nblocks);
const uint8_t *shim_image_get(uint64_t *nblocks);

#endif
