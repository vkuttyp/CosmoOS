/* The certificate checks ssl_client and cosmo-fetch share
 * (ports/mbedtls/README.md): randomness, the certificate authorities, and
 * the report of a certificate that is not accepted. */
#ifndef TLS_COMMON_H
#define TLS_COMMON_H

#include <stddef.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define TLS_CA_FILE_DEFAULT "/etc/ssl/certs/ca-certificates.crt"

/* Everything one client connection holds. */
struct tls_client {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
};

/* mbedtls_strerror's text for `ret`, with the code, into `buf`. */
void tls_strerror(int ret, char *buf, size_t len);

/* Set `c` up as a client of `name` (checked against the certificate and
 * sent as SNI; may be NULL only with no_check). Unless no_check, the
 * server's certificate must chain to one in the file SSL_CERT_FILE names,
 * TLS_CA_FILE_DEFAULT by default, be valid at the wall clock's time, and
 * name `name`. 0, or an mbedTLS error with `what` naming the step. */
int tls_client_setup(struct tls_client *c, const char *name, int no_check, const char **what);

/* Print "PROG: NAME: the server's certificate is not accepted:" and
 * mbedTLS's reasons, one per line, to standard error. */
void tls_report_verify(const struct tls_client *c, const char *prog, const char *name);

/* The SHA-256 of the server certificate's public key (its DER
 * SubjectPublicKeyInfo, as `openssl pkey -pubin -outform der | sha256sum`
 * computes it), as 64 lowercase hex digits. 0, or -1 with no peer
 * certificate. */
int tls_peer_key_sha256(const struct tls_client *c, char hex[65]);

#endif
