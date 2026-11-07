/*
 * cosmofstest.c - Self-tests for the storage pool and cosmofs on the
 * scratch virtio disk (vda). They format the disk, so they run only
 * when it exists, and they leave a filesystem behind for init's
 * user-mode test: /hello.txt and /dir/nested.txt.
 */

#include <kernel/compiler.h>
#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <uapi/cosmo/fsctl.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/thread.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/printf.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "cosmofstest.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* --- the transaction engine (audit milestone 7), on RAM devices --------------- */

#include <kernel/ramblk.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

#define ENG "/mnt/eng"

#if CONFIG_DEBUG
/*
 * A pool of two members. Everything the format change is for is here:
 * blocks are named by member, the allocator spreads across both, each
 * member carries its own bitmap, and a remount finds the second member
 * by its label rather than being told about it.
 */
/*
 * A version-3 disk mounts, writes and remounts under this kernel. That
 * is the whole benefit of packing the DVA into the eight bytes a pointer
 * already had: an old pointer is a new one on member 0, so there is no
 * conversion and no second decoder (design.md, "The DVA is 64 bits").
 */
/*
 * Symbolic links on disk (docs/audit/next-subsystem-symlink.md). The
 * target is the link's own block, written inside the transaction that
 * writes the inode and adds the entry, so a link either exists whole or
 * not at all -- and a filesystem formatted before version 8 refuses to
 * make one, because a kernel of that vintage would read it as a regular
 * file whose contents are a path.
 */

}

}
#else
bool selftest_cosmofs_snapshot_remount(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snap_identity(const char **reason) { (void)reason; return true; }
#endif
