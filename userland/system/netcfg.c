/* netcfg - the machine's DHCP leases, and /etc/resolv.conf from them
 *
 *   netcfg [--wait SECONDS] [--resolv PATH] [--hostname PATH]
 *                                            write PATH (/etc/resolv.conf)
 *   netcfg show                              print each lease
 *
 * The kernel's DHCP client (docs/kernel-services/network/design.md, "The
 * DHCP client") holds each interface's lease; /dev/net/tapctl's snapshot
 * lists them in its lease section (version 7). /etc/rc runs `netcfg` once
 * the system is up: it waits (10 s by default, 0-600) until every DHCP
 * client holds a lease -- not merely the first, which may name no DNS
 * server while another's would -- and not at all when there is none, then
 * writes one `nameserver` line per DNS server the leases name -- musl's
 * resolver reads that file, and uses the first three. The file is written
 * beside the target and renamed over it, so a reader never sees half of
 * it. No lease with a DNS server leaves the file as it was (exit 1).
 *
 * A lease that names the machine (DHCP option 12, netctl version 8; shell
 * completeness S1) also names it here: the first held lease's name is
 * written to /etc/hostname, the same way, and set with sethostname.
 * /etc/rc applies /etc/hostname at the next boot before netcfg runs, so a
 * name a server gave once is kept by a boot whose server gives none.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/cosmo/netctl.h>

static unsigned char g_buf[COSMO_NETCTL_SNAPSHOT_MAX];

static const char *state_name(unsigned s)
{
    switch (s) {
    case COSMO_NETCTL_DHCP_INIT: return "init";
    case COSMO_NETCTL_DHCP_SELECTING: return "selecting";
    case COSMO_NETCTL_DHCP_REQUESTING: return "requesting";
    case COSMO_NETCTL_DHCP_BOUND: return "bound";
    case COSMO_NETCTL_DHCP_RENEWING: return "renewing";
    case COSMO_NETCTL_DHCP_REBINDING: return "rebinding";
    default: return "?";
    }
}

static int held(const struct cosmo_netctl_lease *l)
{
    return l->state == COSMO_NETCTL_DHCP_BOUND || l->state == COSMO_NETCTL_DHCP_RENEWING ||
           l->state == COSMO_NETCTL_DHCP_REBINDING;
}

/* Read the snapshot and walk to the lease section; every count is checked
 * against the bytes read before it is trusted. Returns the lease count and
 * sets *leases, or -1 with a message. */
static int read_leases(const struct cosmo_netctl_lease **leases)
{
    int fd = open("/dev/net/tapctl", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "netcfg: cannot open /dev/net/tapctl: %s\n", strerror(errno));
        return -1;
    }
    ssize_t n = read(fd, g_buf, sizeof(g_buf));
    close(fd);
    if (n < (ssize_t)sizeof(struct cosmo_netctl_list)) {
        fprintf(stderr, "netcfg: reading /dev/net/tapctl: %s\n", n < 0 ? strerror(errno) : "short");
        return -1;
    }
    struct cosmo_netctl_list ph;
    memcpy(&ph, g_buf, sizeof(ph));
    if (ph.version != COSMO_NETCTL_VERSION) {
        fprintf(stderr, "netcfg: snapshot version %u, this netcfg speaks %u\n", ph.version, COSMO_NETCTL_VERSION);
        return -1;
    }
    size_t off = sizeof(ph) + (size_t)ph.count * sizeof(struct cosmo_netctl_rule);
    struct cosmo_netctl_filter_list fh;
    if ((size_t)n < off + sizeof(fh))
        goto short_snap;
    memcpy(&fh, g_buf + off, sizeof(fh));
    off += sizeof(fh) + (size_t)fh.guest_count * sizeof(struct cosmo_netctl_filter_guest) +
           (size_t)fh.rule_count * sizeof(struct cosmo_netctl_filter_rule);
    struct cosmo_netctl_flow_list wh;
    if ((size_t)n < off + sizeof(wh))
        goto short_snap;
    memcpy(&wh, g_buf + off, sizeof(wh));
    off += sizeof(wh) + (size_t)wh.count * sizeof(struct cosmo_netctl_flow);
    struct cosmo_netctl_lease_list lh;
    if ((size_t)n < off + sizeof(lh))
        goto short_snap;
    memcpy(&lh, g_buf + off, sizeof(lh));
    off += sizeof(lh);
    if (lh.version != COSMO_NETCTL_VERSION || (size_t)n < off + (size_t)lh.count * sizeof(struct cosmo_netctl_lease))
        goto short_snap;
    *leases = (const struct cosmo_netctl_lease *)(g_buf + off);
    return lh.count;
short_snap:
    fprintf(stderr, "netcfg: the snapshot is shorter than its counts say\n");
    return -1;
}

static void show(const struct cosmo_netctl_lease *l)
{
    char a[16], g[16], s[16];
    unsigned prefix = 0;
    for (uint32_t m = ntohl(l->mask); m & 0x80000000u; m <<= 1)
        prefix++;
    inet_ntop(AF_INET, &l->addr, a, sizeof(a));
    inet_ntop(AF_INET, &l->router, g, sizeof(g));
    inet_ntop(AF_INET, &l->server, s, sizeof(s));
    printf("%s: %s", l->ifname, state_name(l->state));
    if (held(l)) {
        printf(" %s/%u gateway %s server %s lease %u s (T1 %u, T2 %u; %u s left) dns", a, prefix, g, s, l->lease_s,
               l->t1_s, l->t2_s, l->remaining_ms == 0xffffffffu ? 0xffffffffu : l->remaining_ms / 1000);
        for (unsigned i = 0; i < l->dns_count && i < COSMO_NETCTL_MAX_DNS; i++) {
            char d[16];
            inet_ntop(AF_INET, &l->dns[i], d, sizeof(d));
            printf(" %s", d);
        }
        if (l->dns_count == 0)
            printf(" none");
        if (l->hostname[0] != '\0')
            printf(" hostname %.63s", l->hostname);
    }
    printf("\n");
}

/* Write `len` bytes to `path` beside it and rename over it, so a reader
 * never sees half a file. 0, or -1 with a message. */
static int write_whole(const char *path, const char *text, size_t len)
{
    char tmp[256];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.netcfg", path) >= sizeof(tmp)) {
        fprintf(stderr, "netcfg: %s: the path is too long\n", path);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, len) != (ssize_t)len || close(fd) < 0 || rename(tmp, path) < 0) {
        fprintf(stderr, "netcfg: writing %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            unlink(tmp);
        return -1;
    }
    return 0;
}

/* The first held lease's host name into `path` and the kernel's uts name.
 * 0 when there is none to set or it was set; 1 on a failure. */
static int apply_hostname(const struct cosmo_netctl_lease *l, int n, const char *path)
{
    for (int i = 0; i < n; i++) {
        if (!held(&l[i]) || l[i].hostname[0] == '\0')
            continue;
        char name[sizeof(l[i].hostname)];
        memcpy(name, l[i].hostname, sizeof(name));
        name[sizeof(name) - 1] = '\0';
        size_t len = strlen(name);
        char text[sizeof(name) + 1];
        memcpy(text, name, len);
        text[len] = '\n';
        if (write_whole(path, text, len + 1) < 0)
            return 1;
        if (sethostname(name, len) < 0) {
            fprintf(stderr, "netcfg: sethostname %s: %s\n", name, strerror(errno));
            return 1;
        }
        printf("netcfg: %s: %s (from %s's lease)\n", path, name, l[i].ifname);
        return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned wait_s = 10;
    const char *path = "/etc/resolv.conf";
    const char *host_path = "/etc/hostname";
    int show_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "show") == 0)
            show_only = 1;
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || v < 0 || v > 600) {
                fprintf(stderr, "netcfg: --wait wants whole seconds, 0 to 600\n");
                return 2;
            }
            wait_s = (unsigned)v;
        }
        else if (strcmp(argv[i], "--resolv") == 0 && i + 1 < argc)
            path = argv[++i];
        else if (strcmp(argv[i], "--hostname") == 0 && i + 1 < argc)
            host_path = argv[++i];
        else {
            fprintf(stderr, "usage: netcfg [show] [--wait SECONDS] [--resolv PATH] [--hostname PATH]\n");
            return 2;
        }
    }

    const struct cosmo_netctl_lease *l = NULL;
    int n;
    if (show_only) {
        if ((n = read_leases(&l)) < 0)
            return 1;
        if (n == 0)
            printf("netcfg: no DHCP clients (every interface is configured otherwise)\n");
        for (int i = 0; i < n; i++)
            show(&l[i]);
        return 0;
    }

    /* Wait for every client to hold a lease, 100 ms at a time; with no
     * client at all (every interface configured otherwise) there is
     * nothing to wait for. */
    for (unsigned tries = 0;; tries++) {
        if ((n = read_leases(&l)) < 0)
            return 1;
        int all = 1;
        for (int i = 0; i < n; i++)
            all &= held(&l[i]);
        if (n == 0 || all || tries >= wait_s * 10)
            break;
        usleep(100000);
    }

    int host_rc = apply_hostname(l, n, host_path);

    char text[512];
    size_t len = (size_t)snprintf(text, sizeof(text), "# written by netcfg from the DHCP lease\n");
    unsigned servers = 0;
    uint32_t seen[COSMO_NETCTL_MAX_LEASES * COSMO_NETCTL_MAX_DNS];
    for (int i = 0; i < n; i++) {
        if (!held(&l[i]))
            continue;
        show(&l[i]);
        for (unsigned j = 0; j < l[i].dns_count && j < COSMO_NETCTL_MAX_DNS; j++) {
            int dup = 0;
            for (unsigned k = 0; k < servers; k++)
                dup |= seen[k] == l[i].dns[j];
            if (dup)
                continue;
            seen[servers++] = l[i].dns[j];
            char d[16];
            inet_ntop(AF_INET, &l[i].dns[j], d, sizeof(d));
            len += (size_t)snprintf(text + len, sizeof(text) - len, "nameserver %s\n", d);
        }
    }
    if (servers == 0) {
        fprintf(stderr, "netcfg: no lease names a DNS server; %s left as it was\n", path);
        return 1;
    }
    if (write_whole(path, text, len) < 0)
        return 1;
    printf("netcfg: %s: %u nameserver(s)\n", path, servers);
    return host_rc;
}
