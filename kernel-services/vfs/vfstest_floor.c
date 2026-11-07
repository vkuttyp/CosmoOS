/*
 * vfstest_floor.c - The C under the VFS's CeeSafe self-tests (roadmap
 * 2.0 SA7): the values they write as their own constants, asserted, and
 * the stat record's size, which they keep on their stacks in u64s. The
 * self-tests themselves are CeeSafe (vfstest_*.cee, generated into
 * vfstest_*.c).
 */

#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/pagecache.h>
#include <kernel/thread.h>
#include <kernel/types.h>
#include <kernel/utsns.h>
#include <kernel/vfs.h>

_Static_assert(ENOENT == 2 && EBADF == 9 && EBUSY == 16 && EEXIST == 17 && EXDEV == 18 && ENODEV == 19 &&
                   ENOTDIR == 20 && EISDIR == 21 && EINVAL == 22 && ENAMETOOLONG == 36 && ENOTEMPTY == 39 &&
                   ELOOP == 40,
               "vfstest_base.cee's errno.h values");
_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_RDWR == 2 && COSMO_O_ACCMODE == 3 &&
                   COSMO_O_CREAT == 0x40 && COSMO_O_EXCL == 0x80 && COSMO_O_TRUNC == 0x200 &&
                   COSMO_O_APPEND == 0x400 && COSMO_O_DIRECTORY == 0x10000 && COSMO_O_NOFOLLOW == 0x20000 &&
                   COSMO_SEEK_SET == 0 && COSMO_SEEK_CUR == 1 && COSMO_DT_REG == 1 && COSMO_DT_DIR == 2 &&
                   COSMO_DT_LNK == 6 && COSMO_HOST_NAME_MAX == 64,
               "vfstest_base.cee's syscall.h values");
_Static_assert(VNODE_DIR == 2 && VNODE_DEAD == 2 && VFS_PATH_MAX == 1024 && VFS_MAX_SYMLINKS == 8 &&
                   HANDLE_RIGHT_OWNER == 0x1C && SCHED_PRIO_DEFAULT == 32 && PAGE_SIZE == 4096 && KMEM_ZERO == 1,
               "vfstest_base.cee's vfs.h, handle.h, thread.h, types.h and kmalloc.h values");
_Static_assert(ENOSPC == 28 && VNODE_REG == 1 && MOUNT_CACHE_IS_STORE == 2 && sizeof(struct pagecache_stats) == 11 * 8,
               "vfstest_mount.cee's errno.h, vfs.h and pagecache.h values");
_Static_assert(sizeof(struct cosmo_stat) == 7 * 8, "the self-tests' stat records: seven u64s");
