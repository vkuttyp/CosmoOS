/* mbedTLS's configuration for CosmoOS (ports/mbedtls/README.md): the
 * library's default, mbedtls_config.h, with these changes. Included after
 * it through MBEDTLS_USER_CONFIG_FILE. */

/* Randomness from getrandom(2) (ssl_client.c, mbedtls_hardware_poll), a
 * strong source for every entropy context, PSA's own included. The
 * platform source is off: on a libc other than glibc it reads
 * /dev/urandom, which CosmoOS does not have. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* A client only. */
#undef MBEDTLS_SSL_SRV_C
