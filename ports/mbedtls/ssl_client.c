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
 * reported on standard error and the status is 1. The checks are
 * tls_common.c's, which cosmo-fetch shares. */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/net_sockets.h"
#include "tls_common.h"

static int g_rfd = -1, g_wfd = -1;

static void die(const char *what, int ret)
{
    char buf[200];

    tls_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "ssl_client: %s: %s\n", what, buf);
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
    const char *name = NULL, *what;
    int exit_on_eof = 0, no_check = 0, c, ret;
    static struct tls_client tls;

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

    /* The checks cosmo-fetch makes too (tls_common.c). */
    ret = tls_client_setup(&tls, name, no_check, &what);
    if (ret != 0)
        die(what, ret);
    mbedtls_ssl_set_bio(&tls.ssl, NULL, net_send, net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&tls.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            tls_report_verify(&tls, "ssl_client", name);
            exit(1);
        }
        die("handshake", ret);
    }
    if (no_check)
        fprintf(stderr, "ssl_client: %s: the server's certificate was not checked (-k)\n",
                name ? name : "the server");

    copy_loop(&tls.ssl, exit_on_eof);
    return 0;
}
