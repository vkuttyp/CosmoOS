/*
 * tap.c - A tap interface (tap.h): a netif whose far end is userland.
 *
 * transmit enqueues the frame for the far end to read; tap_inject hands a
 * frame from the far end to netif_rx. The stack does everything else --
 * ARP, IP, the transport -- over an interface whose driver is a reader and
 * a writer rather than a device.
 */
#include <kernel/net/tap.h>

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/printf.h>
#include <kernel/fwcfg.h>
#include <kernel/log.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tapsvc.h>
#include <uapi/cosmo/netctl.h>
#include <kernel/netif.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#define TAP_TXQ_MAX 64u   /* frames the stack has queued for the reader; drops when full */

struct tap {
    struct netif nif;
    struct mbufq txq;      /* stack -> far end: frames transmitted out the tap */
    tap_input_fn in_filter;   /* claims far-end frames before the stack (tapsvc: DHCP) */
    void *in_arg;
};

/* Stack -> tap: the stack is sending a frame out the interface. Queue it for
 * the far end; drop (freeing) when the queue is full, as a NIC drops when it
 * has nowhere to put a frame. Runs in a read-side section: no sleeping. */
static int tap_transmit(struct netif *nif, struct mbuf *m)
{
    struct tap *t = container_of(nif, struct tap, nif);
    if (!mbufq_enqueue(&t->txq, m))
        m_freem(m);        /* "sent" and dropped */
    return 0;
}

static void tap_release(struct netif *nif)
{
    struct tap *t = container_of(nif, struct tap, nif);
    mbufq_drain(&t->txq);
    kfree(t);
}

static const struct netif_ops tap_ops = { .transmit = tap_transmit, .release = tap_release };

struct tap *tap_create(const char *name, uint32_t ip, uint32_t mask, const uint8_t mac[6])
{
    struct tap *t = kmalloc(sizeof(*t), KMEM_ZERO);
    if (t == NULL)
        return NULL;
    strlcpy(t->nif.name, name, sizeof(t->nif.name));
    memcpy(t->nif.mac, mac, 6);
    t->nif.mtu = 1500;
    /* A tap is a point-to-point link to one owner, never the machine's route
     * to the world: keep it out of default-interface selection, so a tap can
     * never swallow the host's outbound traffic, whatever the registration
     * order (and whatever the lifetime -- a tap lives only while its owner
     * holds the channel, destroyed on the last close). */
    t->nif.flags = NETIF_NODEFAULT;
    t->nif.ops = &tap_ops;
    mbufq_init(&t->txq, TAP_TXQ_MAX, "tap-tx");
    if (netif_register(&t->nif) != 0) {
        kfree(t);
        return NULL;
    }
    netif_set_ipv4(&t->nif, ip, mask, 0);
    netif_set_up(&t->nif, true);
    return t;
}

void tap_destroy(struct tap *t)
{
    if (t == NULL)
        return;
    netif_unregister(&t->nif);   /* down and gone; drops the registry's reference */
    netif_put(&t->nif);          /* drop the creator's reference -> release frees t */
}

int tap_inject(struct tap *t, const void *frame, uint32_t len)
{
    if (len < ETH_HLEN || len > t->nif.mtu + ETH_HLEN)
        return -EMSGSIZE;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    if (len > m_trailingspace(m)) {   /* a cluster holds a full frame; this cannot happen */
        m_freem(m);
        return -EMSGSIZE;
    }
    memcpy(m->data, frame, len);
    m->len = m->pkt.len = len;
    if (t->in_filter && t->in_filter(t, m->data, len, t->in_arg)) {
        m_freem(m);                  /* the filter claimed it (and copied what it needed) */
        return 0;
    }
    netif_rx(&t->nif, m);            /* into the stack, as a driver's completion would */
    return 0;
}

struct mbuf *tap_recv(struct tap *t)
{
    return mbufq_dequeue(&t->txq);
}

struct netif *tap_netif(struct tap *t)
{
    return &t->nif;
}

void tap_set_input_filter(struct tap *t, tap_input_fn fn, void *arg)
{
    t->in_arg = arg;
    t->in_filter = fn;   /* arg set first: the filter sees a consistent (fn, arg) */
}

/* --- /dev/net/tap: a tap per open, the owner's frame channel ------------- */

/* Each open of /dev/net/tap is one guest: it gets a tap of its own on a
 * subnet of its own from the pool 10.0.(3+k).0/24 (host .1, guest .15 -- the
 * convention tap0 set), forwarding and masquerade on, and its own DHCP/DNS
 * service; the last close stops the service and destroys the tap. A tap
 * exists exactly while an owner holds the channel (docs/audit/
 * next-subsystem-multiguest.md). */
#define TAP_MAX_GUESTS 8u
static bool g_tap_slot[TAP_MAX_GUESTS];
static spinlock_t g_tap_slot_lock = SPINLOCK_INIT("tap-slots");
static struct vnode *g_tapnode;

struct tap_open {                 /* struct file::priv for /dev/net/tap */
    struct tap *tap;
    struct tapsvc *svc;
    unsigned slot;
};

static int tap_chr_open(struct vnode *vn, struct file *f)
{
    (void)vn;
    arch_irq_state_t s = spin_lock_irqsave(&g_tap_slot_lock);
    int slot = -1;
    for (unsigned i = 0; i < TAP_MAX_GUESTS; i++)
        if (!g_tap_slot[i]) { g_tap_slot[i] = true; slot = (int)i; break; }
    spin_unlock_irqrestore(&g_tap_slot_lock, s);
    if (slot < 0)
        return -ENOSPC;               /* the ninth guest */

    struct tap_open *o = kmalloc(sizeof(*o), KMEM_ZERO);
    if (o == NULL)
        goto fail_slot;
    char name[8];
    ksnprintf(name, sizeof(name), "tap%d", slot);
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, (uint8_t)(0xcc + slot) };
    o->slot = (unsigned)slot;
    o->tap = tap_create(name, IPV4_ADDR(10, 0, 3 + slot, 1), htonl(0xffffff00u), mac);
    if (o->tap == NULL)
        goto fail_open;
    /* The guest's ingress: forward its packets, masquerade them out the
     * host's real interface. The flags are the tap's, never the NIC's. */
    struct netif *nif = tap_netif(o->tap);
    netif_set_forward(nif, true);
    netif_set_masquerade(nif, true);
    o->svc = tapsvc_start(o->tap);
    if (o->svc == NULL)
        goto fail_tap;
    /* Boot-time port-forwards (fw_cfg) name a guest by address; apply the
     * ones that land on this tap now that it exists (others are skipped). */
    char pf[128];
    if (fwcfg_get_string("portforward", pf, sizeof(pf)))
        nat_portforward_apply(pf);
    f->priv = o;
    return 0;

fail_tap:
    tap_destroy(o->tap);
fail_open:
    kfree(o);
fail_slot:
    s = spin_lock_irqsave(&g_tap_slot_lock);
    g_tap_slot[slot] = false;
    spin_unlock_irqrestore(&g_tap_slot_lock, s);
    return -ENOMEM;
}

/* Last close: stop the service first (its threads and DHCP filter must be
 * gone before the tap they point at), then the tap, then free the subnet. */
static void tap_chr_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct tap_open *o = f->priv;
    if (o == NULL)
        return;
    uint32_t guest = (tap_netif(o->tap)->ip4.addr & tap_netif(o->tap)->ip4.mask) | htonl(15u);
    tapsvc_stop(o->svc);
    nat_guest_purge(guest);    /* no stale rules/flows for a reused subnet */
    tap_destroy(o->tap);
    arch_irq_state_t s = spin_lock_irqsave(&g_tap_slot_lock);
    g_tap_slot[o->slot] = false;
    spin_unlock_irqrestore(&g_tap_slot_lock, s);
    kfree(o);
    f->priv = NULL;
}

/* Read one frame the stack transmitted out this owner's tap, or 0 when none
 * waits (a frame is never zero-length; the owner polls). Never blocks. */
static int64_t tap_chr_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn; (void)off;
    struct tap_open *o = f->priv;
    struct mbuf *m = tap_recv(o->tap);
    if (m == NULL)
        return 0;
    uint32_t fl = m_length(m);
    if (fl > len) {          /* the owner must offer a frame-sized buffer */
        m_freem(m);
        return -EMSGSIZE;
    }
    m_copydata(m, 0, fl, buf);
    m_freem(m);
    return (int64_t)fl;
}

/* Inject one frame from this owner's guest into the stack. */
static int64_t tap_chr_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn; (void)off;
    struct tap_open *o = f->priv;
    int rc = tap_inject(o->tap, buf, (uint32_t)len);
    return rc ? rc : (int64_t)len;
}

static const struct chrdev_ops tap_chr_ops = {
    .open = tap_chr_open, .release = tap_chr_release,
    .read_file = tap_chr_read_file, .write_file = tap_chr_write_file,
};

/* --- /dev/net/tapctl: the owner's runtime network-control channel -------- */

static struct vnode *g_ctlnode;

/* A privileged owner writes one struct cosmo_netctl to add or remove a
 * port-forward (DNAT) rule. The command is applied whole or refused. */
static int64_t tap_ctl_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn; (void)off;
    if (len != sizeof(struct cosmo_netctl))
        return -EINVAL;                         /* a command is exactly one struct, applied whole */
    struct cosmo_netctl cmd;
    memcpy(&cmd, buf, sizeof(cmd));
    if (cmd.version != COSMO_NETCTL_VERSION)
        return -ENOTSUP;
    if (cmd.reserved != 0 || cmd.reserved2 != 0)
        return -EINVAL;
    if (cmd.proto != COSMO_NETCTL_PROTO_TCP && cmd.proto != COSMO_NETCTL_PROTO_UDP)
        return -EINVAL;
    uint8_t proto = cmd.proto == COSMO_NETCTL_PROTO_TCP ? IPPROTO_TCP : IPPROTO_UDP;

    switch (cmd.op) {
    case COSMO_NETCTL_FORWARD_ADD:
        if (cmd.host_port == 0 || cmd.guest_port == 0 || cmd.guest_addr == 0)
            return -EINVAL;
        {
            int rc = nat_pf_add(proto, cmd.host_port, cmd.guest_addr, cmd.guest_port);
            if (rc != 0)
                return rc;                      /* -EINVAL off-tap, -EEXIST dup, -ENOSPC full */
        }
        return (int64_t)sizeof(cmd);
    case COSMO_NETCTL_FORWARD_DEL:
        if (cmd.host_port == 0)
            return -EINVAL;
        if (!nat_pf_del(proto, cmd.host_port))
            return -ENOENT;
        return (int64_t)sizeof(cmd);
    default:
        return -EINVAL;
    }
}

/* One read returns the whole snapshot: a struct cosmo_netctl_list header and
 * its rules, or -EMSGSIZE if the buffer is too small (no partial read). */
static int64_t tap_ctl_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn; (void)off;
    struct nat_pf_rule rules[NAT_PF_MAX];
    unsigned n = nat_pf_list(rules, NAT_PF_MAX);
    size_t total = sizeof(struct cosmo_netctl_list) + (size_t)n * sizeof(struct cosmo_netctl_rule);
    if (len < total)
        return -EMSGSIZE;
    struct cosmo_netctl_list hdr = { .version = COSMO_NETCTL_VERSION, .count = (uint16_t)n };
    memcpy(buf, &hdr, sizeof(hdr));
    struct cosmo_netctl_rule *out = (struct cosmo_netctl_rule *)((uint8_t *)buf + sizeof(hdr));
    for (unsigned i = 0; i < n; i++) {
        out[i].proto = rules[i].proto == IPPROTO_TCP ? COSMO_NETCTL_PROTO_TCP : COSMO_NETCTL_PROTO_UDP;
        out[i].reserved = 0;
        out[i].host_port = rules[i].host_port;
        out[i].guest_port = rules[i].guest_port;
        out[i].reserved2 = 0;
        out[i].guest_addr = rules[i].guest_ip;
    }
    return (int64_t)total;
}

static const struct chrdev_ops tap_ctl_ops = { .read = tap_ctl_read, .write = tap_ctl_write };

void tap_dev_init(void)
{
    vfs_mkdir(NULL, "/dev/net", 0755);           /* -EEXIST is fine */
    int rc = ramfs_mkchr("/dev/net/tap", 0600, &tap_chr_ops, NULL, &g_tapnode);
    if (rc)
        kwarn("tap: cannot create /dev/net/tap (%d)", rc);
    rc = ramfs_mkchr("/dev/net/tapctl", 0600, &tap_ctl_ops, NULL, &g_ctlnode);
    if (rc)
        kwarn("tap: cannot create /dev/net/tapctl (%d)", rc);
}
