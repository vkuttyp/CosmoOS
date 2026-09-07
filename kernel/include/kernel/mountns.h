/*
 * mountns.h - Mount namespaces: what set of mounts a process can see
 * (docs/kernel/security/design.md §1d, docs/kernel-services/vfs/design.md).
 *
 * A namespace is a set of mounts, not a set of filesystems. Starting one
 * copies the parent's view -- every mount it can see gains a reference
 * saying the new namespace can see it too -- and never copies a
 * filesystem instance: one mount is one vnode cache, one open
 * transaction and one device, and a second instance of those over the
 * same disk is not isolation but corruption.
 */

#ifndef KERNEL_MOUNTNS_H
#define KERNEL_MOUNTNS_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/types.h>

struct mount;

/*
 * One (mount, namespace) pair: the mount can be seen in the namespace.
 * On two lists at once -- the mount's `ns_refs` and the namespace's
 * `mounts` -- because both directions are walked: a walker asks a mount
 * whether it is visible, and a dying namespace asks itself what it can
 * see.
 */
struct mount_ns_ref {
    struct mount *mnt;
    struct mount_ns *ns;
    struct list_node mnt_link;   /* on mount->ns_refs, under the mountpoint's lock */
    struct list_node ns_link;    /* on mount_ns->mounts, under g_mounts_lock */
};

struct mount_ns {
    uint64_t id;                 /* 0 is the namespace the system boots in */
    /* Processes holding it. Atomic rather than under g_mounts_lock,
     * because a child inherits its parent's namespace while the
     * parent's spinlock is held and a mutex may not be taken there. */
    uint32_t refs;
    struct list_node mounts;     /* struct mount_ns_ref, oldest first */
};

/* The namespace the system boots in. Never freed. */
struct mount_ns *mountns_initial(void);

/* The caller's namespace: its process's, or the initial one for a
 * kernel thread and for anything running before there are processes.
 * Borrowed -- valid because the caller is running in it. */
struct mount_ns *mountns_current(void);

/*
 * A new namespace holding exactly what `parent` can see. Either it is
 * built completely or nothing is left behind: a failure part-way undoes
 * the refs it added, since a half-copied view is a view of a filesystem
 * set nobody chose.
 */
int mountns_create(struct mount_ns *parent, struct mount_ns **out);

struct mount_ns *mountns_get(struct mount_ns *ns);

/*
 * Drop a reference. The last one takes the namespace's mounts with it,
 * newest first so a nested mount goes before the one holding its
 * mountpoint, and unmounts each that no other namespace can see. May
 * block: it commits filesystems.
 */
void mountns_put(struct mount_ns *ns);

/* Whether `ns` can see `mnt`. The caller holds mnt->mountpoint->lock;
 * the root mount is visible everywhere and answers true without it. */
bool mountns_sees(const struct mount_ns *ns, const struct mount *mnt);

#endif /* KERNEL_MOUNTNS_H */
