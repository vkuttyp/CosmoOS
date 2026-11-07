/*
 * cosmofstest_freelog_floor.c - The C under cosmofstest_freelog.cee
 * (roadmap 2.0 SA7): a filesystem's statistics as plain numbers --
 * cosmofs_stats is a function whose name its struct's takes in CeeSafe,
 * so neither can be declared there -- and the values the CeeSafe half
 * writes as its own constants, asserted here, since a self-test has no
 * host test to assert them. cosmofstest_names.cee reads the statistics
 * through this file too.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(ENOENT == 2 && EEXIST == 17 && EOPNOTSUPP == 95 && VFS_UMOUNT_FORCE == 1 && COSMO_DT_REG == 1 &&
                   COSMO_DT_LNK == 6 && CFS_FORMAT_VERSION == 11 && CFS_DEAD_PER_BLOCK == 506,
               "cosmofstest_freelog.cee's errno.h, vfs.h, syscall.h and cosmofs_format.h values");
_Static_assert(sizeof(struct cosmo_stat) <= 64 && offsetof(struct cosmo_stat, type) == 8 &&
                   offsetof(struct cosmo_stat, nlink) == 16 && sizeof(struct cosmofs_check_report) <= 2048,
               "cosmofstest_freelog.cee's cosmo_stat offsets and the check report's slot");

int cfst_fl_stats(struct mount *mnt, uint64_t *out);

/* cosmofs_stats of `mnt` into out[6]: generation, free_blocks,
 * total_blocks, members, version, free_root; cosmofs_stats's result. */
int cfst_fl_stats(struct mount *mnt, uint64_t *out)
{
    struct cosmofs_stats st;
    int rc = cosmofs_stats(mnt, &st);
    if (rc != 0)
        return rc;
    out[0] = st.generation;
    out[1] = st.free_blocks;
    out[2] = st.total_blocks;
    out[3] = st.members;
    out[4] = st.version;
    out[5] = st.free_root;
    return 0;
}
