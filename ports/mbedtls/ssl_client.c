/* ssl_client: TLS for BusyBox wget, with the server's certificate checked
 * (ports/mbedtls/README.md).
 *
 *     ssl_client [-e] [-k] -s FD [-r FD] [-n NAME]
 *
 * BusyBox's own ssl_client's interface, which wget runs it with
 * (ports/busybox/wget-ssl-client.patch): TLS over the connected socket FD
 * (-r: a separate one to read), plaintext on standard input and output.
 * What arrives on standard input is sent; what the server sends is written
 * to standard output. -e: end at end of input (wget's FTPS control
 * connection). The end of the server's data ends the program.
 *
 * The server's certificate must chain to a certificate in the file
 * SSL_CERT_FILE names, /etc/ssl/certs/ca-certificates.crt by default, be
 * valid at the wall clock's time, and name NAME (a DNS name, or an
 * address in the certificate's IP addresses). -k (wget's
 * --no-check-certificate) skips all three, saying so. Any failure is
 * reported on standard error and the status is 1. */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define CA_FILE_DEFAULT "/etc/ssl/certs/ca-certificates.crt"

static int g_rfd = -1, g_wfd = -1;

/* The strong entropy source (cosmo_config.h, MBEDTLS_ENTROPY_HARDWARE_ALT):
 * getrandom(2), which blocks until the kernel's pool is seeded. Declared
 * in the library's private entropy_poll.h. */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    while (*olen < len) {
        ssize_t n = getrandom(out + *olen, len - *olen, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        *olen += (size_t)n;
    }
    return 0;
}

static void die(const char *what, int ret)
{
    char buf[160];

    mbedtls_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "ssl_client: %s: %s (-0x%04x)\n", what, buf, (unsigned)-ret);
    exit(1);
}

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    for (;;) {
        ssize_t n = write(g_wfd, buf, len);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return errno == EPIPE || errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
    }
}

static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    for (;;) {
        ssize_t n = read(g_rfd, buf, len);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

static void write_all(int fd, const unsigned char *p, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            exit(errno == EPIPE ? 0 : 1);   /* the reader went away */
        }
        p += n;
        len -= (size_t)n;
    }
}

static void ssl_write_all(mbedtls_ssl_context *ssl, const unsigned char *p, size_t len)
{
    while (len > 0) {
        int ret = mbedtls_ssl_write(ssl, p, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret < 0)
            die("send", ret);
        p += ret;
        len -= (size_t)ret;
    }
}

/* Whether the server's data has ended: 1 at its end, 0 otherwise. */
static int relay_from_server(mbedtls_ssl_context *ssl)
{
    unsigned char buf[16384];
    int ret = mbedtls_ssl_read(ssl, buf, sizeof(buf));

    if (ret > 0) {
        write_all(1, buf, (size_t)ret);
        return 0;
    }
    switch (ret) {
    case MBEDTLS_ERR_SSL_WANT_READ:
    case MBEDTLS_ERR_SSL_WANT_WRITE:
    case MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET:
        return 0;
    case 0:
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
    /* Closed without close_notify, as many HTTP servers do: the end, as
     * for BusyBox's ssl_client; HTTP's own length says whether it was
     * all of it. */
    case MBEDTLS_ERR_SSL_CONN_EOF:
    case MBEDTLS_ERR_NET_CONN_RESET:
        return 1;
    }
    die("receive", ret);
    return 1;
}

static void copy_loop(mbedtls_ssl_context *ssl, int exit_on_eof)
{
    unsigned char buf[16384];
    int in_open = 1;

    for (;;) {
        struct pollfd p[2] = {
            { .fd = g_rfd, .events = POLLIN },
            { .fd = in_open ? 0 : -1, .events = POLLIN },
        };

        /* Data already read from the socket and not yet returned: poll
         * would not report it. */
        if (mbedtls_ssl_check_pending(ssl)) {
            if (relay_from_server(ssl))
                return;
            continue;
        }
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("ssl_client: poll");
            exit(1);
        }
        if (p[1].revents) {
            ssize_t n = read(0, buf, sizeof(buf));
            if (n < 0 && errno != EINTR) {
                perror("ssl_client: read");
                exit(1);
            }
            if (n == 0) {
                if (exit_on_eof) {
                    mbedtls_ssl_close_notify(ssl);
                    return;
                }
                in_open = 0;
            }
            if (n > 0)
                ssl_write_all(ssl, buf, (size_t)n);
        }
        if (p[0].revents && relay_from_server(ssl))
            return;
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: ssl_client [-e] [-k] -s FD [-r FD] [-n NAME]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *name = NULL, *ca_file;
    int exit_on_eof = 0, no_check = 0, c, ret;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;

    while ((c = getopt(argc, argv, "eks:r:n:")) != -1) {
        switch (c) {
        case 'e': exit_on_eof = 1; break;
        case 'k': no_check = 1; break;
        case 's': g_wfd = atoi(optarg); break;
        case 'r': g_rfd = atoi(optarg); break;
        case 'n': name = optarg; break;
        default: usage();
        }
    }
    if (g_wfd < 0 || optind != argc)
        usage();
    if (g_rfd < 0)
        g_rfd = g_wfd;
    if (!name && !no_check) {
        fprintf(stderr, "ssl_client: no server name (-n) to check the certificate against\n");
        exit(1);
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        fprintf(stderr, "ssl_client: PSA crypto did not initialise\n");
        exit(1);
    }
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);

    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)"ssl_client", 10);
    if (ret != 0)
        die("random generator", ret);

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        die("configuration", ret);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

    if (no_check) {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        ca_file = getenv("SSL_CERT_FILE");
        if (!ca_file || !*ca_file)
            ca_file = CA_FILE_DEFAULT;
        /* A positive result counts certificates the file has and the
         * library cannot parse (an algorithm it lacks): the rest stay. */
        ret = mbedtls_x509_crt_parse_file(&ca, ca_file);
        if (ret < 0) {
            char what[256];
            snprintf(what, sizeof(what), "the certificate authorities in %s", ca_file);
            die(what, ret);
        }
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0)
        die("setup", ret);
    /* The name checked against the certificate, and the one sent as SNI. */
    if (name && (ret = mbedtls_ssl_set_hostname(&ssl, name)) != 0)
        die("server name", ret);
    mbedtls_ssl_set_bio(&ssl, NULL, net_send, net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char info[512];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "  ", mbedtls_ssl_get_verify_result(&ssl));
            fprintf(stderr, "ssl_client: %s: the server's certificate is not accepted:\n%s", name, info);
            exit(1);
        }
        die("handshake", ret);
    }
    if (no_check)
        fprintf(stderr, "ssl_client: %s: the server's certificate was not checked (-k)\n",
                name ? name : "the server");

    copy_loop(&ssl, exit_on_eof);
    return 0;
}
