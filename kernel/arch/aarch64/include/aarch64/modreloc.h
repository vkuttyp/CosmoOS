/*
 * aarch64/modreloc.h - R_AARCH64 relocation types and the pure patcher
 * (docs/kernel/arch/aarch64/design.md, "Modules"). The patcher works on a
 * buffer so tests/host/test_reloc_aarch64.c can check every encoding.
 */

#ifndef AARCH64_MODRELOC_H
#define AARCH64_MODRELOC_H

#include <stdint.h>

#define R_AARCH64_NONE               0
#define R_AARCH64_NONE_ALT           256
#define R_AARCH64_ABS64              257
#define R_AARCH64_ABS32              258
#define R_AARCH64_ABS16              259
#define R_AARCH64_PREL64             260
#define R_AARCH64_PREL32             261
#define R_AARCH64_PREL16             262
#define R_AARCH64_LD_PREL_LO19       273
#define R_AARCH64_ADR_PREL_LO21      274
#define R_AARCH64_ADR_PREL_PG_HI21   275
#define R_AARCH64_ADD_ABS_LO12_NC    277
#define R_AARCH64_LDST8_ABS_LO12_NC  278
#define R_AARCH64_TSTBR14            279
#define R_AARCH64_CONDBR19           280
#define R_AARCH64_JUMP26             282
#define R_AARCH64_CALL26             283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299

/* Apply one relocation of `type` at `where` (the bytes in memory) whose
 * address in the final image is P, for symbol value S and addend A.
 * 0, or -ENOEXEC (unknown type) / -ERANGE (out of range) with *why set. */
int aarch64_reloc_apply(uint32_t type, uint8_t *where, uint64_t P, uint64_t S, int64_t A, const char **why);

/* Bytes the relocation writes at its offset (4 or 8), 0 for none/unknown. */
unsigned aarch64_reloc_width(uint32_t type);

#endif /* AARCH64_MODRELOC_H */
