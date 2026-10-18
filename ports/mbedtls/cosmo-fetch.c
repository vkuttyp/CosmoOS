/* cosmo-fetch: one file over HTTPS, with the server's certificate checked
 * as ssl_client checks it (tls_common.c), for pkg and cosmo-upgrade
 * (ports/mbedtls/README.md, "cosmo-fetch"; docs/pkg/design.md, "Network
 * repositories").
 *
 *     cosmo-fetch [-c] [-i] [-p sha256:HEX] [-m MAX] URL FILE
 *
 * GET URL into FILE. https:// always; http:// only with -i (a repository
 * marked insecure), refused before any connection otherwise. Before the
 * request is sent the server's certificate must chain to one in
 * SSL_CERT_FILE (/etc/ssl/certs/ca-certificates.crt by default), be valid
 * at the wall clock's time and name the URL's host; with -p, the SHA-256 of
 * its public key (DER SubjectPublicKeyInfo) must also be HEX. A redirect is
 * followed (at most 5) only to the same scheme, host and port. -c resumes:
 * FILE's bytes are kept and the rest asked for by range; a server that
 * answers with the whole file starts it again. -m refuses a file larger
 * than MAX bytes. FILE is not renamed or checked here: the caller checks
 * it against what it expects and renames it into place.
 *
 * Exit 0 fetched; 1 failed (DNS, connection, a body cut short: FILE keeps
 * what arrived, for -c); 2 usage; 3 refused (the certificate, the pin,
 * plain HTTP without -i, a redirect elsewhere, larger than MAX: FILE is
 * removed if this run wrote it); 4 the server answered with an HTTP error. */

#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/net_sockets.h"
#include "tls_common.h"

enum { FX_FETCHED = 0, FX_FAILED = 1, FX_USAGE = 2, FX_REFUSED = 3, FX_HTTP = 4 };

#define TIMEOUT_MS   30000
#define MAX_REDIRECT 5
#define LINE_MAX_LEN 8192

struct url {
    int https;
    char host[256];
    char port[8];
    int explicit_port;
    char path[2048];
};

struct conn {
    int fd;
    int tls_on;
    struct tls_client tls;
    unsigned char buf[16384];
    size_t pos, len;
    int eof;
    int reset;   /* the end was a reset, not a close */
};

static const char *g_url;
static const char *g_file;
static int g_wrote_file;   /* this run created or truncated FILE */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "cosmo-fetch: %s: ", g_url);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void refuse_exit(void)
{
    if (g_wrote_file)
        unlink(g_file);
    exit(FX_REFUSED);
}

/* --- URLs ------------------------------------------------------------------------- */

static int url_parse(const char *text, struct url *u)
{
    const char *p, *hend, *pstart;
    size_t n;

    memset(u, 0, sizeof(*u));
    if (strncmp(text, "https://", 8) == 0) {
        u->https = 1;
        p = text + 8;
    } else if (strncmp(text, "http://", 7) == 0) {
        p = text + 7;
    } else {
        return -1;
    }
    pstart = p + strcspn(p, "/?#");
    if (memchr(p, '@', (size_t)(pstart - p)))
        return -1;   /* no user information */
    if (*p == '[') {
        hend = memchr(p, ']', (size_t)(pstart - p));
        if (hend == NULL)
            return -1;
        n = (size_t)(hend - p - 1);
        if (n == 0 || n >= sizeof(u->host))
            return -1;
        memcpy(u->host, p + 1, n);
        hend++;
    } else {
        hend = memchr(p, ':', (size_t)(pstart - p));
        if (hend == NULL)
            hend = pstart;
        n = (size_t)(hend - p);
        if (n == 0 || n >= sizeof(u->host))
            return -1;
        memcpy(u->host, p, n);
    }
    for (char *h = u->host; *h; h++)
        *h = (char)tolower((unsigned char)*h);
    if (hend < pstart) {
        if (*hend != ':' || pstart - hend - 1 < 1 || pstart - hend - 1 > 5)
            return -1;
        memcpy(u->port, hend + 1, (size_t)(pstart - hend - 1));
        for (char *d = u->port; *d; d++)
            if (!isdigit((unsigned char)*d))
                return -1;
        u->explicit_port = 1;
    } else {
        strcpy(u->port, u->https ? "443" : "80");
    }
    if (*pstart == '\0')
        strcpy(u->path, "/");
    else if (*pstart == '/')
        snprintf(u->path, sizeof(u->path), "%.*s", (int)strcspn(pstart, "#"), pstart);
    else
        snprintf(u->path, sizeof(u->path), "/%.*s", (int)strcspn(pstart, "#"), pstart);
    for (const char *c = u->path; *c; c++)
        if ((unsigned char)*c <= ' ' || *c == 0x7f)
            return -1;
    return 0;
}

static int same_origin(const struct url *a, const struct url *b)
{
    return a->https == b->https && strcmp(a->host, b->host) == 0 && strcmp(a->port, b->port) == 0;
}

/* "scheme://host:port" of `u`, the host bracketed when it is IPv6. */
static void origin_text(const struct url *u, char *out, size_t cap)
{
    int v6 = strchr(u->host, ':') != NULL;
    snprintf(out, cap, "%s://%s%s%s:%s", u->https ? "https" : "http", v6 ? "[" : "", u->host, v6 ? "]" : "", u->port);
}

/* Where a Location header points, from `base`. */
static int url_resolve(const struct url *base, const char *loc, struct url *out)
{
    char origin[300], text[2400];

    if (strncmp(loc, "https://", 8) == 0 || strncmp(loc, "http://", 7) == 0)
        return url_parse(loc, out);
    origin_text(base, origin, sizeof(origin));
    if (loc[0] == '/' && loc[1] == '/') {
        snprintf(text, sizeof(text), "%s:%s", base->https ? "https" : "http", loc);
    } else if (loc[0] == '/') {
        snprintf(text, sizeof(text), "%s%s", origin, loc);
    } else if (loc[0] == '#') {   /* the same resource */
        snprintf(text, sizeof(text), "%s%s", origin, base->path);
    } else if (loc[0] == '?') {   /* the same path, another query */
        snprintf(text, sizeof(text), "%s%.*s%s", origin, (int)strcspn(base->path, "?"), base->path, loc);
    } else {
        char path[2048];
        snprintf(path, sizeof(path), "%.*s", (int)strcspn(base->path, "?"), base->path);
        int dir = (int)(strrchr(path, '/') - path + 1);
        snprintf(text, sizeof(text), "%s%.*s%s", origin, dir, path, loc);
    }
    return url_parse(text, out);
}

/* --- the connection --------------------------------------------------------------- */

static int wait_fd(int fd, short events)
{
    for (;;) {
        struct pollfd p = { .fd = fd, .events = events };
        int r = poll(&p, 1, TIMEOUT_MS);
        if (r > 0)
            return 0;
        if (r == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
    }
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = write(fd, buf, len);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return errno == EPIPE || errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
    }
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    if (wait_fd(fd, POLLIN) < 0)
        return errno == ETIMEDOUT ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n >= 0)
            return (int)n;
        if (errno == EINTR)
            continue;
        return errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

static int tcp_connect(const struct url *u)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res, *ai;
    int fd = -1, err, saved = 0;

    err = getaddrinfo(u->host, u->port, &hints, &res);
    if (err != 0) {
        say("cannot resolve %s: %s", u->host, gai_strerror(err));
        exit(FX_FAILED);
    }
    /* Each address within TIMEOUT_MS: nonblocking, then blocking again
     * once connected. */
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        int fl = fcntl(fd, F_GETFL);
        int rc = fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (wait_fd(fd, POLLOUT) < 0)
                soerr = errno;
            else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
                soerr = errno;
            rc = soerr ? -1 : 0;
            errno = soerr;
        }
        if (rc == 0 && fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) == 0)
            break;
        saved = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        say("cannot connect to %s port %s: %s", u->host, u->port, strerror(saved));
        exit(FX_FAILED);
    }
    return fd;
}

/* Connected, and for https the server's certificate (and pin) accepted:
 * nothing is sent before this returns. */
static void conn_open(struct conn *c, const struct url *u, const char *pin)
{
    const char *what;
    char tbuf[200];
    int ret;

    memset(c, 0, sizeof(*c));
    c->fd = tcp_connect(u);
    if (!u->https)
        return;
    c->tls_on = 1;
    ret = tls_client_setup(&c->tls, u->host, 0, &what);
    if (ret != 0) {
        tls_strerror(ret, tbuf, sizeof(tbuf));
        say("%s: %s", what, tbuf);
        exit(FX_FAILED);
    }
    mbedtls_ssl_set_bio(&c->tls.ssl, &c->fd, bio_send, bio_recv, NULL);
    while ((ret = mbedtls_ssl_handshake(&c->tls.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            tls_report_verify(&c->tls, "cosmo-fetch", u->host);
            refuse_exit();
        }
        tls_strerror(ret, tbuf, sizeof(tbuf));
        say("TLS handshake with %s: %s", u->host, tbuf);
        exit(FX_FAILED);
    }
    if (pin) {
        char got[65];
        if (tls_peer_key_sha256(&c->tls, got) != 0) {
            say("%s: no server key to compare with the pin", u->host);
            refuse_exit();
        }
        if (strcmp(got, pin) != 0) {
            say("%s: the server's key is sha256:%s, not the pinned sha256:%s", u->host, got, pin);
            refuse_exit();
        }
    }
}

static void conn_close(struct conn *c)
{
    if (c->tls_on) {
        mbedtls_ssl_free(&c->tls.ssl);
        mbedtls_ssl_config_free(&c->tls.conf);
        mbedtls_x509_crt_free(&c->tls.ca);
        mbedtls_ctr_drbg_free(&c->tls.drbg);
        mbedtls_entropy_free(&c->tls.entropy);
    }
    close(c->fd);
}

static void conn_write(struct conn *c, const char *p, size_t len)
{
    while (len > 0) {
        int n;
        if (c->tls_on) {
            n = mbedtls_ssl_write(&c->tls.ssl, (const unsigned char *)p, len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
        } else {
            n = bio_send(&c->fd, (const unsigned char *)p, len);
        }
        if (n < 0) {
            say("sending the request failed");
            exit(FX_FAILED);
        }
        p += n;
        len -= (size_t)n;
    }
}

/* Refill the buffer: 0, or -1 at the end of the server's data. */
static int conn_fill(struct conn *c)
{
    int n;

    if (c->eof)
        return -1;
    for (;;) {
        if (c->tls_on) {
            n = mbedtls_ssl_read(&c->tls.ssl, c->buf, sizeof(c->buf));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
                n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;
            /* Closed without close_notify, as many HTTP servers do: the
             * end, as for ssl_client; HTTP's own length says whether it
             * was all of it. */
            if (n == MBEDTLS_ERR_NET_CONN_RESET)
                c->reset = 1;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF ||
                n == MBEDTLS_ERR_NET_CONN_RESET)
                n = 0;
        } else {
            n = bio_recv(&c->fd, c->buf, sizeof(c->buf));
            if (n == MBEDTLS_ERR_NET_CONN_RESET) {
                c->reset = 1;
                n = 0;
            }
        }
        break;
    }
    if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
        say("no data from the server for %d s", TIMEOUT_MS / 1000);
        exit(FX_FAILED);
    }
    if (n < 0) {
        char tbuf[200];
        tls_strerror(n, tbuf, sizeof(tbuf));
        say("receiving: %s", tbuf);
        exit(FX_FAILED);
    }
    if (n == 0) {
        c->eof = 1;
        return -1;
    }
    c->pos = 0;
    c->len = (size_t)n;
    return 0;
}

/* One line without its CRLF; -1 at the end of the data, including inside
 * a line (an unterminated line is not one). */
static int conn_line(struct conn *c, char *line, size_t cap)
{
    size_t n = 0;

    for (;;) {
        if (c->pos == c->len && conn_fill(c) < 0) {
            line[0] = '\0';
            return -1;
        }
        char ch = (char)c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (n + 1 >= cap) {
            say("a header line longer than %zu bytes", cap);
            exit(FX_FAILED);
        }
        line[n++] = ch;
    }
    if (n && line[n - 1] == '\r')
        n--;
    line[n] = '\0';
    return 0;
}

/* --- the body --------------------------------------------------------------------- */

static int g_fd = -1;
static uint64_t g_have;   /* bytes in FILE */
static uint64_t g_max;    /* 0: no limit */

static void too_large(void)
{
    say("larger than %llu bytes: refused", (unsigned long long)g_max);
    g_wrote_file = 1;   /* what it wrote is not the file asked for */
    refuse_exit();
}

static void body_put(const unsigned char *p, size_t len)
{
    if (g_max && (g_have > g_max || len > g_max - g_have))
        too_large();
    while (len > 0) {
        ssize_t n = write(g_fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            say("writing %s: %s", g_file, strerror(errno));
            exit(FX_FAILED);
        }
        p += n;
        len -= (size_t)n;
        g_have += (uint64_t)n;
    }
}

/* Up to `want` bytes (all of the data when want is UINT64_MAX); the count
 * copied. */
static uint64_t body_copy(struct conn *c, uint64_t want)
{
    uint64_t got = 0;

    while (got < want) {
        if (c->pos == c->len && conn_fill(c) < 0)
            break;
        size_t n = c->len - c->pos;
        if ((uint64_t)n > want - got)
            n = (size_t)(want - got);
        body_put(c->buf + c->pos, n);
        c->pos += n;
        got += n;
    }
    return got;
}

static void body_chunked(struct conn *c)
{
    char line[256];

    for (;;) {
        char *end;
        if (conn_line(c, line, sizeof(line)) < 0) {
            say("the connection closed inside a chunked body");
            exit(FX_FAILED);
        }
        errno = 0;
        unsigned long long n = strtoull(line, &end, 16);
        if (end == line || errno || (*end && *end != ';' && *end != ' ')) {
            say("a bad chunk size line '%.40s'", line);
            exit(FX_FAILED);
        }
        if (n == 0)
            break;
        if (body_copy(c, n) != n) {
            say("the connection closed inside a chunk");
            exit(FX_FAILED);
        }
        if (conn_line(c, line, sizeof(line)) < 0 || line[0]) {
            say("a chunk without its CRLF");
            exit(FX_FAILED);
        }
    }
    while (conn_line(c, line, sizeof(line)) == 0 && line[0])
        ;   /* trailers */
}

static void file_restart(void)
{
    if (ftruncate(g_fd, 0) < 0 || lseek(g_fd, 0, SEEK_SET) < 0) {
        say("cannot truncate %s: %s", g_file, strerror(errno));
        exit(FX_FAILED);
    }
    g_have = 0;
    g_wrote_file = 1;
}

/* --- one request ------------------------------------------------------------------ */

struct reply {
    int status;
    int chunked;
    int has_length;
    uint64_t length;
    char location[2048];
    int has_range;
    uint64_t range_start, range_end, range_total;   /* range_total 0: unknown */
};

/* A decimal number at `s`, ending at `*end` (when given) or at the end of
 * the string; false on no digits or overflow. */
static int number(const char *s, uint64_t *out, const char **end)
{
    uint64_t v = 0;
    const char *p = s;
    if (*p < '0' || *p > '9')
        return 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (v > (UINT64_MAX - (uint64_t)(*p - '0')) / 10)
            return 0;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    if (end)
        *end = p;
    else if (*p)
        return 0;
    *out = v;
    return 1;
}

static void read_headers(struct conn *c, struct reply *r)
{
    char line[LINE_MAX_LEN];
    int minor;

    memset(r, 0, sizeof(*r));
    if (conn_line(c, line, sizeof(line)) < 0) {
        say("the server closed the connection without an answer");
        exit(FX_FAILED);
    }
    if (sscanf(line, "HTTP/1.%d %3d", &minor, &r->status) != 2 || r->status < 100) {
        say("not an HTTP answer: '%.60s'", line);
        exit(FX_FAILED);
    }
    for (int n = 0;; n++) {
        if (conn_line(c, line, sizeof(line)) < 0) {
            say("the connection closed inside the headers");
            exit(FX_FAILED);
        }
        if (line[0] == '\0')
            break;
        if (n == 200) {
            say("more than 200 header lines");
            exit(FX_FAILED);
        }
        char *v = strchr(line, ':');
        if (v == NULL)
            continue;
        *v++ = '\0';
        while (*v == ' ' || *v == '\t')
            v++;
        if (strcasecmp(line, "content-length") == 0) {
            uint64_t len;
            if (!number(v, &len, NULL) || (r->has_length && len != r->length)) {
                say("a bad or second, different Content-Length '%.40s'", v);
                exit(FX_FAILED);
            }
            r->length = len;
            r->has_length = 1;
        } else if (strcasecmp(line, "transfer-encoding") == 0) {
            /* chunked once seen stays: a later header does not undo it */
            if (strcasecmp(v, "chunked") == 0)
                r->chunked = 1;
            else if (strcasecmp(v, "identity") != 0) {
                say("an unsupported Transfer-Encoding '%.40s'", v);
                exit(FX_FAILED);
            }
        } else if (strcasecmp(line, "location") == 0) {
            snprintf(r->location, sizeof(r->location), "%s", v);
        } else if (strcasecmp(line, "content-range") == 0) {
            /* "bytes START-END/TOTAL" or "bytes *\/TOTAL" (an asterisk), nothing else */
            const char *p = v;
            uint64_t a, b, t;
            if (strncmp(p, "bytes ", 6) != 0) {
                say("a bad Content-Range '%.60s'", v);
                exit(FX_FAILED);
            }
            p += 6;
            if (*p == '*' && p[1] == '/' && number(p + 2, &t, NULL)) {
                r->range_total = t;
            } else if (number(p, &a, &p) && *p++ == '-' && number(p, &b, &p) && *p++ == '/' &&
                       number(p, &t, NULL) && a <= b && b < t) {
                r->has_range = 1;
                r->range_start = a;
                r->range_end = b;
                r->range_total = t;
            } else {
                say("a bad Content-Range '%.60s'", v);
                exit(FX_FAILED);
            }
        }
    }
}

static const char *reason(int status)
{
    switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 410: return "Gone";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "";
    }
}

int main(int argc, char **argv)
{
    const char *pin = NULL;
    int resume = 0, insecure = 0, opt, redirects = 0, restarted = 0;
    struct url u, next;
    struct conn *c = calloc(1, sizeof(*c));
    struct stat st;

    while ((opt = getopt(argc, argv, "cip:m:")) != -1) {
        switch (opt) {
        case 'c': resume = 1; break;
        case 'i': insecure = 1; break;
        case 'p':
            if (strncmp(optarg, "sha256:", 7) != 0 || strlen(optarg + 7) != 64 ||
                strspn(optarg + 7, "0123456789abcdef") != 64) {
                fprintf(stderr, "cosmo-fetch: -p takes sha256: and 64 lowercase hex digits\n");
                return FX_USAGE;
            }
            pin = optarg + 7;
            break;
        case 'm':
            if (!number(optarg, &g_max, NULL) || g_max == 0) {
                fprintf(stderr, "cosmo-fetch: -m takes a number of bytes, at least 1\n");
                return FX_USAGE;
            }
            break;
        default:
            fprintf(stderr, "usage: cosmo-fetch [-c] [-i] [-p sha256:HEX] [-m MAX] URL FILE\n");
            return FX_USAGE;
        }
    }
    if (optind + 2 != argc || c == NULL) {
        fprintf(stderr, "usage: cosmo-fetch [-c] [-i] [-p sha256:HEX] [-m MAX] URL FILE\n");
        return FX_USAGE;
    }
    g_url = argv[optind];
    g_file = argv[optind + 1];
    if (url_parse(g_url, &u) < 0) {
        say("not an http:// or https:// URL this can fetch");
        return FX_USAGE;
    }
    /* Before any connection; a redirect from https to http is refused
     * below as another scheme. */
    if (!u.https && !insecure) {
        say("plain HTTP is refused (only a repository marked insecure is fetched over http)");
        return FX_REFUSED;
    }
    if (!u.https && pin) {
        say("-p needs an https:// URL");
        return FX_USAGE;
    }

    g_wrote_file = !(resume && stat(g_file, &st) == 0);
    g_fd = open(g_file, O_WRONLY | O_CREAT | (resume ? 0 : O_TRUNC), 0644);
    if (g_fd < 0) {
        say("cannot open %s: %s", g_file, strerror(errno));
        return FX_FAILED;
    }
    g_have = 0;
    if (resume) {
        off_t end = lseek(g_fd, 0, SEEK_END);
        if (end < 0) {
            say("cannot find the end of %s: %s", g_file, strerror(errno));
            return FX_FAILED;
        }
        g_have = (uint64_t)end;
    }

    for (;;) {
        char req[3072];
        struct reply r;

        conn_open(c, &u, pin);
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s%s%s%s%s\r\nUser-Agent: cosmo-fetch/1\r\n"
                         "Accept-Encoding: identity\r\nConnection: close\r\n",
                         u.path, strchr(u.host, ':') ? "[" : "", u.host, strchr(u.host, ':') ? "]" : "",
                         u.explicit_port ? ":" : "", u.explicit_port ? u.port : "");
        if (g_have > 0)
            n += snprintf(req + n, sizeof(req) - (size_t)n, "Range: bytes=%llu-\r\n", (unsigned long long)g_have);
        n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
        conn_write(c, req, (size_t)n);
        /* Informational answers (100, 103) come before the final one; a
         * 101 switch of protocol is not something asked for. */
        do {
            read_headers(c, &r);
            if (r.status == 101) {
                say("HTTP 101: the server switched protocols, which was not asked for");
                return FX_FAILED;
            }
        } while (r.status >= 100 && r.status < 200);

        if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
            conn_close(c);
            if (r.location[0] == '\0') {
                say("HTTP %d without a Location", r.status);
                return FX_FAILED;
            }
            if (url_resolve(&u, r.location, &next) < 0) {
                say("redirected to '%.200s', not a URL this can fetch", r.location);
                refuse_exit();
            }
            if (!same_origin(&u, &next)) {
                say("redirected to '%.200s', another host or scheme: refused", r.location);
                refuse_exit();
            }
            if (++redirects > MAX_REDIRECT) {
                say("more than %d redirects", MAX_REDIRECT);
                return FX_FAILED;
            }
            u = next;
            continue;
        }
        if (r.status == 416 && g_have > 0) {
            conn_close(c);
            if (r.range_total == g_have) {
                if (g_max && g_have > g_max)
                    too_large();
                break;   /* it was all there */
            }
            if (restarted++) {
                say("HTTP 416 twice");
                return FX_FAILED;
            }
            file_restart();
            continue;
        }
        if (r.status >= 400 || (r.status != 200 && r.status != 206)) {
            say("HTTP %d %s", r.status, reason(r.status));
            conn_close(c);
            if (g_wrote_file && g_have == 0)
                unlink(g_file);
            return FX_HTTP;
        }
        if (r.status == 200 && g_have > 0)
            file_restart();   /* the whole file again */
        /* A part must be the rest of the file: from where FILE ends to the
         * total, and its length (when given) exactly that. */
        if (r.status == 206 && (g_have == 0 || !r.has_range || r.range_start != g_have ||
                                r.range_end + 1 != r.range_total ||
                                (r.has_length && r.length != r.range_end - r.range_start + 1))) {
            say("asked for bytes from %llu to the end, the server sent another range", (unsigned long long)g_have);
            conn_close(c);
            return FX_FAILED;
        }
        if (r.status == 206 && !r.has_length && !r.chunked) {
            r.length = r.range_end - r.range_start + 1;
            r.has_length = 1;
        }
        if (g_max && r.has_length && (g_have > g_max || r.length > g_max - g_have))
            too_large();
        if (r.chunked) {
            body_chunked(c);
        } else if (r.has_length) {
            uint64_t got = body_copy(c, r.length);
            if (got != r.length) {
                say("the connection closed after %llu of %llu bytes (%s keeps them; -c resumes)",
                    (unsigned long long)got, (unsigned long long)r.length, g_file);
                conn_close(c);
                return FX_FAILED;
            }
        } else {
            /* No length: the close ends it, a reset is a failure. */
            body_copy(c, UINT64_MAX);
            if (c->reset) {
                say("the connection was reset before the end of a body of no stated length");
                conn_close(c);
                return FX_FAILED;
            }
        }
        conn_close(c);
        if (r.status == 206 && g_have != r.range_total) {
            say("the part ended at %llu bytes, not the %llu the server announced", (unsigned long long)g_have,
                (unsigned long long)r.range_total);
            return FX_FAILED;
        }
        break;
    }
    if (fsync(g_fd) < 0 && errno != EINVAL && errno != ENOSYS) {
        say("fsync %s: %s", g_file, strerror(errno));
        return FX_FAILED;
    }
    if (close(g_fd) < 0) {
        say("closing %s: %s", g_file, strerror(errno));
        return FX_FAILED;
    }
    return FX_FETCHED;
}
