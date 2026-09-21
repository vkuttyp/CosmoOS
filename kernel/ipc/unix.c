/*
 * unix.c - Unix domain sockets (docs/kernel/ipc/design.md, "Unix domain
 * sockets"; docs/audit/next-subsystem-unix-sockets.md).
 *
 * A stream connection is one `struct unix_conn`: two queues of sends,
 * one read by each side, each bounded by UNIX_BUF bytes, under one
 * spinlock never held while blocking or touching user memory (the
 * pipe's rule). A datagram socket owns one such queue. A send is one
 * `struct umsg` -- its bytes, the sender's name, and the handles that
 * rode with it, whose references the message owns while it is queued.
 * On a stream a read consumes bytes across messages but stops at the
 * boundary of a message that carries handles, so ancillary items stay
 * with the bytes they were sent with.
 *
 * Names: a bound socket holds one vnode reference -- its node in the
 * filesystem, or the caller's root for an abstract name -- and one entry
 * in the registry below, keyed by that vnode (and the bytes, for an
 * abstract name), for exactly as long as it lives. The node's mode is
 * the access control for connecting to it; an abstract name is visible
 * only to processes with the same root.
 *
 * Locking: the socket's mutex (state changes: bind, listen, connect,
 * accept), then the registry spinlock, then a conn's or a queue's
 * spinlock; never the reverse. A registry lookup takes a reference to
 * what it finds and drops the registry lock before touching that
 * socket's mutex. Handles are installed in the receiver's table after
 * the message has left its queue and the queue lock has been dropped.
 */
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/socket.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/uaccess.h>
#include <kernel/unix.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>
#include <uapi/cosmo/syscall.h>

/* One send. */
struct umsg {
    struct list_node link;
    size_t len, off;               /* off: bytes of it already read (a stream) */
    struct unix_addr from;         /* a datagram: the sender's name */
    struct unix_handles h;         /* what rode with it */
    uint8_t data[];
};

/* A bounded queue of sends, one direction. */
struct uqueue {
    struct list_node msgs;
    size_t bytes;
    unsigned count;
    bool wr_closed;                /* nothing more arrives: the writer is gone or shut down */
    bool rd_closed;                /* nobody reads: the reader is gone or shut down */
};

/* A stream connection: two ends, two queues. q[i] is read by side i and
 * written by side 1 - i. wq[i] is side i's socket wait queue, NULL once
 * that side has been released. Freed when both ends have let go. */
struct unix_conn {
    spinlock_t lock;
    unsigned ends;
    struct uqueue q[2];
    struct waitqueue *wq[2];
};

struct unix_sock {
    struct socket *sock;
    /* The name, and what holds it: the node (a path) or the root (an
     * abstract name), referenced from bind to release. */
    struct unix_addr name;
    struct vnode *held;
    struct list_node reg_link;
    bool registered;
    /* A stream. */
    struct unix_conn *conn;
    int side;
    bool listening;
    unsigned backlog, queued;
    struct list_node acceptq;      /* server-side sockets made at connect, referenced by the queue */
    struct list_node accept_link;  /* this socket's place in a listener's queue */
    struct cosmo_ucred cred;       /* a listener: its owner at listen(); connected: the peer's */
    bool has_cred;
    struct unix_addr peer_name;    /* a connected stream socket: the other end's name at connect */
    /* A datagram socket. */
    spinlock_t qlock;
    struct uqueue rxq;
    struct unix_sock *dgram_peer;  /* connect's default destination: a pointer, not a reference (see below) */
    struct list_node peer_link;    /* in g_peered while dgram_peer is set */
};

static void uq_init(struct uqueue *q)
{
    list_init(&q->msgs);
    q->bytes = 0;
    q->count = 0;
    q->wr_closed = false;
    q->rd_closed = false;
}

/*
 * The registry of bound sockets, and the list of datagram sockets with a
 * default destination. A datagram peer is a pointer and not a reference:
 * two sockets connected to each other (a socketpair) would otherwise hold
 * each other alive forever, since the handle to a socket is the only thing
 * that releases it. A sender takes a reference with kobject_tryget under
 * this lock, and a socket being released clears every pointer to itself
 * under the same lock, so a peer is either found alive or not found.
 */
static spinlock_t g_reg_lock = SPINLOCK_INIT("unix-registry");
static struct list_node g_reg = LIST_HEAD_INIT(g_reg);
static struct list_node g_peered = LIST_HEAD_INIT(g_peered);
static unsigned g_count;

/*
 * A waiter on ANOTHER socket -- a connector on a full backlog, a datagram
 * sender on a full queue -- holds no reference to it while it waits.
 * Holding one would keep the socket alive past its last handle, and the
 * wait would then depend on a release the wait itself prevents. Instead
 * every such waiter sleeps on this one queue for a change of generation,
 * bumped whenever room may have appeared or a socket has gone, and
 * resolves the name again when it wakes: a released listener is not
 * found, and the connect is refused.
 */
static struct waitqueue g_room_wq = WAITQUEUE_INIT(g_room_wq);
static unsigned g_room_gen;

static void room_changed(void)
{
    __atomic_fetch_add(&g_room_gen, 1u, __ATOMIC_RELEASE);
    waitqueue_wake_all(&g_room_wq);
}

static bool name_eq(const struct unix_addr *a, const struct unix_addr *b)
{
    return a->abstract == b->abstract && a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* Under g_reg_lock: the bound socket keyed by `key` (its node, or the
 * root plus the bytes for an abstract name), referenced, or NULL. */
static struct unix_sock *reg_find_locked(struct vnode *key, const struct unix_addr *a)
{
    struct unix_sock *u;
    list_for_each_entry(u, &g_reg, reg_link) {
        if (u->held != key)
            continue;
        if (a->abstract && !name_eq(&u->name, a))
            continue;
        ksock_get(u->sock);
        return u;
    }
    return NULL;
}

static void wake_sock(struct socket *s)
{
    waitqueue_wake_all(&s->wait);
}

static void umsg_free(struct umsg *m)
{
    unix_handles_drop(&m->h);
    kfree(m);
}

void unix_handles_drop(struct unix_handles *h)
{
    if (h == NULL)
        return;
    for (unsigned i = 0; i < h->nr; i++)
        kobject_put(h->objs[i]);
    h->nr = 0;
}

/* Under the queue's lock: drop every message; the handles are put after
 * the lock is released, by the caller, so a release never runs under a
 * spinlock. Moves the messages onto `gone`. */
static void uq_drain_locked(struct uqueue *q, struct list_node *gone)
{
    while (!list_empty(&q->msgs)) {
        struct list_node *n = list_pop_front(&q->msgs);
        list_push_back(gone, n);
    }
    q->bytes = 0;
    q->count = 0;
}

static void free_gone(struct list_node *gone)
{
    while (!list_empty(gone)) {
        struct umsg *m = container_of(list_pop_front(gone), struct umsg, link);
        umsg_free(m);
    }
}

/* --- creation and release --------------------------------------------- */

int unix_create(struct socket *s)
{
    struct unix_sock *u = kzalloc(sizeof(*u));
    if (u == NULL)
        return -ENOMEM;
    u->sock = s;
    list_init(&u->reg_link);
    list_init(&u->acceptq);
    list_init(&u->accept_link);
    list_init(&u->peer_link);
    spinlock_init(&u->qlock, "unix-dgram");
    uq_init(&u->rxq);
    s->un = u;
    __atomic_fetch_add(&g_count, 1, __ATOMIC_RELAXED);
    return 0;
}

static void conn_put(struct unix_conn *c, int side)
{
    struct list_node gone = LIST_HEAD_INIT(gone);
    arch_irq_state_t st = spin_lock_irqsave(&c->lock);
    c->wq[side] = NULL;
    /* The other end reads end-of-stream and writes -EPIPE from now on. */
    c->q[1 - side].wr_closed = true;
    c->q[side].rd_closed = true;
    uq_drain_locked(&c->q[side], &gone);   /* unread bytes and handles sent to this side */
    struct waitqueue *peer = c->wq[1 - side];
    if (peer)
        waitqueue_wake_all(peer);
    unsigned left = --c->ends;
    spin_unlock_irqrestore(&c->lock, st);
    free_gone(&gone);
    if (left == 0)
        kfree(c);
}

void unix_release(struct socket *s)
{
    struct unix_sock *u = s->un;
    if (u == NULL)
        return;
    /* The name goes first: nothing can find this socket once it is gone. */
    arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
    if (u->registered) {
        list_remove(&u->reg_link);
        u->registered = false;
    }
    if (!list_empty(&u->peer_link))
        list_remove(&u->peer_link);
    /* Every datagram socket whose default destination this was. */
    struct unix_sock *p;
    list_for_each_entry(p, &g_peered, peer_link) {
        if (p->dgram_peer == u)
            p->dgram_peer = NULL;
    }
    spin_unlock_irqrestore(&g_reg_lock, st);
    if (u->held) {
        vnode_put(u->held);
        u->held = NULL;
    }
    room_changed();   /* a connector or sender waiting on this socket resolves it again and fails */
    /* A listener refuses what it never accepted: each queued server-side
     * socket is released, which closes its end of the connection, and
     * the client reads end-of-stream. */
    while (!list_empty(&u->acceptq)) {
        struct unix_sock *c = container_of(list_pop_front(&u->acceptq), struct unix_sock, accept_link);
        ksock_put(c->sock);
    }
    if (u->conn)
        conn_put(u->conn, u->side);
    /* A datagram socket's queue, with the handles in it. */
    struct list_node gone = LIST_HEAD_INIT(gone);
    st = spin_lock_irqsave(&u->qlock);
    u->rxq.rd_closed = true;
    uq_drain_locked(&u->rxq, &gone);
    spin_unlock_irqrestore(&u->qlock, st);
    free_gone(&gone);
    kfree(u);
    s->un = NULL;
    __atomic_fetch_sub(&g_count, 1, __ATOMIC_RELAXED);
}

unsigned unix_socket_count(void)
{
    return __atomic_load_n(&g_count, __ATOMIC_RELAXED);
}

/* --- names -------------------------------------------------------------- */

int unix_addr_parse(const char *path, size_t plen, struct unix_addr *out)
{
    memset(out, 0, sizeof(*out));
    if (plen == 0)
        return 0;   /* the family alone: no name */
    if (plen > UNIX_PATH_MAX)
        return -EINVAL;
    if (path[0] == '\0') {
        out->abstract = true;
        out->len = (uint8_t)(plen - 1);
        memcpy(out->bytes, path + 1, plen - 1);
        return 0;
    }
    size_t n = strnlen(path, plen);   /* NUL-terminated, or exactly as long as the length says */
    out->len = (uint8_t)n;
    memcpy(out->bytes, path, n);
    return 0;
}

int unix_addr_from_user(uint64_t uptr, size_t len, struct unix_addr *out)
{
    struct cosmo_sockaddr_un un;   /* the same layout at both doors: a 16-bit family, 108 bytes of path */
    if (len < sizeof(un.family) || len > sizeof(un))
        return -EINVAL;
    if (copy_from_user(&un, uptr, len))
        return -EFAULT;
    return unix_addr_parse(un.path, len - sizeof(un.family), out);
}

size_t unix_addr_pack(const struct unix_addr *a, uint16_t family, void *out)
{
    uint8_t *p = out;
    memset(p, 0, 2 + UNIX_PATH_MAX);
    memcpy(p, &family, 2);
    if (a->len == 0)
        return 2;
    if (a->abstract) {
        memcpy(p + 3, a->bytes, a->len);
        return 2 + 1 + a->len;
    }
    memcpy(p + 2, a->bytes, a->len);
    return 2 + a->len + 1;   /* and the NUL */
}

static void cred_now(struct cosmo_ucred *out)
{
    const struct credentials *c = cred_current();
    struct process *p = process_current();
    out->pid = p ? (int32_t)p->pid : 0;
    out->uid = c->euid;
    out->gid = c->egid;
}

int unix_bind(struct socket *s, const struct unix_addr *a)
{
    struct unix_sock *u = s->un;
    if (a->len == 0)
        return -EINVAL;
    mutex_lock(&s->lock);
    if (u->name.len != 0 || s->state != SS_UNCONNECTED) {
        mutex_unlock(&s->lock);
        return -EINVAL;
    }
    struct vnode *key;
    int rc = 0;
    if (a->abstract) {
        key = vfs_current_root();
    } else {
        /* The node: made by the caller's cwd and root as open(O_CREAT)
         * would, mode 0755 (no umask exists here), owned by the caller.
         * A name that exists is in use, whether or not a socket is still
         * behind it -- the program unlinks it first, as on Linux. */
        char path[UNIX_PATH_MAX + 1];
        memcpy(path, a->bytes, a->len);
        path[a->len] = '\0';
        struct vnode *cwd = process_cwd_get();
        rc = vfs_mknod(cwd, path, 0755, VNODE_SOCK, &key);
        if (cwd)
            vnode_put(cwd);
        if (rc == -EEXIST)
            rc = -EADDRINUSE;
        if (rc) {
            mutex_unlock(&s->lock);
            return rc;
        }
    }
    arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
    if (a->abstract) {
        struct unix_sock *other = reg_find_locked(key, a);
        if (other) {
            spin_unlock_irqrestore(&g_reg_lock, st);
            ksock_put(other->sock);
            vnode_put(key);
            mutex_unlock(&s->lock);
            return -EADDRINUSE;
        }
    }
    u->name = *a;
    u->held = key;
    list_push_back(&g_reg, &u->reg_link);
    u->registered = true;
    spin_unlock_irqrestore(&g_reg_lock, st);
    s->state = SS_BOUND;
    mutex_unlock(&s->lock);
    return 0;
}

/*
 * The socket a name reaches, referenced, or an error: -ECONNREFUSED for a
 * node that is not a socket or has no socket behind it (as Linux), the
 * lookup's own error for a path that does not resolve, -EACCES for a node
 * the caller may not write. Resolved from the caller's cwd and root, so a
 * jail names nothing outside itself; an abstract name is looked up under
 * the caller's root only.
 */
static int unix_resolve(const struct unix_addr *a, struct unix_sock **out)
{
    if (a->len == 0)
        return -EINVAL;
    struct vnode *key;
    if (a->abstract) {
        key = vfs_current_root();
    } else {
        char path[UNIX_PATH_MAX + 1];
        memcpy(path, a->bytes, a->len);
        path[a->len] = '\0';
        struct vnode *cwd = process_cwd_get();
        int rc = vfs_lookup(cwd, path, &key);
        if (cwd)
            vnode_put(cwd);
        if (rc)
            return rc;
        if (key->type != VNODE_SOCK) {
            vnode_put(key);
            return -ECONNREFUSED;
        }
        rc = vfs_permission(key, VFS_MAY_WRITE);
        if (rc) {
            vnode_put(key);
            return rc;
        }
    }
    arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
    struct unix_sock *u = reg_find_locked(key, a);
    spin_unlock_irqrestore(&g_reg_lock, st);
    vnode_put(key);
    if (u == NULL)
        return -ECONNREFUSED;
    *out = u;
    return 0;
}

int unix_getsockname(struct socket *s, struct unix_addr *out)
{
    *out = s->un->name;
    return 0;
}

int unix_getpeername(struct socket *s, struct unix_addr *out)
{
    struct unix_sock *u = s->un;
    if (s->type == COSMO_SOCK_STREAM) {
        if (u->conn == NULL)
            return -ENOTCONN;
        *out = u->peer_name;
        return 0;
    }
    arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
    struct unix_sock *p = u->dgram_peer;
    if (p == NULL) {
        spin_unlock_irqrestore(&g_reg_lock, st);
        return -ENOTCONN;
    }
    *out = p->name;
    spin_unlock_irqrestore(&g_reg_lock, st);
    return 0;
}

int unix_peercred(struct socket *s, struct cosmo_ucred *out)
{
    struct unix_sock *u = s->un;
    if (s->type != COSMO_SOCK_STREAM || u->conn == NULL || !u->has_cred)
        return -ENOTCONN;
    *out = u->cred;
    return 0;
}

/* --- streams ------------------------------------------------------------ */

int unix_listen(struct socket *s, int backlog)
{
    struct unix_sock *u = s->un;
    if (s->type != COSMO_SOCK_STREAM)
        return -EOPNOTSUPP;
    mutex_lock(&s->lock);
    int rc = 0;
    if (u->conn != NULL || u->name.len == 0) {
        rc = -EINVAL;
    } else {
        unsigned b = backlog < 1 ? 1u : (unsigned)backlog;
        if (b > UNIX_BACKLOG_MAX)
            b = UNIX_BACKLOG_MAX;
        u->backlog = b;
        if (!u->listening) {
            u->listening = true;
            cred_now(&u->cred);   /* what a connecting peer is told about the listener */
            u->has_cred = true;
        }
        s->state = SS_LISTENING;
    }
    mutex_unlock(&s->lock);
    room_changed();
    return rc;
}

static struct unix_conn *conn_new(struct socket *a, struct socket *b)
{
    struct unix_conn *c = kzalloc(sizeof(*c));
    if (c == NULL)
        return NULL;
    spinlock_init(&c->lock, "unix-conn");
    c->ends = 2;
    uq_init(&c->q[0]);
    uq_init(&c->q[1]);
    c->wq[0] = &a->wait;
    c->wq[1] = &b->wait;
    return c;
}

/* Link two stream sockets through a fresh connection; both become
 * connected, each told the other's name and credentials. */
static int link_pair(struct socket *a, struct socket *b, const struct cosmo_ucred *a_cred,
                     const struct cosmo_ucred *b_cred)
{
    struct unix_conn *c = conn_new(a, b);
    if (c == NULL)
        return -ENOMEM;
    a->un->conn = c;
    a->un->side = 0;
    b->un->conn = c;
    b->un->side = 1;
    a->un->cred = *b_cred;      /* what a is told about its peer, b */
    a->un->has_cred = true;
    b->un->cred = *a_cred;
    b->un->has_cred = true;
    a->un->peer_name = b->un->name;
    b->un->peer_name = a->un->name;
    a->state = SS_CONNECTED;
    b->state = SS_CONNECTED;
    return 0;
}

int unix_connect(struct socket *s, const struct unix_addr *a)
{
    struct unix_sock *u = s->un;
    struct unix_sock *target;
    int rc;
    if (s->type == COSMO_SOCK_DGRAM) {
        rc = unix_resolve(a, &target);
        if (rc)
            return rc;
        struct socket *t = target->sock;
        if (t->type != COSMO_SOCK_DGRAM) {
            ksock_put(t);
            return -EPROTOTYPE;
        }
        /* A default destination: a pointer under the registry lock, so the
         * pair a socketpair makes does not hold itself alive. */
        mutex_lock(&s->lock);
        arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
        u->dgram_peer = target;
        if (list_empty(&u->peer_link))
            list_push_back(&g_peered, &u->peer_link);
        spin_unlock_irqrestore(&g_reg_lock, st);
        s->state = SS_CONNECTED;
        mutex_unlock(&s->lock);
        ksock_put(t);
        return 0;
    }
    mutex_lock(&s->lock);
    if (u->conn != NULL) {
        mutex_unlock(&s->lock);
        return -EISCONN;
    }
    if (u->listening) {
        mutex_unlock(&s->lock);
        return -EINVAL;
    }
    /* Room in the listener's backlog: a blocking connect waits for it
     * holding no reference (see g_room_wq) and resolves the name again;
     * a non-blocking one does not wait. The listener's mutex is taken
     * under this socket's (the two are never taken the other way round:
     * a listener connects to nobody). */
    struct socket *t;
    for (;;) {
        unsigned gen = __atomic_load_n(&g_room_gen, __ATOMIC_ACQUIRE);
        rc = unix_resolve(a, &target);
        if (rc) {
            mutex_unlock(&s->lock);
            return rc;
        }
        t = target->sock;
        if (t->type != COSMO_SOCK_STREAM) {
            ksock_put(t);
            mutex_unlock(&s->lock);
            return -EPROTOTYPE;
        }
        mutex_lock_nested(&t->lock, 1);
        if (!target->listening) {
            mutex_unlock(&t->lock);
            ksock_put(t);
            mutex_unlock(&s->lock);
            return -ECONNREFUSED;
        }
        if (target->queued < target->backlog)
            break;   /* t locked and referenced */
        mutex_unlock(&t->lock);
        ksock_put(t);
        if (io_nonblocking(s->nonblock)) {
            mutex_unlock(&s->lock);
            return -EAGAIN;
        }
        int w = wait_event_killable(&g_room_wq, __atomic_load_n(&g_room_gen, __ATOMIC_ACQUIRE) != gen);
        if (w) {
            mutex_unlock(&s->lock);
            return w;
        }
    }
    /* The server side exists now, before anyone accepts: the client may
     * write and the bytes wait in the queue. */
    struct socket *c;
    rc = ksock_create(COSMO_AF_UNIX, COSMO_SOCK_STREAM, t->uid, &c);
    if (rc == 0) {
        struct cosmo_ucred mine;
        cred_now(&mine);
        c->un->name = target->name;   /* the accepted socket answers to the listener's name */
        rc = link_pair(s, c, &mine, &target->cred);
        if (rc) {
            ksock_put(c);
        } else {
            list_push_back(&target->acceptq, &c->un->accept_link);   /* the creation reference is the queue's */
            target->queued++;
        }
    }
    mutex_unlock(&t->lock);
    if (rc == 0)
        wake_sock(t);
    ksock_put(t);
    mutex_unlock(&s->lock);
    return rc;
}

int unix_accept(struct socket *s, struct socket **out)
{
    struct unix_sock *u = s->un;
    if (s->type != COSMO_SOCK_STREAM)
        return -EOPNOTSUPP;
    for (;;) {
        mutex_lock(&s->lock);
        if (!u->listening) {
            mutex_unlock(&s->lock);
            return -EINVAL;
        }
        if (!list_empty(&u->acceptq)) {
            struct unix_sock *c = container_of(list_pop_front(&u->acceptq), struct unix_sock, accept_link);
            u->queued--;
            mutex_unlock(&s->lock);
            room_changed();   /* a connector waiting for room */
            *out = c->sock;   /* the queue's reference becomes the caller's */
            return 0;
        }
        mutex_unlock(&s->lock);
        if (s->shut & 1)
            return -EINVAL;
        if (io_nonblocking(s->nonblock))
            return -EAGAIN;
        int w = wait_event_killable(&s->wait, u->queued > 0 || (s->shut & 1));
        if (w)
            return w;
    }
}

int unix_socketpair(int type, struct socket **a, struct socket **b)
{
    if (type != COSMO_SOCK_STREAM && type != COSMO_SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;
    uint32_t uid = cred_current()->euid;
    struct socket *x, *y;
    int rc = ksock_create(COSMO_AF_UNIX, type, uid, &x);
    if (rc)
        return rc;
    rc = ksock_create(COSMO_AF_UNIX, type, uid, &y);
    if (rc) {
        ksock_put(x);
        return rc;
    }
    if (type == COSMO_SOCK_STREAM) {
        struct cosmo_ucred me;
        cred_now(&me);
        rc = link_pair(x, y, &me, &me);
        if (rc) {
            ksock_put(x);
            ksock_put(y);
            return rc;
        }
    } else {
        arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
        x->un->dgram_peer = y->un;
        y->un->dgram_peer = x->un;
        list_push_back(&g_peered, &x->un->peer_link);
        list_push_back(&g_peered, &y->un->peer_link);
        spin_unlock_irqrestore(&g_reg_lock, st);
        x->state = SS_CONNECTED;
        y->state = SS_CONNECTED;
    }
    *a = x;
    *b = y;
    return 0;
}

/* --- send and receive --------------------------------------------------- */

static struct umsg *umsg_new(const void *buf, size_t n, struct unix_handles *h)
{
    struct umsg *m = kmalloc(sizeof(*m) + (n ? n : 1), 0);
    if (m == NULL)
        return NULL;
    list_init(&m->link);
    m->len = n;
    m->off = 0;
    memset(&m->from, 0, sizeof(m->from));
    m->h.nr = 0;
    if (h != NULL && h->nr > 0) {
        m->h = *h;
        h->nr = 0;   /* the references are the message's now */
    }
    memcpy(m->data, buf, n);
    return m;
}

/* A unix socket does not ride in a message: two sockets each queued in
 * the other would hold each other alive with nothing outside to release
 * either, the cycle Linux collects with a garbage collector. Refused,
 * and the report says so (docs/audit/next-subsystem-unix-sockets.md). */
static bool handles_allowed(const struct unix_handles *h)
{
    if (h == NULL)
        return true;
    for (unsigned i = 0; i < h->nr; i++) {
        struct socket *s = socket_from_kobject(h->objs[i]);
        if (s != NULL && s->family == COSMO_AF_UNIX)
            return false;
    }
    return true;
}

static int64_t stream_send(struct socket *s, const void *buf, size_t len, struct unix_handles *h, bool dontwait)
{
    struct unix_sock *u = s->un;
    struct unix_conn *c = u->conn;
    if (c == NULL)
        return -ENOTCONN;
    if (s->shut & 2)
        return -EPIPE;
    struct uqueue *q = &c->q[1 - u->side];
    bool nonblock = dontwait || io_nonblocking(s->nonblock);
    size_t want = len < UNIX_MSG_MAX ? len : UNIX_MSG_MAX;
    struct umsg *m = umsg_new(buf, want, NULL);   /* the handles are attached under the lock, once it fits */
    if (m == NULL)
        return -ENOMEM;
    for (;;) {
        arch_irq_state_t st = spin_lock_irqsave(&c->lock);
        if (q->rd_closed) {
            spin_unlock_irqrestore(&c->lock, st);
            kfree(m);
            return -EPIPE;
        }
        size_t space = UNIX_BUF - q->bytes;
        if (space > 0 || len == 0) {
            if (m->len > space)
                m->len = space;   /* a partial send; the caller's loop brings the rest */
            if (h != NULL && h->nr > 0) {
                m->h = *h;
                h->nr = 0;
            }
            list_push_back(&q->msgs, &m->link);
            q->bytes += m->len;
            q->count++;
            struct waitqueue *peer = c->wq[1 - u->side];
            if (peer)
                waitqueue_wake_all(peer);
            size_t sent = m->len;
            spin_unlock_irqrestore(&c->lock, st);
            return (int64_t)sent;
        }
        spin_unlock_irqrestore(&c->lock, st);
        if (nonblock) {
            kfree(m);
            return -EAGAIN;
        }
        int w = wait_event_killable(&s->wait, q->rd_closed || q->bytes < UNIX_BUF);
        if (w) {
            kfree(m);
            return w;
        }
    }
}

static int64_t dgram_send(struct socket *s, const void *buf, size_t len, const struct unix_addr *to,
                          struct unix_handles *h, bool dontwait)
{
    struct unix_sock *u = s->un;
    if (len > UNIX_MSG_MAX)
        return -EMSGSIZE;
    struct umsg *m = umsg_new(buf, len, NULL);
    if (m == NULL)
        return -ENOMEM;
    m->from = u->name;
    bool nonblock = dontwait || io_nonblocking(s->nonblock);
    int64_t rc;
    for (;;) {
        /* The destination, referenced for this attempt only: a sender
         * waiting for room holds nothing (see g_room_wq). */
        unsigned gen = __atomic_load_n(&g_room_gen, __ATOMIC_ACQUIRE);
        struct unix_sock *target;
        if (to != NULL) {
            int r = unix_resolve(to, &target);
            if (r) {
                rc = r;
                break;
            }
            if (target->sock->type != COSMO_SOCK_DGRAM) {
                ksock_put(target->sock);
                rc = -EPROTOTYPE;
                break;
            }
        } else {
            arch_irq_state_t st = spin_lock_irqsave(&g_reg_lock);
            target = u->dgram_peer;
            if (target != NULL && !kobject_tryget(&target->sock->obj))
                target = NULL;
            spin_unlock_irqrestore(&g_reg_lock, st);
            if (target == NULL) {
                rc = s->state == SS_CONNECTED ? -ECONNREFUSED : -ENOTCONN;
                break;
            }
        }
        struct socket *t = target->sock;
        arch_irq_state_t st = spin_lock_irqsave(&target->qlock);
        bool refused = target->rxq.rd_closed;
        bool room = target->rxq.count < UNIX_DGRAM_MAX && target->rxq.bytes + len <= UNIX_DGRAM_BYTES;
        if (!refused && room) {
            if (h != NULL && h->nr > 0) {
                m->h = *h;
                h->nr = 0;
            }
            list_push_back(&target->rxq.msgs, &m->link);
            target->rxq.bytes += len;
            target->rxq.count++;
        }
        spin_unlock_irqrestore(&target->qlock, st);
        if (!refused && room) {
            wake_sock(t);
            ksock_put(t);
            m = NULL;
            rc = (int64_t)len;
            break;
        }
        ksock_put(t);
        if (refused) {
            rc = -ECONNREFUSED;
            break;
        }
        if (nonblock) {
            rc = -EAGAIN;
            break;
        }
        int w = wait_event_killable(&g_room_wq, __atomic_load_n(&g_room_gen, __ATOMIC_ACQUIRE) != gen);
        if (w) {
            rc = w;
            break;
        }
    }
    if (m)
        kfree(m);
    return rc;
}

int64_t unix_send(struct socket *s, const void *buf, size_t len, const struct unix_addr *to, struct unix_handles *h,
                  bool dontwait)
{
    if (h != NULL && h->nr > UNIX_HANDLES_MAX)
        return -EINVAL;
    if (!handles_allowed(h))
        return -EINVAL;
    if (s->type == COSMO_SOCK_STREAM) {
        if (to != NULL)
            return -EISCONN;
        return stream_send(s, buf, len, h, dontwait);
    }
    return dgram_send(s, buf, len, to, h, dontwait);
}

/* Hand a message's handles to the receiver's set, in message order and
 * as many as there is room for (the references move); the rest go to
 * `dropped` with the truncation flag. "What fits" is literal. */
static void take_handles(struct umsg *m, struct unix_handles *h, unsigned room, unsigned *flags,
                         struct unix_handles *dropped)
{
    if (m->h.nr == 0)
        return;
    unsigned take = h != NULL ? (room < m->h.nr ? room : m->h.nr) : 0;
    if (h != NULL) {
        h->nr = take;
        for (unsigned i = 0; i < take; i++) {
            h->objs[i] = m->h.objs[i];
            h->rights[i] = m->h.rights[i];
        }
    }
    if (take < m->h.nr) {
        dropped->nr = m->h.nr - take;
        for (unsigned i = take; i < m->h.nr; i++) {
            dropped->objs[i - take] = m->h.objs[i];
            dropped->rights[i - take] = m->h.rights[i];
        }
        *flags |= COSMO_MSG_HTRUNC;
    }
    m->h.nr = 0;
}

static int64_t stream_recv(struct socket *s, void *buf, size_t len, struct unix_handles *h, unsigned *flags,
                           bool dontwait)
{
    struct unix_sock *u = s->un;
    struct unix_conn *c = u->conn;
    if (c == NULL)
        return -ENOTCONN;
    struct uqueue *q = &c->q[u->side];
    bool nonblock = dontwait || io_nonblocking(s->nonblock);
    struct unix_handles dropped = { .nr = 0 };
    unsigned room = h ? h->nr : 0;
    if (h)
        h->nr = 0;
    for (;;) {
        arch_irq_state_t st = spin_lock_irqsave(&c->lock);
        if (!list_empty(&q->msgs) && !(s->shut & 1)) {
            size_t done = 0;
            bool took = false;
            while (done < len && !list_empty(&q->msgs)) {
                struct umsg *m = container_of(q->msgs.next, struct umsg, link);
                if (m->h.nr > 0 && m->off == 0) {
                    /* Ancillary items ride with the first byte of their
                     * send: delivered with it, never with an earlier
                     * one, and never two sends' worth in one read. A
                     * read that has already copied bytes stops here. */
                    if (done > 0 || took)
                        break;
                    take_handles(m, h, room, flags, &dropped);
                    took = true;
                }
                size_t n = m->len - m->off;
                if (n > len - done)
                    n = len - done;
                memcpy((uint8_t *)buf + done, m->data + m->off, n);
                m->off += n;
                done += n;
                q->bytes -= n;
                if (m->off == m->len) {
                    list_remove(&m->link);
                    q->count--;
                    kfree(m);   /* its handles were moved or dropped above */
                }
            }
            struct waitqueue *peer = c->wq[1 - u->side];
            if (peer)
                waitqueue_wake_all(peer);   /* space for the writer */
            spin_unlock_irqrestore(&c->lock, st);
            unix_handles_drop(&dropped);
            return (int64_t)done;
        }
        bool eof = q->wr_closed || (s->shut & 1);
        spin_unlock_irqrestore(&c->lock, st);
        if (eof || len == 0)
            return 0;
        if (nonblock)
            return -EAGAIN;
        int w = wait_event_killable(&s->wait, !list_empty(&q->msgs) || q->wr_closed || (s->shut & 1));
        if (w)
            return w;
    }
}

static int64_t dgram_recv(struct socket *s, void *buf, size_t len, struct unix_addr *from, struct unix_handles *h,
                          unsigned *flags, bool dontwait)
{
    struct unix_sock *u = s->un;
    bool nonblock = dontwait || io_nonblocking(s->nonblock);
    unsigned room = h ? h->nr : 0;
    if (h)
        h->nr = 0;
    for (;;) {
        arch_irq_state_t st = spin_lock_irqsave(&u->qlock);
        if (!list_empty(&u->rxq.msgs) && !(s->shut & 1)) {
            struct umsg *m = container_of(list_pop_front(&u->rxq.msgs), struct umsg, link);
            u->rxq.bytes -= m->len;
            u->rxq.count--;
            spin_unlock_irqrestore(&u->qlock, st);
            room_changed();   /* a sender waiting for room in this queue */
            size_t n = m->len < len ? m->len : len;
            memcpy(buf, m->data, n);
            if (m->len > len)
                *flags |= COSMO_MSG_TRUNC;
            if (from)
                *from = m->from;
            struct unix_handles dropped = { .nr = 0 };
            take_handles(m, h, room, flags, &dropped);
            kfree(m);
            unix_handles_drop(&dropped);
            return (int64_t)n;
        }
        spin_unlock_irqrestore(&u->qlock, st);
        if (s->shut & 1)
            return 0;
        if (nonblock)
            return -EAGAIN;
        int w = wait_event_killable(&s->wait, !list_empty(&u->rxq.msgs) || (s->shut & 1));
        if (w)
            return w;
    }
}

int64_t unix_recv(struct socket *s, void *buf, size_t len, struct unix_addr *from, struct unix_handles *h,
                  unsigned *flags, bool dontwait)
{
    unsigned f = 0;
    int64_t rc;
    if (s->type == COSMO_SOCK_STREAM) {
        if (from)
            memset(from, 0, sizeof(*from));
        rc = stream_recv(s, buf, len, h, &f, dontwait);
        if (from && rc >= 0)
            *from = s->un->peer_name;
    } else {
        rc = dgram_recv(s, buf, len, from, h, &f, dontwait);
    }
    if (flags)
        *flags = f;
    return rc;
}

int unix_shutdown(struct socket *s, int how)
{
    struct unix_sock *u = s->un;
    if (s->type == COSMO_SOCK_STREAM && u->conn != NULL) {
        struct unix_conn *c = u->conn;
        struct list_node gone = LIST_HEAD_INIT(gone);
        arch_irq_state_t st = spin_lock_irqsave(&c->lock);
        if (how == COSMO_SHUT_RD || how == COSMO_SHUT_RDWR) {
            c->q[u->side].rd_closed = true;         /* the peer's writes fail */
            uq_drain_locked(&c->q[u->side], &gone);
        }
        if (how == COSMO_SHUT_WR || how == COSMO_SHUT_RDWR)
            c->q[1 - u->side].wr_closed = true;     /* the peer reads end-of-stream */
        struct waitqueue *peer = c->wq[1 - u->side];
        if (peer)
            waitqueue_wake_all(peer);
        spin_unlock_irqrestore(&c->lock, st);
        free_gone(&gone);
    } else if (s->type == COSMO_SOCK_DGRAM && (how == COSMO_SHUT_RD || how == COSMO_SHUT_RDWR)) {
        struct list_node gone = LIST_HEAD_INIT(gone);
        arch_irq_state_t st = spin_lock_irqsave(&u->qlock);
        uq_drain_locked(&u->rxq, &gone);
        spin_unlock_irqrestore(&u->qlock, st);
        free_gone(&gone);
    }
    wake_sock(s);
    return 0;
}

unsigned unix_ready(struct socket *s)
{
    struct unix_sock *u = s->un;
    unsigned r = 0;
    if (s->type == COSMO_SOCK_STREAM) {
        if (u->listening) {
            if (u->queued > 0)
                r |= COSMO_IO_READABLE;
        } else if (u->conn == NULL) {
            r |= COSMO_IO_WRITABLE;   /* a write fails at once with -ENOTCONN */
        } else {
            struct unix_conn *c = u->conn;
            arch_irq_state_t st = spin_lock_irqsave(&c->lock);
            struct uqueue *rq = &c->q[u->side], *wq = &c->q[1 - u->side];
            if (rq->bytes > 0 || rq->wr_closed)
                r |= COSMO_IO_READABLE;
            if (wq->bytes < UNIX_BUF || wq->rd_closed)
                r |= COSMO_IO_WRITABLE;
            if (rq->wr_closed)
                r |= COSMO_IO_HANGUP;
            spin_unlock_irqrestore(&c->lock, st);
        }
    } else {
        arch_irq_state_t st = spin_lock_irqsave(&u->qlock);
        if (u->rxq.count > 0)
            r |= COSMO_IO_READABLE;
        spin_unlock_irqrestore(&u->qlock, st);
        r |= COSMO_IO_WRITABLE;
    }
    if (s->shut & 1)
        r |= COSMO_IO_READABLE | COSMO_IO_HANGUP;
    if (s->shut & 2)
        r |= COSMO_IO_WRITABLE;
    return r;
}
