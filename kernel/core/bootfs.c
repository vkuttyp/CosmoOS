/*
 * bootfs.c - Boot composition of the filesystem namespace (bootfs.h).
 *
 * The one place that names concrete filesystems: the VFS registers what
 * it is given and mounts the root it is told to (roadmap M2, which moved
 * both out of vfs_init).
 */

#include <kernel/blk.h>
#include <kernel/bootfs.h>
#include <kernel/cmdline.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/gpt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/part.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

extern struct fs_type ramfs_fs_type;
extern struct fs_type procfs_fs_type;

/* root= as written; read once, when the disks are ready. */
static char g_root_spec[128];

void bootfs_init(void)
{
    if (vfs_register_fs(&ramfs_fs_type))
        panic("bootfs: cannot register ramfs");
    if (vfs_register_fs(&procfs_fs_type))
        panic("bootfs: cannot register procfs");
    cosmofs_init();
    /* The live root, always: a ramfs holding the boot archive. A disk
     * root replaces it later, from init, never from here. */
    int rc = vfs_mount_root("ramfs", NULL, 0);
    if (rc)
        panic("bootfs: cannot mount the root ramfs (%d)", rc);
    /* Anonymous files (memfd, shm) on a ramfs of their own, attached
     * nowhere: they must outlive the live root when init switches to a
     * disk root. */
    struct mount *anon;
    rc = vfs_mount_internal("ramfs", &anon);
    if (rc)
        panic("bootfs: cannot make the anonymous-file ramfs (%d)", rc);
    ramfs_set_anon_mount(anon);
    ramfs_populate_boot();
    /* /dev is a ramfs of its own, so the device nodes the kernel makes
     * there move with it onto a disk root (vfs_switch_root). */
    if (vfs_mount("/dev", "ramfs", NULL, 0) != 0)
        panic("bootfs: cannot mount /dev");
    /* /proc: facts about processes, addressable as files
     * (docs/kernel-services/filesystem/procfs/design.md). */
    if (vfs_mount("/proc", "procfs", NULL, 0) != 0)
        kwarn("vfs: cannot mount /proc");
}

const char *bootfs_root_spec(void)
{
    return g_root_spec;
}

int bootfs_root_device(char *out, size_t len)
{
    if (g_root_spec[0] == '\0')
        return -ENOENT;
    struct blkdev *bd = NULL;
    if (strncmp(g_root_spec, "PARTUUID=", 9) == 0) {
        uint8_t uuid[16];
        if (strlen(g_root_spec + 9) != 36 || !gpt_guid_parse(g_root_spec + 9, uuid))
            return -EINVAL;
        bd = blk_find_partuuid(uuid);
    } else {
        if (strchr(g_root_spec, '=') != NULL || strchr(g_root_spec, '/') != NULL ||
            strlen(g_root_spec) >= BLKDEV_NAME_MAX)
            return -EINVAL;
        bd = blk_find(g_root_spec);   /* root=<device>, for development */
    }
    if (bd == NULL)
        return -ENODEV;
    strlcpy(out, bd->name, len);
    blkdev_put(bd);
    return 0;
}

void bootfs_disks_ready(void)
{
    blk_part_scan_all();
    cmdline_log();
    int rc = cmdline_get("root", g_root_spec, sizeof(g_root_spec));
    if (rc == -2) {
        kerror("root: the root= value is too long; staying on the live root");
        g_root_spec[0] = '\0';
        return;
    }
    if (rc < 0) {
        kinfo("root: no root= on the command line; the live root stays");
        return;
    }
    char dev[BLKDEV_NAME_MAX];
    rc = bootfs_root_device(dev, sizeof(dev));
    if (rc == 0)
        kinfo("root: root=%s is %s; init mounts it and switches to it", g_root_spec, dev);
    else if (rc == -EINVAL)
        kerror("root: root=%s is neither PARTUUID=<guid> nor a device name; init stays on the live root",
               g_root_spec);
    else
        kerror("root: root=%s: no such device; init stays on the live root", g_root_spec);
}
