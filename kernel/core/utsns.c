/*
 * utsns.c - The uts namespace
 * (docs/kernel/security/design.md §1e, invariant S13).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/utsns.h>

/* The namespace the system boots in. Static, never freed: it outlives
 * every process, and its reference count is only ever a formality. */
static struct uts_ns g_init_uts = {
    .refs = 1,
    .name = "cosmo",
};

static bool g_init_uts_ready;

static void init_once(void)
{
    if (!g_init_uts_ready) {
        spinlock_init(&g_init_uts.lock, "utsns");
        g_init_uts_ready = true;
    }
}

struct uts_ns *utsns_initial(void)
{
    init_once();
    return &g_init_uts;
}

struct uts_ns *utsns_current(void)
{
    struct process *p = process_current();
    if (p == NULL || p->utsns == NULL)
        return utsns_initial();
    return p->utsns;
}

int utsns_create(struct uts_ns *parent, struct uts_ns **out)
{
    struct uts_ns *ns = kmalloc(sizeof(*ns), KMEM_ZERO);
    if (ns == NULL)
        return -ENOMEM;
    ns->refs = 1;
    spinlock_init(&ns->lock, "utsns");
    /* A copy of the name as it is now: what the parent is called at the
     * moment of the split, not whatever it is called later. */
    utsns_gethostname(parent, ns->name, sizeof(ns->name));
    *out = ns;
    return 0;
}

/* Lock-free on purpose: a child takes its parent's namespace while the
 * parent's spinlock is held, where a mutex may not be taken. */
struct uts_ns *utsns_get(struct uts_ns *ns)
{
    if (ns == NULL)
        return NULL;
    uint32_t old = __atomic_fetch_add(&ns->refs, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    return ns;
}

void utsns_put(struct uts_ns *ns)
{
    if (ns == NULL || ns == &g_init_uts)
        return;
    uint32_t old = __atomic_fetch_sub(&ns->refs, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    if (old == 1)
        kfree(ns);
}

size_t utsns_gethostname(struct uts_ns *ns, char *out, size_t n)
{
    if (n == 0)
        return 0;
    arch_irq_state_t s = spin_lock_irqsave(&ns->lock);
    size_t len = strlcpy(out, ns->name, n);
    spin_unlock_irqrestore(&ns->lock, s);
    return len < n ? len : n - 1;
}

/*
 * A name reaches log lines, `uname` and whatever a peer is told. One
 * that can hold a newline can forge a log line, and one that can hold a
 * NUL is not the string the setter thinks it set, so both are refused
 * rather than trimmed: a name that arrives different from how it was
 * sent is worse than a name that was refused.
 */
int utsns_sethostname(struct uts_ns *ns, const char *name, size_t len)
{
    if (len == 0 || len >= COSMO_HOST_NAME_MAX)
        return -EINVAL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f)
            return -EINVAL;
    }
    arch_irq_state_t s = spin_lock_irqsave(&ns->lock);
    memcpy(ns->name, name, len);
    ns->name[len] = '\0';
    spin_unlock_irqrestore(&ns->lock, s);
    return 0;
}
