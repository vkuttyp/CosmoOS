/*
 * mountns.c - Mount namespaces
 * (docs/kernel/security/design.md §1d, invariant V28).
 *
 * A namespace is a set of mounts a process can see. Starting one copies
 * the parent's *view* -- a ref per (mount, namespace) pair -- and never
 * a filesystem: one mount is one vnode cache, one open transaction and
 * one device, and a second instance of those over the same disk is not
 * isolation but corruption.
 *
 * Two lists hold the same refs from both ends, because both directions
 * are walked: a walker asks a mount who can see it, and a dying
 * namespace asks itself what it can see.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/vfs.h>

#include "vfs_internal.h"

/* The namespace the system boots in. Static: it predates kmalloc being
 * worth using and outlives every process, so it is never freed and its
 * reference count is only ever a formality. */
static struct mount_ns g_init_ns = {
    .id = 0,
    .refs = 1,
    .mounts = LIST_HEAD_INIT(g_init_ns.mounts),
};

/* Identifiers for the log, from 1: 0 is the initial namespace and
 * belongs to nobody else. Under g_mounts_lock. */
static uint64_t g_next_ns_id = 1;

struct mount_ns *mountns_initial(void)
{
    return &g_init_ns;
}

struct mount_ns *mountns_current(void)
{
    struct process *p = process_current();
    if (p == NULL || p->mntns == NULL)
        return &g_init_ns;   /* kernel threads, and everything before init */
    return p->mntns;
}

/*
 * The root mount answers true without being on any list. Every
 * namespace has a root and none may unmount it, so a set that always
 * has the same one member is a fact better stated than stored -- and
 * stating it here keeps the root out of the copy loop, where a failure
 * part-way would otherwise be able to produce a namespace with no root.
 */
bool mountns_sees(const struct mount_ns *ns, const struct mount *mnt)
{
    if (mnt == g_root_mount)
        return true;
    const struct mount_ns_ref *r;
    list_for_each_entry(r, &mnt->ns_refs, mnt_link)
        if (r->ns == ns)
            return true;
    return false;
}

/* Add `ns` to what can see `mnt`. Takes the mountpoint's lock, the same
 * lock that guards where the mount is attached. */
static int ns_add(struct mount_ns *ns, struct mount *mnt)
{
    struct mount_ns_ref *ref = kmalloc(sizeof(*ref), KMEM_ZERO);
    if (ref == NULL)
        return -ENOMEM;
    ref->mnt = mnt;
    ref->ns = ns;
    mutex_lock(&mnt->mountpoint->lock);
    list_push_back(&mnt->ns_refs, &ref->mnt_link);
    mutex_unlock(&mnt->mountpoint->lock);
    list_push_back(&ns->mounts, &ref->ns_link);
    return 0;
}

/*
 * Take a ref -- already off its namespace's list -- off the mount's
 * too, and say whether that leaves a mount no namespace can see. Both
 * callers pop the node before calling, which is also the only shape the
 * static analyzer can follow across the free.
 */
static bool ns_drop(struct mount_ns_ref *ref)
{
    struct mount *mnt = ref->mnt;
    mutex_lock(&mnt->mountpoint->lock);
    list_remove(&ref->mnt_link);
    bool orphaned = list_empty(&mnt->ns_refs);
    mutex_unlock(&mnt->mountpoint->lock);
    kfree(ref);
    return orphaned;
}

int mountns_create(struct mount_ns *parent, struct mount_ns **out)
{
    struct mount_ns *ns = kmalloc(sizeof(*ns), KMEM_ZERO);
    if (ns == NULL)
        return -ENOMEM;
    ns->refs = 1;
    list_init(&ns->mounts);

    mutex_lock(&g_mounts_lock);
    ns->id = g_next_ns_id++;
    int rc = 0;
    struct mount *mnt;
    list_for_each_entry(mnt, &g_mounts, link) {
        if (mnt == g_root_mount || !mountns_sees(parent, mnt))
            continue;
        rc = ns_add(ns, mnt);
        if (rc)
            break;
    }
    if (rc) {
        /* A half-copied view is a view of a filesystem set nobody
         * chose, so it is undone rather than handed out. Nothing here
         * can be orphaned: every one of these mounts is still seen by
         * the namespace it was copied from. */
        struct mount_ns_ref *dead, *next;
        list_for_each_entry_safe(dead, next, &ns->mounts, ns_link)
            ns_drop(dead);
        list_init(&ns->mounts);
    }
    mutex_unlock(&g_mounts_lock);

    if (rc) {
        kfree(ns);
        return rc;
    }
    *out = ns;
    return 0;
}

/* Lock-free on purpose: a child takes its parent's namespace while the
 * parent's spinlock is held, where a mutex may not be taken. */
struct mount_ns *mountns_get(struct mount_ns *ns)
{
    if (ns == NULL)
        return NULL;
    uint32_t old = __atomic_fetch_add(&ns->refs, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    return ns;
}

void mountns_put(struct mount_ns *ns)
{
    if (ns == NULL || ns == &g_init_ns)
        return;
    uint32_t old = __atomic_fetch_sub(&ns->refs, 1u, __ATOMIC_ACQ_REL);
    KASSERT(old > 0);
    if (old > 1)
        return;

    /*
     * Nothing can name this namespace any more, so nothing can mount
     * into it while this runs -- which is what lets the whole list come
     * off in one go and be worked through with the table lock dropped.
     * That matters because unmounting commits, and a commit must not
     * run under the lock every other mount needs.
     */
    struct list_node todo;
    list_init(&todo);
    mutex_lock(&g_mounts_lock);
    list_move_all(&ns->mounts, &todo);
    mutex_unlock(&g_mounts_lock);
    kfree(ns);

    /* Newest first: a mount nested inside another holds a vnode of the
     * one below it, so the outer one is only releasable once the inner
     * one is gone. */
    struct mount_ns_ref *ref, *next;
    list_for_each_entry_safe_reverse(ref, next, &todo, ns_link) {
        struct mount *mnt = ref->mnt;
        mutex_lock(&g_mounts_lock);
        bool orphaned = ns_drop(ref);
        if (orphaned)
            /* Unreachable from here on: off the mount table and off the
             * directory it covered, before the lock is dropped. */
            vfs_mount_detach(mnt);
        mutex_unlock(&g_mounts_lock);
        if (orphaned)
            vfs_mount_orphaned(mnt);
    }
}
