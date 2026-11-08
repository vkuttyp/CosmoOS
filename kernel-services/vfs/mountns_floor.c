/*
 * mountns_floor.c - The C under mountns.cee (roadmap 2.0 SA7): the
 * namespace the system boots in, a static whose list head points into
 * itself; the VFS's mount table, its lock and the root mount
 * (vfs_internal.h), C globals handed over by address; and the values
 * mountns.cee writes as its own constants, asserted. Mount namespaces
 * themselves are CeeSafe (mountns.cee, generated into mountns.c).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mountns.h>
#include <kernel/vfs.h>

#include "vfs_internal.h"

_Static_assert(ENOMEM == 12 && KMEM_ZERO == 1 && VFS_PATH_MAX == 1024 &&
                   sizeof(((struct mount_ns_ref *)0)->path) == VFS_PATH_MAX &&
                   sizeof(((struct mount_ns *)0)->refs) == 4,
               "mountns.cee's errno.h, kmalloc.h and vfs.h values");

/* What mountns.cee calls here. */
struct mount_ns *mountns_floor_init_ns(void);
struct list_node *mountns_floor_mounts(void);
struct mutex *mountns_floor_mounts_lock(void);
struct mount *mountns_floor_root(void);

/* The namespace the system boots in. Static: it predates kmalloc being
 * worth using and outlives every process, so it is never freed and its
 * reference count is only ever a formality. */
static struct mount_ns g_init_ns = {
    .id = 0,
    .refs = 1,
    .mounts = LIST_HEAD_INIT(g_init_ns.mounts),
};

struct mount_ns *mountns_floor_init_ns(void)
{
    return &g_init_ns;
}

struct list_node *mountns_floor_mounts(void)
{
    return &g_mounts;
}

struct mutex *mountns_floor_mounts_lock(void)
{
    return &g_mounts_lock;
}

struct mount *mountns_floor_root(void)
{
    return g_root_mount;
}
