/* The certificate checks ssl_client and cosmo-fetch share
 * (tls_common.h; ports/mbedtls/README.md). */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"
#include "tls_common.h"

/* The strong entropy source (cosmo_config.h, MBEDTLS_ENTROPY_HARDWARE_ALT):
 * getentropy(3), getrandom(2) on CosmoOS, which blocks until the kernel's
 * pool is seeded. At most 256 bytes a call. Declared in the library's
 * private entropy_poll.h. */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    while (*olen < len) {
        size_t n = len - *olen > 256 ? 256 : len - *olen;
        if (getentropy(out + *olen, n) != 0) {
            if (errno == EINTR)
                continue;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        *olen += n;
    }
    return 0;
}

void tls_strerror(int ret, char *buf, size_t len)
{
    char text[160];

    mbedtls_strerror(ret, text, sizeof(text));
    snprintf(buf, len, "%s (-0x%04x)", text, (unsigned)-ret);
}

int tls_client_setup(struct tls_client *c, const char *name, int no_check, const char **what)
{
    static char ca_what[300];
    const char *ca_file;
    int ret;

    *what = "PSA crypto";
    if (psa_crypto_init() != PSA_SUCCESS)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_init(&c->ssl);

    *what = "random generator";
    ret = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, (const unsigned char *)"cosmo-tls", 9);
    if (ret != 0)
        return ret;
    *what = "configuration";
    ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        return ret;
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    if (no_check) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        ca_file = getenv("SSL_CERT_FILE");
        if (!ca_file || !*ca_file)
            ca_file = TLS_CA_FILE_DEFAULT;
        /* A positive result counts certificates the file has and the
         * library cannot parse (an algorithm it lacks): the rest stay. */
        ret = mbedtls_x509_crt_parse_file(&c->ca, ca_file);
        if (ret < 0) {
            snprintf(ca_what, sizeof(ca_what), "the certificate authorities in %s", ca_file);
            *what = ca_what;
            return ret;
        }
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    }

    *what = "setup";
    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0)
        return ret;
    /* The name checked against the certificate, and the one sent as SNI. */
    *what = "server name";
    if (name && (ret = mbedtls_ssl_set_hostname(&c->ssl, name)) != 0)
        return ret;
    return 0;
}

void tls_report_verify(const struct tls_client *c, const char *prog, const char *name)
{
    char info[512];

    mbedtls_x509_crt_verify_info(info, sizeof(info), "  ", mbedtls_ssl_get_verify_result(&c->ssl));
    fprintf(stderr, "%s: %s: the server's certificate is not accepted:\n%s", prog, name, info);
}

int tls_peer_key_sha256(const struct tls_client *c, char hex[65])
{
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&c->ssl);
    unsigned char der[2048], sum[32];
    int n;

    if (crt == NULL)
        return -1;
    /* Written at the end of the buffer; n bytes long. */
    n = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&crt->pk, der, sizeof(der));
    if (n <= 0)
        return -1;
    if (mbedtls_sha256(der + sizeof(der) - n, (size_t)n, sum, 0) != 0)
        return -1;
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    return 0;
}
