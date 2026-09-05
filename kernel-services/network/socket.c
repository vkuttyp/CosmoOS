/*
 * socket.c - The socket layer: blocking semantics over UDP and TCP pcbs,
 * exposed as kobjects for handle tables.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/net/tcp.h>
#include <kernel/sched.h>
#include <kernel/socket.h>
#include <kernel/string.h>

#include <uapi/cosmo/syscall.h>

static uint32_t g_count;

static void socket_release(struct kobject *obj)
{
    struct socket *s = container_of(obj, struct socket, obj);
    if (s->type == COSMO_SOCK_DGRAM) {
        s->udp.sock = NULL;
        udp_unbind(&s->udp);
    } else if (s->tcp) {
        tcp_close(s->tcp);   /* detaches sock; the pcb frees itself */
        s->tcp = NULL;
    }
    __atomic_fetch_sub(&g_count, 1, __ATOMIC_RELAXED);
    kfree(s);
}

static int64_t socket_obj_read(struct kobject *obj, void *buf, size_t len)
{
    return ksock_recvfrom(container_of(obj, struct socket, obj), buf, len, NULL);
}

static int64_t socket_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    return ksock_sendto(container_of(obj, struct socket, obj), buf, len, NULL);
}

static int socket_obj_stat(struct kobject *obj, struct cosmo_stat *st)
{
    (void)obj;
    memset(st, 0, sizeof(*st));
    st->type = COSMO_DT_SOCK;
    st->mode = 0600;
    st->nlink = 1;
    return 0;
}

static unsigned socket_obj_ready(struct kobject *obj)
{
    return ksock_ready(container_of(obj, struct socket, obj));
}

static int socket_obj_set_nonblock(struct kobject *obj, int on)
{
    struct socket *s = container_of(obj, struct socket, obj);
    int was = __atomic_load_n(&s->nonblock, __ATOMIC_RELAXED) ? 1 : 0;
    if (on >= 0)
        ksock_set_nonblock(s, on != 0);
    return was;
}

static const struct kobject_io_type socket_type = {
    .base = { .name = "socket", .release = socket_release, .flags = KOBJECT_TYPE_IO },
    .read = socket_obj_read,
    .write = socket_obj_write,
    .stat = socket_obj_stat,
    .ready = socket_obj_ready,
    .set_nonblock = socket_obj_set_nonblock,
};

struct socket *socket_from_kobject(struct kobject *obj)
{
    return obj->type == &socket_type.base ? container_of(obj, struct socket, obj) : NULL;
}

void socket_init(void)
{
}

static struct socket *alloc_socket(int family, int type, uint32_t uid)
{
    struct socket *s = kzalloc(sizeof(*s));
    if (s == NULL)
        return NULL;
    kobject_init(&s->obj, &socket_type.base);
    s->family = family;
    s->type = type;
    s->uid = uid;
    s->state = SS_UNCONNECTED;
    waitqueue_init(&s->wait, "socket");
    mutex_init(&s->lock, "socket");
    __atomic_fetch_add(&g_count, 1, __ATOMIC_RELAXED);
    return s;
}

int ksock_create(int family, int type, uint32_t uid, struct socket **out)
{
    if (family != COSMO_AF_INET && family != COSMO_AF_INET6)
        return -EAFNOSUPPORT;
    if (type != COSMO_SOCK_STREAM && type != COSMO_SOCK_DGRAM)
        return -EINVAL;
    struct socket *s = alloc_socket(family, type, uid);
    if (s == NULL)
        return -ENOMEM;
    if (type == COSMO_SOCK_DGRAM) {
        udp_pcb_init(&s->udp, (uint16_t)family);
        s->udp.sock = s;
    } else {
        s->tcp = tcp_pcb_new((uint16_t)family);
        if (s->tcp == NULL) {
            ksock_put(s);
            return -ENOMEM;
        }
        s->tcp->sock = s;
    }
    *out = s;
    return 0;
}

void sock_wake(struct socket *s)
{
    waitqueue_wake_all(&s->wait);
}

void sock_set_error(struct socket *s, int err)
{
    s->error = err;
    sock_wake(s);
}

static int take_error(struct socket *s)
{
    int e = s->error;
    s->error = 0;
    if (e == 0 && s->tcp && s->tcp->error) {
        e = s->tcp->error;
    }
    return e;
}

/* A non-blocking connect that finished since connect() returned becomes
 * SS_CONNECTED here (under the socket mutex, like every state change). */
static void settle_connecting(struct socket *s)
{
    mutex_lock(&s->lock);
    if (s->state == SS_CONNECTING) {
        enum tcp_state st = tcp_state_of(s->tcp);
        if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT)
            s->state = SS_CONNECTED;
    }
    mutex_unlock(&s->lock);
}

int ksock_bind(struct socket *s, const struct netaddr *addr)
{
    if (addr->family != s->family)
        return -EAFNOSUPPORT;
    /* Reserved ports are judged on the caller's credentials at bind time,
     * not on who created the socket: a handle inherited from a privileged
     * process, or kept across setresuid, confers nothing. */
    if (addr->port != 0 && addr->port < 1024 && !cred_privileged(cred_current()))
        return -EPERM;
    mutex_lock(&s->lock);
    int rc;
    if (s->state != SS_UNCONNECTED) {
        rc = -EINVAL;
    } else {
        rc = s->type == COSMO_SOCK_DGRAM ? udp_bind(&s->udp, addr) : tcp_bind(s->tcp, addr);
        if (rc == 0)
            s->state = SS_BOUND;
    }
    mutex_unlock(&s->lock);
    return rc;
}

int ksock_listen(struct socket *s, int backlog)
{
    if (s->type != COSMO_SOCK_STREAM)
        return -EOPNOTSUPP;
    mutex_lock(&s->lock);
    int rc;
    if (s->state != SS_BOUND) {
        rc = -EINVAL;
    } else {
        rc = tcp_listen(s->tcp, backlog < 0 ? 0 : (unsigned)backlog);
        if (rc == 0)
            s->state = SS_LISTENING;
    }
    mutex_unlock(&s->lock);
    return rc;
}

int ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer)
{
    if (s->type != COSMO_SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state != SS_LISTENING)
        return -EINVAL;
    /* The child socket exists before the pcb is dequeued, so the pcb is
     * never without an owner (tcp_accept attaches under the TCP lock). */
    struct socket *c = alloc_socket(s->family, COSMO_SOCK_STREAM, s->uid);
    if (c == NULL)
        return -ENOMEM;
    struct tcp_pcb *child;
    for (;;) {
        child = tcp_accept(s->tcp, c);
        if (child)
            break;
        if (s->error || (s->shut & 1)) {
            ksock_put(c);
            return take_error(s) ? take_error(s) : -EINVAL;
        }
        if (s->nonblock) {
            ksock_put(c);
            return -EAGAIN;
        }
        int w = wait_event_killable(&s->wait, tcp_accept_ready(s->tcp) || s->error || (s->shut & 1));
        if (w) {
            ksock_put(c);
            return w;
        }
    }
    c->tcp = child;
    c->state = SS_CONNECTED;
    if (peer)
        *peer = child->remote;
    *out = c;
    return 0;
}

int ksock_connect(struct socket *s, const struct netaddr *addr)
{
    if (addr->family != s->family)
        return -EAFNOSUPPORT;
    mutex_lock(&s->lock);
    int rc;
    if (s->type == COSMO_SOCK_DGRAM) {
        s->udp.remote = *addr;
        s->state = SS_CONNECTED;
        rc = 0;
    } else if (s->state == SS_CONNECTED) {
        rc = -EISCONN;
    } else if (s->state == SS_LISTENING) {
        rc = -EINVAL;
    } else if (s->state == SS_CONNECTING) {
        /* A non-blocking connect in progress, or finished since. */
        enum tcp_state st = tcp_state_of(s->tcp);
        if (st == TCP_SYN_SENT || st == TCP_SYN_RCVD) {
            rc = -EALREADY;
        } else if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
            s->state = SS_CONNECTED;
            rc = -EISCONN;
        } else {
            rc = take_error(s);
            if (rc == 0)
                rc = -ECONNREFUSED;
            s->state = SS_UNCONNECTED;
        }
    } else {
        rc = tcp_connect(s->tcp, addr);
        if (rc == 0 && s->nonblock) {
            enum tcp_state st = tcp_state_of(s->tcp);
            if (st == TCP_SYN_SENT || st == TCP_SYN_RCVD) {
                s->state = SS_CONNECTING;
                rc = -EINPROGRESS;
            } else if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
                s->state = SS_CONNECTED;
            } else {
                rc = take_error(s);
                if (rc == 0)
                    rc = -ECONNREFUSED;
            }
        } else if (rc == 0) {
            s->state = SS_CONNECTING;
            mutex_unlock(&s->lock);
            int w = wait_event_killable(&s->wait,
                                        tcp_state_of(s->tcp) != TCP_SYN_SENT && tcp_state_of(s->tcp) != TCP_SYN_RCVD);
            mutex_lock(&s->lock);
            if (w) {
                mutex_unlock(&s->lock);
                return w;   /* being killed; the pcb finishes or times out on its own */
            }
            if (tcp_state_of(s->tcp) == TCP_ESTABLISHED || tcp_state_of(s->tcp) == TCP_CLOSE_WAIT) {
                s->state = SS_CONNECTED;
            } else {
                rc = take_error(s);
                if (rc == 0)
                    rc = -ECONNREFUSED;
                s->state = SS_UNCONNECTED;
            }
        }
    }
    mutex_unlock(&s->lock);
    return rc;
}

int64_t ksock_sendto(struct socket *s, const void *buf, size_t len, const struct netaddr *to)
{
    if (s->shut & 2)
        return -EPIPE;
    if (s->type == COSMO_SOCK_DGRAM) {
        const struct netaddr *dst = to;
        if (dst == NULL) {
            if (s->state != SS_CONNECTED)
                return -ENOTCONN;
            dst = &s->udp.remote;
        }
        mutex_lock(&s->lock);
        int rc = udp_sendto(&s->udp, buf, len, dst);
        if (rc == 0 && s->state == SS_UNCONNECTED)
            s->state = SS_BOUND;
        mutex_unlock(&s->lock);
        return rc ? rc : (int64_t)len;
    }
    if (to != NULL)
        return -EISCONN;
    if (s->state == SS_CONNECTING)
        settle_connecting(s);
    if (s->state != SS_CONNECTED)
        return -ENOTCONN;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        int64_t n = tcp_send(s->tcp, p + done, len - done);
        if (n < 0) {
            if (n == -EAGAIN)
                n = 0;
            else
                return done ? (int64_t)done : n;
        }
        done += (size_t)n;
        if (done < len) {
            if (s->nonblock)
                return done ? (int64_t)done : -EAGAIN;
            int w = wait_event_killable(&s->wait, tcp_send_space(s->tcp) > 0 || s->tcp->error ||
                                                      tcp_state_of(s->tcp) == TCP_CLOSED);
            if (w)
                return done ? (int64_t)done : w;
            if (s->tcp->error || tcp_state_of(s->tcp) == TCP_CLOSED)
                return done ? (int64_t)done : (s->tcp->error ? s->tcp->error : -EPIPE);
        }
    }
    return (int64_t)done;
}

int64_t ksock_recvfrom(struct socket *s, void *buf, size_t len, struct netaddr *from)
{
    if (s->type == COSMO_SOCK_DGRAM) {
        if (s->udp.local.port == 0)
            return -EINVAL;   /* unbound: nothing can arrive */
        struct mbuf *m;
        for (;;) {
            m = udp_recv(&s->udp);
            if (m)
                break;
            if (s->shut & 1)
                return 0;
            if (s->error)
                return take_error(s);
            if (s->nonblock)
                return -EAGAIN;
            int w = wait_event_killable(&s->wait, mbufq_len(&s->udp.rxq) > 0 || s->error || (s->shut & 1));
            if (w)
                return w;
        }
        uint32_t n = m->pkt.len < len ? m->pkt.len : (uint32_t)len;
        m_copydata(m, 0, n, buf);
        if (from)
            *from = m->pkt.src;
        m_freem(m);
        return (int64_t)n;
    }
    if (s->state == SS_CONNECTING)
        settle_connecting(s);
    if (s->state != SS_CONNECTED)
        return -ENOTCONN;
    for (;;) {
        bool closed = false;
        int64_t n = tcp_recv(s->tcp, buf, len, &closed);
        if (n > 0) {
            if (from)
                *from = s->tcp->remote;
            return n;
        }
        if (n < 0)
            return n;
        if (closed || (s->shut & 1))
            return 0;
        if (s->tcp->state == TCP_CLOSED)
            return s->tcp->error ? s->tcp->error : 0;
        if (s->nonblock)
            return -EAGAIN;
        int w = wait_event_killable(&s->wait, tcp_recv_avail(s->tcp) > 0 || s->tcp->fin_rcvd || s->tcp->error ||
                                                  s->tcp->state == TCP_CLOSED || (s->shut & 1));
        if (w)
            return w;
    }
}

int ksock_shutdown(struct socket *s, int how)
{
    if (how < COSMO_SHUT_RD || how > COSMO_SHUT_RDWR)
        return -EINVAL;
    mutex_lock(&s->lock);
    int rc = 0;
    if (how == COSMO_SHUT_RD || how == COSMO_SHUT_RDWR)
        s->shut |= 1;
    if (how == COSMO_SHUT_WR || how == COSMO_SHUT_RDWR) {
        s->shut |= 2;
        if (s->type == COSMO_SOCK_STREAM && s->state == SS_CONNECTED)
            rc = tcp_shutdown_write(s->tcp);
    }
    mutex_unlock(&s->lock);
    sock_wake(s);
    return rc;
}

int ksock_getsockname(struct socket *s, struct netaddr *out)
{
    *out = s->type == COSMO_SOCK_DGRAM ? s->udp.local : s->tcp->local;
    return 0;
}

int ksock_getpeername(struct socket *s, struct netaddr *out)
{
    if (s->state != SS_CONNECTED)
        return -ENOTCONN;
    *out = s->type == COSMO_SOCK_DGRAM ? s->udp.remote : s->tcp->remote;
    return 0;
}

void ksock_set_nonblock(struct socket *s, bool on)
{
    __atomic_store_n(&s->nonblock, on, __ATOMIC_RELAXED);
}

unsigned ksock_ready(struct socket *s)
{
    unsigned r = 0;
    if (s->type == COSMO_SOCK_DGRAM) {
        if (mbufq_len(&s->udp.rxq) > 0)
            r |= COSMO_IO_READABLE;
        r |= COSMO_IO_WRITABLE;
    } else {
        r = tcp_ready(s->tcp);
        enum socket_state st = s->state;
        if (st != SS_CONNECTED && st != SS_LISTENING && st != SS_CONNECTING)
            r |= COSMO_IO_WRITABLE;   /* a write fails at once with -ENOTCONN */
    }
    if (s->error)
        r |= COSMO_IO_READABLE | COSMO_IO_WRITABLE | COSMO_IO_ERROR;
    if (s->shut & 1)
        r |= COSMO_IO_READABLE | COSMO_IO_HANGUP;
    if (s->shut & 2)
        r |= COSMO_IO_WRITABLE;
    return r;
}

unsigned socket_count(void)
{
    return __atomic_load_n(&g_count, __ATOMIC_RELAXED);
}
