/* syslogd - /dev/log into the kernel's log (shell completeness S2)
 *
 *   syslogd [PATH]        bind PATH (/dev/log) and copy every message
 *
 * musl's syslog(3) -- BusyBox's logger, and any program that logs -- sends
 * each message as one datagram to the unix socket /dev/log, formatted
 * "<PRI>Mmm dd hh:mm:ss TAG: TEXT". This service binds that socket and
 * writes each message to the kernel's log
 * (SYS_log, which dmesg and the console show) as
 * "syslog: [FACILITY.LEVEL] TAG: TEXT": the priority by name, the
 * timestamp dropped (the kernel's line has its own), the text cut to what
 * one log line takes. Nothing is written to a file. The socket's node is
 * the kernel's 0755, owned by root (the native ABI has no chmod), so a
 * process of another uid cannot send to it (inventory); every login here
 * is root's.
 *
 * It runs with no privileges (/etc/svc/syslogd), so the kernel's rate
 * limit for an unprivileged writer applies to it as to anyone: a burst
 * beyond it is dropped rather than flooding the log, and the next line
 * that gets through says how many were. A message a user sends is logged
 * under this service's pid, with the tag the sender chose.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cosmo/syscall.h>

/* What one SYS_log call takes: fewer than 200 bytes. */
#define LOG_LINE_MAX 199

static const char *const g_levels[8] = { "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug" };
static const char *const g_facilities[24] = {
    "kern", "user", "mail", "daemon", "auth", "syslog", "lpr", "news", "uucp", "cron", "authpriv", "ftp",
    "16", "17", "18", "19", "local0", "local1", "local2", "local3", "local4", "local5", "local6", "local7",
};

/* "Mmm dd hh:mm:ss " at `s`: syslog(3)'s timestamp. */
static int is_stamp(const char *s, size_t n)
{
    if (n < 16 || s[3] != ' ' || s[6] != ' ' || s[9] != ':' || s[12] != ':' || s[15] != ' ')
        return 0;
    for (unsigned i = 0; i < 3; i++)
        if ((s[i] < 'A' || s[i] > 'Z') && (s[i] < 'a' || s[i] > 'z'))
            return 0;
    return 1;
}

/* One datagram, `n` bytes, into the kernel's log. 0 when it was written,
 * -1 when the rate limit dropped it. */
static int relay(char *msg, size_t n, unsigned long dropped)
{
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\0'))
        n--;
    unsigned pri = 13;   /* user.notice, syslog(3)'s default, for a message without one */
    size_t i = 0;
    if (n > 0 && msg[0] == '<') {
        unsigned v = 0;
        size_t j = 1;
        while (j < n && j < 5 && msg[j] >= '0' && msg[j] <= '9')
            v = v * 10 + (unsigned)(msg[j++] - '0');
        if (j > 1 && j < n && msg[j] == '>' && v < 24 * 8) {
            pri = v;
            i = j + 1;
        }
    }
    if (is_stamp(msg + i, n - i))
        i += 16;
    char line[LOG_LINE_MAX + 1];
    int len;
    if (dropped)
        len = snprintf(line, sizeof(line), "syslog: [%s.%s] (%lu dropped before) %.*s", g_facilities[pri >> 3],
                       g_levels[pri & 7], dropped, (int)(n - i), msg + i);
    else
        len = snprintf(line, sizeof(line), "syslog: [%s.%s] %.*s", g_facilities[pri >> 3], g_levels[pri & 7],
                       (int)(n - i), msg + i);
    if (len < 0)
        return 0;
    if ((size_t)len > LOG_LINE_MAX)
        len = LOG_LINE_MAX;
    /* A control character would split or garble the console line. */
    for (int k = 0; k < len; k++)
        if ((unsigned char)line[k] < 0x20 || line[k] == 0x7f)
            line[k] = ' ';
    return cosmo_log(line, (size_t)len) == -EAGAIN ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/log";
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "syslogd: socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "syslogd: %s: the path is too long\n", path);
        return 1;
    }
    strcpy(sa.sun_path, path);
    unlink(path);   /* a node left by an earlier run would refuse the bind */
    if (bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "syslogd: bind %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("syslogd: %s\n", path);
    fflush(stdout);
    unsigned long dropped = 0;
    for (;;) {
        char msg[1024];
        ssize_t n = recv(fd, msg, sizeof(msg), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "syslogd: recv: %s\n", strerror(errno));
            return 1;
        }
        if (relay(msg, (size_t)n, dropped) < 0)
            dropped++;
        else
            dropped = 0;
    }
}
