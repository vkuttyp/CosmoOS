/* verify.h - Signature and checksum helpers (verify.c). */

#ifndef PKG_VERIFY_H
#define PKG_VERIFY_H

#include "pkg.h"

int ring_load(const char *dir);   /* keys loaded, or -1 when the directory is missing */
int trailer_split(const uint8_t *blob, size_t len, size_t *payload_len, const uint8_t **sig, const uint8_t **key_id,
                  char *err, size_t errlen);
bool ring_verify(const uint8_t *payload, size_t len, const uint8_t *sig, const uint8_t *key_id, char *err,
                 size_t errlen);
/* Split and verify in one step: 0 and *payload_len, or -1 with err. */
int verify_signed(const uint8_t *blob, size_t len, size_t *payload_len, char *err, size_t errlen);
void sha512_of(const void *data, size_t len, uint8_t out[SHA512_LEN]);

#endif
