/*
 * crc32c.h - CRC-32C (Castagnoli, polynomial 0x1EDC6F41), the integrity
 * checksum for on-disk metadata. Detection only; it is not authenticity
 * (constitution section 31). Table driven, any context.
 */

#ifndef KERNEL_CRC32C_H
#define KERNEL_CRC32C_H

#include <kernel/types.h>

/* Standard CRC-32C: init 0xFFFFFFFF, reflected, final xor. crc32c("123456789") == 0xE3069283. */
uint32_t crc32c(const void *data, size_t len);
/* Continue a checksum: pass the previous result as `seed` (raw, un-finalised form is handled internally). */
uint32_t crc32c_update(uint32_t crc, const void *data, size_t len);

#endif /* KERNEL_CRC32C_H */
