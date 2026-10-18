/*
 * fetch.h - A URL into a file, for the system's own programs (pkg,
 * cosmo-upgrade): runs /bin/cosmo-fetch, the HTTPS client on ssl_client's
 * certificate checks (ports/mbedtls/README.md, "cosmo-fetch"). The native
 * libc has no resolver and no TLS, so the transfer is that program's;
 * checking what arrived and renaming it into place are the caller's.
 */

#ifndef FETCH_H
#define FETCH_H

#include <stdbool.h>
#include <stdint.h>

#define FETCH_PROGRAM "/bin/cosmo-fetch"   /* or $COSMO_FETCH */

/* cosmo-fetch's exit statuses. */
#define FETCH_OK      0
#define FETCH_FAILED  1   /* DNS, connection, a body cut short (the file keeps what arrived) */
#define FETCH_REFUSED 3   /* certificate, pin, plain HTTP, a redirect elsewhere, too large */
#define FETCH_HTTP    4   /* the server answered with an HTTP error */
#define FETCH_LOCAL   5   /* the file could not be written: this system's fault, not the server's */

struct fetch_opts {
    const char *pin;   /* "sha256:HEX", or NULL */
    bool insecure;     /* http:// allowed */
    bool resume;       /* keep the file's bytes and ask for the rest */
    uint64_t max;      /* refuse a larger file; 0: no limit */
};

/* Fetch `url` into `path`. cosmo-fetch's status, its messages on standard
 * error; -1 with errno when it could not be run. */
int fetch_url(const char *url, const char *path, const struct fetch_opts *o);

#endif
