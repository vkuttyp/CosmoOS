/*
 * test_tap.c - The tap interface and its devices (kernel-services/network/
 * tap.c) on the host: roadmap 2.0 SA6. A digest over seeded histories,
 * recorded from the C, of:
 *
 *   - /dev/net/tap opened (a tap per open, up to eight; the ninth refused;
 *     the interface's registration or the tap services failing), closed,
 *     written (frames of every length, the services' input filter claiming
 *     some, a caller without PRIV_DEVICE), read (a frame the stack sent, a
 *     buffer too small, non-blocking by the file's flag or an I/O ring
 *     entry, blocking until a frame arrives or the reader is killed), its
 *     readiness, poll queue and non-blocking switch;
 *   - the stack transmitting out a tap, past its 64-frame queue;
 *   - /dev/net/tapctl written (port-forward and firewall commands, valid and
 *     not: every field, the version, the length, the privilege) and read
 *     (the whole snapshot -- port-forwards, the firewall's guests and rules,
 *     NAT's and the firewall's flows, DHCP leases, interfaces, routes,
 *     neighbours -- into buffers of every size);
 *   - the tap's own calls: create, destroy, inject, receive, the filter.
 *
 * The code under test is tap.c alone, over real mbufs: the interface layer,
 * NAT, the firewall, the DHCP client, routes, ARP, the tap services, the
 * credentials, ramfs and the waits are this file's, deterministic and
 * seeded, each call and its arguments folded into the digest with every
 * frame the tap hands on, every byte a read returns and every return. The
 * UAPI's bounds and record sizes tap.c relies on are asserted here, at
 * compile time. After each history nothing is allocated beyond the
 * baseline, every tap is released and no mbuf is alive.
 *
 * kmalloc fails, now and then, at the operation's first, second or third
 * allocation.
 */

/* The kernel's pid_t (process.h, through syscall.h) is not the host's: renamed
 * while the kernel's headers are read, then the host's headers after them
 * (Darwin's define htons, which the kernel's inet.h defines too). */
#define pid_t cosmo_pid_t
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tap.h>
#include <kernel/net/tapsvc.h>
#include <kernel/netif.h>
#include <kernel/percpu.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>
#include <uapi/cosmo/netctl.h>
#undef pid_t

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#include "harness.h"

/* ---- the ABI tap.c relies on (moved here from tap.c) ---- */

/* The UAPI's snapshot bounds are promises about these tables; keep them true. */
_Static_assert(COSMO_NETCTL_MAX_FORWARDS == NAT_PF_MAX, "netctl.h forward bound drifted from NAT_PF_MAX");
_Static_assert(COSMO_NETCTL_MAX_GUESTS == FW_MAX_GUESTS, "netctl.h guest bound drifted from FW_MAX_GUESTS");
_Static_assert(COSMO_NETCTL_MAX_RULES_PER_GUEST == FW_RULES_PER_GUEST,
               "netctl.h rules-per-guest bound drifted from FW_RULES_PER_GUEST");
_Static_assert(COSMO_NETCTL_HOST_ADDR == FW_HOST_GUEST_IP, "netctl.h host sentinel drifted from FW_HOST_GUEST_IP");
_Static_assert(COSMO_NETCTL_MAX_NAT_FLOWS == NAT_TABLE_SIZE, "netctl.h NAT flow bound drifted from NAT_TABLE_SIZE");
_Static_assert(COSMO_NETCTL_MAX_FW_FLOWS == FW_FLOW_MAX, "netctl.h firewall flow bound drifted from FW_FLOW_MAX");
_Static_assert(COSMO_NETCTL_PROTO_ICMP == IPPROTO_ICMP && COSMO_NETCTL_PROTO_TCP == IPPROTO_TCP &&
               COSMO_NETCTL_PROTO_UDP == IPPROTO_UDP, "netctl.h protocol values are the IP protocol numbers");
/* One read is one object call through the syscall bounce, which offers at
 * most IO_BOUNCE_MAX: a snapshot that could outgrow it could never be read. */
_Static_assert(COSMO_NETCTL_SNAPSHOT_MAX <= IO_BOUNCE_MAX, "the netctl snapshot must fit one read");
_Static_assert(sizeof(struct cosmo_netctl_flow) == 32, "cosmo_netctl_flow is 32 bytes in version 6");
_Static_assert(sizeof(struct cosmo_netctl_flow_list) == 128, "cosmo_netctl_flow_list is 128 bytes in version 6");
/* The record sizes are the ABI; a writer of the previous version must be
 * refused by size, never misread. Version 5 spent one of the command's and
 * the rule's three reserved bytes on `scope` (so those two are unchanged)
 * and grew the policy record, which had none left, from 8 to 12. */
_Static_assert(sizeof(struct cosmo_netctl_filter) == 28, "cosmo_netctl_filter is 28 bytes in version 5");
_Static_assert(sizeof(struct cosmo_netctl_filter_rule) == 24, "cosmo_netctl_filter_rule is 24 bytes in version 5");
_Static_assert(sizeof(struct cosmo_netctl_filter_guest) == 12, "cosmo_netctl_filter_guest is 12 bytes in version 5");
_Static_assert(COSMO_NETCTL_MAX_LEASES == DHCP_MAX_CLIENTS && COSMO_NETCTL_MAX_DNS == DHCP_MAX_DNS,
               "the lease section's bounds are the client table's");
_Static_assert(COSMO_NETCTL_DHCP_INIT == DHCP_S_INIT && COSMO_NETCTL_DHCP_SELECTING == DHCP_S_SELECTING &&
               COSMO_NETCTL_DHCP_REQUESTING == DHCP_S_REQUESTING && COSMO_NETCTL_DHCP_BOUND == DHCP_S_BOUND &&
               COSMO_NETCTL_DHCP_RENEWING == DHCP_S_RENEWING && COSMO_NETCTL_DHCP_REBINDING == DHCP_S_REBINDING,
               "the UAPI's DHCP states are the client's");
_Static_assert(COSMO_NETCTL_MAX_ROUTES == IPV4_ROUTES_MAX && COSMO_NETCTL_MAX_NEIGH == ARP_TABLE_SIZE &&
               COSMO_NETCTL_MAX_IFS == NETIF_MAX, "the version-8 sections' bounds are the listings', whole");
_Static_assert(sizeof(struct cosmo_netctl_lease) == 128 && sizeof(struct cosmo_netctl_if) == 120 &&
               sizeof(struct cosmo_netctl_route) == 32 && sizeof(struct cosmo_netctl_neigh) == 20,
               "the version-8 records' sizes are the ABI");

/* The digest of the C (tap.c, 2026-11-04), which the CeeSafe port gives. */
#define TAP_DIGEST 0xedefcf1c515836e7ull
#define HISTORIES 3u
#define OPS 3000u
#define SEED 0x7A9DE11C2026B110ull
#define NFILE 10u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix32(uint32_t v) { mix(&v, sizeof(v)); }
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static bool g_benching;

static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix32(0x7a00u + which);
    mix64(value);
}

static void fill(void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

static void on_klog(int level, const char *line)
{
    note(1, (uint64_t)level);
    mix(line, strlen(line));
}

/* ---- the allocator: malloc, counted; kmalloc fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(70, size);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    if (p != NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align)
{
    (void)align;
    struct kmem_cache *c = calloc(1, sizeof(*c));
    c->name = name;
    c->object_size = size;
    return c;
}

static bool g_fail_cache;   /* the next mbuf allocation fails */

void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags)
{
    (void)flags;
    if (g_fail_cache) {
        g_fail_cache = false;
        return NULL;
    }
    g_live++;
    return malloc(c->object_size);
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
    (void)c;
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the thread, the credentials, the clock ---- */

static struct thread g_thread;
static struct percpu g_percpu = { .current = &g_thread };
struct percpu *arch_percpu_get(void) { return &g_percpu; }

static struct credentials g_cred;
const struct credentials *cred_current(void) { return &g_cred; }

static uint64_t g_now = 5000ull * 1000000000ull;
uint64_t clock_now_ns(void) { return g_now; }

/* ---- kobjects: a plain count ---- */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    memset(obj, 0, sizeof(*obj));
    obj->type = type;
    obj->refcount = 1;
}

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    obj->refcount++;
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    if (--obj->refcount == 0 && obj->type && obj->type->release)
        obj->type->release(obj);
}

/* ---- the interface layer ---- */

static bool g_fail_register;
static unsigned g_next_index = 2, g_released, g_live_ifs;
#define NIFS 16u
static struct netif *g_ifs[NIFS];

static void netif_released(struct kobject *obj)
{
    struct netif *nif = container_of(obj, struct netif, obj);
    note(2, nif->index);
    g_released++;
    g_live_ifs--;
    nif->ops->release(nif);
}

static const struct kobject_type g_netif_type = { .name = "test-netif", .release = netif_released };

int netif_register(struct netif *nif)
{
    note(3, ((uint64_t)nif->flags << 32) | nif->mtu);
    mix(nif->name, sizeof(nif->name));
    mix(nif->mac, 6);
    if (g_fail_register) {
        g_fail_register = false;
        return -EEXIST;
    }
    kobject_init(&nif->obj, &g_netif_type);
    kobject_get(&nif->obj);   /* the registry's reference and the creator's */
    nif->index = g_next_index++;
    spinlock_init(&nif->lock, "netif");
    for (unsigned i = 0; i < NIFS; i++)
        if (g_ifs[i] == NULL) {
            g_ifs[i] = nif;
            break;
        }
    g_live_ifs++;
    return 0;
}

void netif_unregister(struct netif *nif)
{
    note(4, nif->index);
    for (unsigned i = 0; i < NIFS; i++)
        if (g_ifs[i] == nif)
            g_ifs[i] = NULL;
    kobject_put(&nif->obj);   /* the registry's reference */
}

void netif_set_ipv4(struct netif *nif, uint32_t addr, uint32_t mask, uint32_t gateway)
{
    note(5, ((uint64_t)addr << 32) | mask);
    note(5, gateway);
    nif->ip4.addr = addr;
    nif->ip4.mask = mask;
    nif->ip4.gateway = gateway;
}

void netif_set_up(struct netif *nif, bool up)
{
    note(6, ((uint64_t)nif->index << 1) | up);
    if (up)
        nif->flags |= NETIF_UP;
    else
        nif->flags &= ~NETIF_UP;
}

void netif_set_forward(struct netif *nif, bool on) { note(7, ((uint64_t)nif->index << 1) | on); }
void netif_set_masquerade(struct netif *nif, bool on) { note(8, ((uint64_t)nif->index << 1) | on); }

/* A frame the tap hands to the stack. */
void netif_rx(struct netif *nif, struct mbuf *m)
{
    uint8_t b[2048];
    uint32_t n = m_length(m);
    EXPECT(n <= sizeof(b));
    EXPECT(m_copydata(m, 0, n, b));
    note(9, ((uint64_t)nif->index << 32) | n);
    mix(b, n);
    m_freem(m);
}

unsigned netif_list(struct netif_info *out, unsigned max)
{
    unsigned n = pick(NETIF_MAX + 3);
    note(10, ((uint64_t)max << 32) | n);
    if (n > max + 2)
        n = max + 2;   /* the listing may be longer than asked: tap.c clips it */
    unsigned k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) {
        fill(&out[i], sizeof(out[i]));
        out[i].name[7] = 0;
    }
    return n;
}

/* ---- the tap services ---- */

static bool g_fail_svc;
static unsigned g_svc_tokens[NFILE];

/* The services' DHCP filter: claims an IPv4 frame whose first payload byte is odd. */
static bool test_filter(struct tap *t, const void *frame, uint32_t len, void *arg)
{
    (void)t;
    const uint8_t *f = frame;
    bool claim = len > 14 && f[12] == 0x08 && f[13] == 0x00 && (f[14] & 1);
    note(11, ((uint64_t)*(unsigned *)arg << 33) | ((uint64_t)len << 1) | claim);
    return claim;
}

struct tapsvc *tapsvc_start(struct tap *t)
{
    note(12, 0);
    if (g_fail_svc) {
        g_fail_svc = false;
        return NULL;
    }
    for (unsigned i = 0; i < NFILE; i++)
        if (g_svc_tokens[i] == 0) {
            g_svc_tokens[i] = 100 + i;
            tap_set_input_filter(t, test_filter, &g_svc_tokens[i]);
            return (struct tapsvc *)(void *)&g_svc_tokens[i];
        }
    return NULL;
}

static struct tap *g_svc_tap[NFILE];

void tapsvc_stop(struct tapsvc *svc)
{
    unsigned *tok = (unsigned *)(void *)svc;
    note(13, *tok);
    *tok = 0;
}

/* ---- NAT and the firewall ---- */

static const char *g_portforward;

bool fwcfg_get_string(const char *key, char *buf, size_t len)
{
    note(14, strcmp(key, "portforward") == 0);
    if (g_portforward == NULL || len == 0)
        return false;
    snprintf(buf, len, "%s", g_portforward);
    return true;
}

void nat_portforward_apply(const char *cfg)
{
    note(15, strlen(cfg));
    mix(cfg, strlen(cfg));
}

void fw_guest_attach(uint32_t guest_ip, uint32_t gateway_ip) { note(16, ((uint64_t)guest_ip << 32) | gateway_ip); }
void fw_guest_purge(uint32_t guest_ip) { note(17, guest_ip); }
void nat_guest_purge(uint32_t guest_ip) { note(18, guest_ip); }

static int g_rc_pool[] = { 0, 0, 0, -EINVAL, -EEXIST, -ENOSPC };

int nat_pf_add(uint8_t proto, uint16_t host_port, uint32_t guest_ip, uint16_t guest_port)
{
    note(19, ((uint64_t)proto << 48) | ((uint64_t)host_port << 32) | guest_ip);
    note(19, guest_port);
    int rc = g_rc_pool[pick(6)];
    note(19, (uint64_t)(int64_t)rc);
    return rc;
}

bool nat_pf_del(uint8_t proto, uint16_t host_port)
{
    note(20, ((uint64_t)proto << 16) | host_port);
    return pick(3) != 0;
}

unsigned nat_pf_list(struct nat_pf_rule *out, unsigned max)
{
    unsigned n = pick(5) == 0 ? max : pick(6);
    for (unsigned i = 0; i < n; i++) {
        out[i].proto = pick(2) ? IPPROTO_TCP : IPPROTO_UDP;
        out[i].host_port = (uint16_t)next();
        out[i].guest_port = (uint16_t)next();
        out[i].guest_ip = (uint32_t)next();
    }
    note(21, n);
    return n;
}

unsigned nat_flow_list(struct nat_flow *out, unsigned max, uint64_t now)
{
    unsigned n = pick(4) == 0 ? max : pick(12);
    for (unsigned i = 0; i < n; i++) {
        fill(&out[i], sizeof(out[i]));
        out[i].kind = pick(2) ? NAT_KIND_DNAT : 0;
        out[i].est = pick(2);
        uint64_t span = pick(8) == 0 ? ~0ull / 2 : 600000000000ull;   /* one draw per statement */
        out[i].expires_ns = now + 1 + next() % span;
    }
    note(22, ((uint64_t)n << 32) | max);
    return n;
}

unsigned fw_flow_list(struct fw_flow_info *out, unsigned max, uint64_t now)
{
    unsigned n = pick(4) == 0 ? max : pick(12);
    for (unsigned i = 0; i < n; i++) {
        fill(&out[i], sizeof(out[i]));
        if (pick(3) == 0)
            out[i].guest_ip = FW_HOST_GUEST_IP;
        out[i].est = pick(2);
        out[i].expires_ns = now + 1 + next() % 600000000000ull;
    }
    note(23, ((uint64_t)n << 32) | max);
    return n;
}

void nat_get_stats(struct nat_stats *out)
{
    fill(out, sizeof(*out));
    note(24, 0);
}

void fw_get_stats(struct fw_stats *out)
{
    fill(out, sizeof(*out));
    note(25, 0);
}

static void note_rule(uint32_t which, const struct fw_rule *r)
{
    note(which, ((uint64_t)r->direction << 56) | ((uint64_t)r->proto << 48) | ((uint64_t)r->dst_prefix << 40) |
                    ((uint64_t)r->verdict << 32) | r->dst_ip);
    note(which, ((uint64_t)r->dst_port << 48) | ((uint64_t)r->src_prefix << 40) | ((uint64_t)r->scope << 32) |
                    r->src_ip);
}

int fw_rule_add(uint32_t guest_ip, unsigned at_index, const struct fw_rule *r)
{
    note(26, ((uint64_t)guest_ip << 32) | at_index);
    note_rule(26, r);
    int rc = g_rc_pool[pick(6)];
    note(26, (uint64_t)(int64_t)rc);
    return rc;
}

int fw_rule_del(uint32_t guest_ip, const struct fw_rule *match)
{
    note(27, guest_ip);
    note_rule(27, match);
    return pick(3) == 0 ? -ENOENT : 0;
}

unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule *out, unsigned max)
{
    unsigned n = pick(6) == 0 ? max : pick(4);
    for (unsigned i = 0; i < n; i++)
        fill(&out[i], sizeof(out[i]));
    note(28, ((uint64_t)guest_ip << 32) | n);
    return n;
}

int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict)
{
    note(29, ((uint64_t)guest_ip << 16) | ((uint64_t)direction << 8) | verdict);
    return pick(4) == 0 ? -EINVAL : 0;
}

int fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest, uint8_t *to_host, uint8_t *from_uplink,
                  uint8_t *output)
{
    note(30, guest_ip);
    *to_uplink = (uint8_t)next();
    *to_guest = (uint8_t)next();
    *to_host = (uint8_t)next();
    *from_uplink = (uint8_t)next();
    *output = (uint8_t)next();
    return 0;
}

unsigned fw_guest_list(uint32_t *out, unsigned max)
{
    unsigned n = pick(5) == 0 ? max : pick(4);
    for (unsigned i = 0; i < n; i++)
        out[i] = (uint32_t)next();
    note(31, n);
    return n;
}

/* ---- the DHCP client, routes, ARP ---- */

unsigned dhcp_lease_list(struct dhcp_lease_info *out, unsigned max)
{
    unsigned n = pick(max + 1);
    for (unsigned i = 0; i < n; i++) {
        fill(&out[i], sizeof(out[i]));
        out[i].hostname[pick(DHCP_HOSTNAME_MAX)] = 0;
        out[i].hostname[DHCP_HOSTNAME_MAX - 1] = 0;
    }
    note(32, n);
    return n;
}

unsigned ipv4_route_list(struct ipv4_route_info *out, unsigned max)
{
    unsigned n = pick(4) == 0 ? max : pick(6);
    for (unsigned i = 0; i < n; i++)
        fill(&out[i], sizeof(out[i]));
    note(33, n);
    return n;
}

unsigned arp_list(struct arp_info *out, unsigned max)
{
    unsigned n = pick(6) == 0 ? max + 1 : pick(8);
    unsigned k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) {
        fill(&out[i], sizeof(out[i]));
        out[i].complete = pick(2);
    }
    note(34, n);
    return n;
}

/* ---- the files, ramfs, the waits ---- */

static const struct chrdev_ops *g_tap_ops, *g_ctl_ops;
static bool g_fail_mkchr;

int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)
{
    (void)start;
    note(35, mode);
    mix(path, strlen(path));
    return -EEXIST;
}

int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)
{
    (void)priv;
    note(36, mode);
    mix(path, strlen(path));
    *out = NULL;
    if (g_fail_mkchr)
        return -ENOSPC;
    if (strcmp(path, "/dev/net/tap") == 0)
        g_tap_ops = ops;
    else
        g_ctl_ops = ops;
    note(37, ((uint64_t)ops->open_priv << 1) | (ops->read_file != NULL));
    return 0;
}

static bool g_nonblock[NFILE];
static struct file g_files[NFILE];
static bool g_open[NFILE];

static unsigned file_index(const struct file *f) { return (unsigned)(f - g_files); }

bool file_nonblocking(const struct file *f) { return g_nonblock[file_index(f)]; }

int file_set_nonblock(struct file *f, int on)
{
    unsigned i = file_index(f);
    int was = g_nonblock[i];
    if (on >= 0)
        g_nonblock[i] = on != 0;
    note(38, ((uint64_t)i << 8) | (unsigned)(on + 1));
    return was;
}

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    memset(wq, 0, sizeof(*wq));
    mix(name, strlen(name));
}

static unsigned g_wakes;
unsigned waitqueue_wake_all(struct waitqueue *wq)
{
    (void)wq;
    g_wakes++;
    return 0;
}

/* A blocked reader: a frame comes (the stack transmits one out the tap), or the reader is killed. */
static void stack_send(struct netif *nif, unsigned len);

int wait_floor_until_killable(struct waitqueue *wq, bool (*cond)(void *), void *arg)
{
    (void)wq;
    if (cond(arg))
        return 0;
    unsigned w = pick(3);
    note(39, w);
    if (w == 0)
        return -EINTR;
    struct netif *nif = tap_netif((struct tap *)arg);   /* the condition's argument is the tap */
    stack_send(nif, 60 + pick(200));
    EXPECT(cond(arg));
    return 0;
}

/* ---- the stack's side ---- */

static void stack_send(struct netif *nif, unsigned len)
{
    struct mbuf *m = m_getcl();
    EXPECT(m != NULL);
    if (m == NULL)
        return;
    uint8_t b[2048];
    fill(b, len);
    EXPECT(m_append(m, b, len) == 0);
    note(40, ((uint64_t)nif->index << 32) | len);
    EXPECT(nif->ops->transmit(nif, m) == 0);
}

/* ---- the operations ---- */

static void give_privs(void)
{
    g_cred.euid = pick(24) == 0 ? 1000 : 0;
    g_cred.privs = pick(16) == 0 ? 0 : PRIV_DEVICE | PRIV_NET;
}

static struct tap_open_view { struct tap *tap; } *open_of(unsigned i) { return g_files[i].priv; }

static void op_open(void)
{
    unsigned i = pick(NFILE);
    if (g_open[i])
        return;
    unsigned f = pick(16);
    g_fail_register = f == 0;
    g_fail_svc = f == 1;
    static const char *const pfs[] = { NULL, NULL, "tcp:8080:10.0.3.15:80", "udp:53:10.0.4.15:53,tcp:22:10.0.9.15:22" };
    g_portforward = pfs[pick(4)];
    memset(&g_files[i], 0, sizeof(g_files[i]));
    g_nonblock[i] = false;
    int rc = g_tap_ops->open(NULL, &g_files[i]);
    g_fail_register = g_fail_svc = false;
    note(41, ((uint64_t)i << 32) | (uint32_t)rc);
    if (rc == 0) {
        g_open[i] = true;
        g_svc_tap[i] = open_of(i)->tap;
    }
}

static unsigned g_after_purge;

/* The debug builds' release seam: after the purges, before the slot goes. */
static void after_purge(void *arg)
{
    note(71, *(unsigned *)arg);
    g_after_purge++;
}

static void op_close(void)
{
    unsigned i = pick(NFILE);
    if (!g_open[i]) {
        struct file f;
        memset(&f, 0, sizeof(f));
        g_tap_ops->release(NULL, &f);   /* an open that set no instance */
        return;
    }
    static unsigned tag = 9;
    tap_test_after_purge = pick(2) ? after_purge : NULL;
    tap_test_after_purge_arg = &tag;
    g_tap_ops->release(NULL, &g_files[i]);
    EXPECT(g_files[i].priv == NULL);
    g_open[i] = false;
    tap_test_after_purge = NULL;
    note(42, i);
}

static void op_write(void)
{
    unsigned i = pick(NFILE);
    if (!g_open[i])
        return;
    give_privs();
    static const unsigned lens[] = { 0, 10, 13, 14, 15, 60, 600, 1514, 1515, 2000 };
    unsigned len = lens[pick(sizeof(lens) / sizeof(lens[0]))];
    uint8_t b[2048];
    fill(b, len);
    if (len >= 14 && pick(2)) {
        b[12] = 0x08;
        b[13] = 0x00;
    }
    int64_t rc = g_tap_ops->write_file(NULL, &g_files[i], 0, b, len);
    note(43, ((uint64_t)i << 32) | (uint32_t)rc);
}

static void op_transmit(void)
{
    unsigned i = pick(NFILE);
    if (!g_open[i])
        return;
    unsigned n = pick(6) == 0 ? 40 + pick(40) : 1 + pick(3);
    for (unsigned k = 0; k < n; k++)
        stack_send(tap_netif(open_of(i)->tap), 42 + pick(1473));
}

static void op_read(void)
{
    unsigned i = pick(NFILE);
    if (!g_open[i])
        return;
    give_privs();
    static const unsigned lens[] = { 0, 40, 60, 600, 1514, 2048 };
    unsigned len = lens[pick(sizeof(lens) / sizeof(lens[0]))];
    unsigned nb = pick(4);
    g_nonblock[i] = nb == 0;
    g_thread.io_nonblock = nb == 1;
    uint8_t b[2048];
    memset(b, 0, sizeof(b));
    int64_t rc = g_tap_ops->read_file(NULL, &g_files[i], 0, b, len);
    g_thread.io_nonblock = false;
    note(44, ((uint64_t)i << 32) | (uint32_t)rc);
    if (rc > 0)
        mix(b, (size_t)rc);
}

static void op_ready(void)
{
    unsigned i = pick(NFILE);
    if (!g_open[i])
        return;
    note(45, g_tap_ops->ready(NULL, &g_files[i]));
    unsigned ev = pick(4);
    struct waitqueue *wq = g_tap_ops->poll_wq(NULL, &g_files[i], ev);
    note(46, ((uint64_t)ev << 1) | (wq != NULL));
    int was = g_tap_ops->set_nonblock(NULL, &g_files[i], (int)pick(3) - 1);
    note(47, (uint64_t)(int64_t)was);
}

/* A control command: mostly well-formed, otherwise wrong in one field or its length. */
static void op_ctl_write(void)
{
    give_privs();
    uint8_t b[64];
    memset(b, 0, sizeof(b));
    size_t len;
    unsigned k = pick(10);
    if (k < 5) {
        struct cosmo_netctl c;
        memset(&c, 0, sizeof(c));
        c.version = COSMO_NETCTL_VERSION;
        c.op = pick(2) ? COSMO_NETCTL_FORWARD_ADD : COSMO_NETCTL_FORWARD_DEL;
        c.proto = pick(6) == 0 ? COSMO_NETCTL_PROTO_ICMP : pick(2) ? COSMO_NETCTL_PROTO_TCP : COSMO_NETCTL_PROTO_UDP;
        c.host_port = pick(8) == 0 ? 0 : (uint16_t)(1 + pick(60000));
        c.guest_port = pick(8) == 0 ? 0 : (uint16_t)(1 + pick(60000));
        c.guest_addr = pick(8) == 0 ? 0 : IPV4_ADDR(10, 0, 3 + pick(8), 15);
        if (pick(12) == 0)
            c.reserved = 1;
        if (pick(12) == 0)
            c.reserved2 = 1;
        memcpy(b, &c, sizeof(c));
        len = sizeof(c);
    } else if (k < 9) {
        struct cosmo_netctl_filter c;
        memset(&c, 0, sizeof(c));
        c.version = COSMO_NETCTL_VERSION;
        static const uint16_t ops[] = { COSMO_NETCTL_FILTER_ADD, COSMO_NETCTL_FILTER_DEL, COSMO_NETCTL_FILTER_POLICY };
        c.op = ops[pick(3)];
        c.guest_addr = pick(4) == 0 ? COSMO_NETCTL_HOST_ADDR : IPV4_ADDR(10, 0, 3 + pick(8), 15);
        c.direction = (uint8_t)pick(6);
        c.proto = (uint8_t)pick(18);
        c.dst_prefix = (uint8_t)pick(34);
        c.verdict = (uint8_t)pick(3);
        c.dst_addr = (uint32_t)next();
        c.dst_port = (uint16_t)next();
        c.at_index = (uint16_t)pick(40);
        c.src_prefix = (uint8_t)pick(34);
        c.src_addr = (uint32_t)next();
        c.scope = (uint8_t)pick(4);
        if (pick(12) == 0)
            c.reserved[pick(2)] = 1;
        memcpy(b, &c, sizeof(c));
        len = sizeof(c);
    } else {
        fill(b, sizeof(b));
        len = pick(sizeof(b));
    }
    unsigned w = pick(16);
    if (w == 0)
        b[0] ^= 1;   /* the version */
    else if (w == 1)
        b[2] = (uint8_t)next();   /* the op */
    else if (w == 2 && len > 0)
        len -= 1 + pick((unsigned)len);
    else if (w == 3)
        len += 1;
    int64_t rc = g_ctl_ops->write(NULL, 0, b, len);
    note(48, ((uint64_t)len << 32) | (uint32_t)rc);
}

static void op_ctl_read(void)
{
    static uint8_t buf[COSMO_NETCTL_SNAPSHOT_MAX];
    static const unsigned lens[] = { 0, 4, 8, 64, 300, 1024, 4096, COSMO_NETCTL_SNAPSHOT_MAX };
    unsigned len = lens[pick(sizeof(lens) / sizeof(lens[0]))];
    if (len > sizeof(buf))
        len = sizeof(buf);
    memset(buf, 0xee, len);
    g_now += 1000000ull * pick(500);
    int64_t rc = g_ctl_ops->read(NULL, 0, buf, len);
    note(49, ((uint64_t)len << 32) | (uint32_t)rc);
    if (rc > 0) {
        EXPECT((size_t)rc <= len);
        mix(buf, (size_t)rc);
    }
}

/* The tap's own calls, as the self-tests make them. */
static void op_direct(void)
{
    static const uint8_t mac[6] = { 0x02, 0x7a, 0, 0, 0, 0x42 };
    g_fail_register = pick(8) == 0;
    struct tap *t = tap_create("tapd", IPV4_ADDR(10, 88, 0, 1), htonl(0xffffff00u), mac);
    g_fail_register = false;
    note(50, t != NULL);
    if (t == NULL)
        return;
    unsigned tok = 7;
    if (pick(2))
        tap_set_input_filter(t, test_filter, &tok);
    unsigned n = pick(5);
    for (unsigned k = 0; k < n; k++) {
        uint8_t b[1600];
        unsigned len = pick(1600);
        g_fail_cache = pick(10) == 0;   /* no mbuf for it */
        fill(b, len);
        if (len > 14 && pick(2)) {
            b[12] = 0x08;
            b[13] = 0;
        }
        note(51, (uint64_t)(int64_t)tap_inject(t, b, len));
        g_fail_cache = false;
    }
    for (unsigned k = 0; k < pick(4); k++)
        stack_send(tap_netif(t), 60 + pick(100));
    struct mbuf *m;
    while ((m = tap_recv(t)) != NULL) {
        note(52, m_length(m));
        m_freem(m);
    }
    note(53, tap_ready(t));
    note(54, tap_poll_wq(t) != NULL);
    tap_set_input_filter(t, NULL, NULL);
    if (pick(2))
        stack_send(tap_netif(t), 64);   /* left queued: the release drains it */
    tap_destroy(t);
    tap_destroy(NULL);
}

/* ---- the histories ---- */

static void teardown(long baseline)
{
    for (unsigned i = 0; i < NFILE; i++)
        if (g_open[i]) {
            g_tap_ops->release(NULL, &g_files[i]);
            g_open[i] = false;
        }
    EXPECT(g_live_ifs == 0);
    for (unsigned i = 0; i < NFILE; i++)
        EXPECT(g_svc_tokens[i] == 0);
    EXPECT(harness_locks_held() == 0);
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    EXPECT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    EXPECT(g_live == baseline);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    mbuf_init();
    g_fail_mkchr = true;
    tap_dev_init();   /* the nodes not made: the warnings */
    g_fail_mkchr = false;
    tap_dev_init();
    EXPECT(g_tap_ops != NULL && g_ctl_ops != NULL);
    long baseline = g_live;
    for (unsigned h = 0; h < HISTORIES; h++) {
        for (unsigned r = 0; r < OPS; r++) {
            unsigned k = pick(32);
            g_fail_in = pick(16) == 0 ? (int)pick(7) : -1;
            if (k < 3)
                op_open();
            else if (k < 5)
                op_close();
            else if (k < 11)
                op_write();
            else if (k < 15)
                op_transmit();
            else if (k < 20)
                op_read();
            else if (k < 22)
                op_ready();
            else if (k < 27)
                op_ctl_write();
            else if (k < 30)
                op_ctl_read();
            else
                op_direct();
            g_fail_in = -1;
            EXPECT(harness_locks_held() == 0);
        }
        teardown(baseline);
        note(60, h);
    }
    harness_klog_hook = NULL;
    printf("  tap digest 0x%016llx over %u histories of %u operations (%u interfaces released, %u wakes)\n",
           (unsigned long long)g_digest, HISTORIES, OPS, g_released, g_wakes);
    EXPECT(g_digest == TAP_DIGEST);
}

/* TAP_BENCH=1: the median of nine runs of 20,000 rounds on one open tap -- a frame written in (to the stack), a
 * frame the stack sends read out, and the control channel's snapshot read once every 64 rounds. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void tap_bench(void)
{
    g_rng = SEED;
    g_benching = true;
    g_cred.euid = 0;
    g_cred.privs = PRIV_DEVICE | PRIV_NET;
    memset(&g_files[0], 0, sizeof(g_files[0]));
    EXPECT(g_tap_ops->open(NULL, &g_files[0]) == 0);
    struct netif *nif = tap_netif(open_of(0)->tap);
    static uint8_t frame[1514], out[2048], snap[COSMO_NETCTL_SNAPSHOT_MAX];
    memset(frame, 0x5a, sizeof(frame));
    frame[12] = 0x08;
    frame[13] = 0;
    frame[14] = 0x44;   /* even: the filter leaves it to the stack */
    double t[9];
    for (unsigned r = 0; r < 9; r++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            EXPECT(g_tap_ops->write_file(NULL, &g_files[0], 0, frame, sizeof(frame)) == (int64_t)sizeof(frame));
            stack_send(nif, 1514);
            EXPECT(g_tap_ops->read_file(NULL, &g_files[0], 0, out, sizeof(out)) == 1514);
            if (i % 64 == 0)
                EXPECT(g_ctl_ops->read(NULL, 0, snap, sizeof(snap)) > 0);
        }
        t[r] = ns_since(&t0) / 20000;
    }
    qsort(t, 9, sizeof(double), cmp_double);
    g_tap_ops->release(NULL, &g_files[0]);
    g_benching = false;
    printf("  bench a round (a frame in, a frame out, a snapshot every 64) %.1f ns\n", t[4]);
}

static const struct host_test tests[] = {
    { "tap digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("TAP_BENCH") != NULL)
        tap_bench();
    return rc;
}
