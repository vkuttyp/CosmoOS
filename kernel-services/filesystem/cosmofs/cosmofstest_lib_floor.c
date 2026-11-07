/*
 * cosmofstest_lib_floor.c - The C under cosmofstest_lib.cee (roadmap 2.0
 * SA7): the engine mount's operation-boundary fault seams and their
 * self-test, cosmofs-fixture-cleanup -- both exist only in a build with
 * CONFIG_SELFTEST and CONFIG_FAULTINJECT, a choice CeeSafe's @cfg cannot
 * make for a function that is not exported -- the self-tests' one call to
 * cosmofs_stats, a name its struct has too, so an extern block cannot
 * declare both, and the layout the CeeSafe half writes as its own
 * constants, asserted here, since a self-test has no host test to assert
 * it.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/log.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"
#include "cosmofstest.h"

_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_CREAT == 0x40 && COSMO_O_TRUNC == 0x200 &&
                   VFS_UMOUNT_FORCE == 1 && EEXIST == 17 && CFS_BLOCK == 4096 &&
                   offsetof(struct cfs_mhdr, crc) == 24 && CFS_DVA(0, 12345) == 12345,
               "cosmofstest_lib.cee's syscall.h, vfs.h, errno.h and cosmofs_format.h values");
_Static_assert(sizeof(struct cosmofs_snap_info) == 104 && sizeof(struct cosmofs_snap_req) <= 96 &&
                   COSMOFS_SNAP_NAME_MAX + 1 == 32 && sizeof(struct cosmo_stat) <= 64,
               "cosmofstest_lib.cee's cosmofs.h and syscall.h sizes");

#define ENG "/mnt/eng"

/* cosmofs_stats for the CeeSafe self-tests; `out` is a struct cosmofs_stats. */
int cfst_stats(struct mount *mnt, void *out)
{
    return cosmofs_stats(mnt, out);
}

/* Only the serial self-test runner arms these operation-boundary seams. */
#if CONFIG_SELFTEST && CONFIG_FAULTINJECT
static unsigned engine_fail_stage;
static struct blkdev *engine_observed;
static uint64_t engine_fail_hits;

void cfst_floor_fail_begin(uint32_t stage, struct blkdev *bd)
{
    if (engine_fail_stage != stage)
        return;
    engine_observed = bd;
    blkdev_get(bd);   /* inspection reference, independent of fixture ownership */
    faultinject_set(stage == 1 ? FI_BLK_SUBMIT : FI_KMALLOC, 1, 1, thread_current());
}

void cfst_floor_fail_end(uint32_t stage)
{
    if (engine_fail_stage != stage)
        return;
    enum fi_kind kind = stage == 1 ? FI_BLK_SUBMIT : FI_KMALLOC;
    struct fi_stats st;
    faultinject_stats(kind, &st);
    engine_fail_hits = st.hits;
    faultinject_clear(kind);
}
#else
void cfst_floor_fail_begin(uint32_t stage, struct blkdev *bd)
{
    (void)stage;
    (void)bd;
}

void cfst_floor_fail_end(uint32_t stage)
{
    (void)stage;
}
#endif

#if CONFIG_DEBUG
bool selftest_cosmofs_fixture_cleanup(const char **reason)
{
#if CONFIG_SELFTEST && CONFIG_FAULTINJECT
    bool ok = true;
    for (unsigned stage = 1; stage <= 3; stage++) {
        (void)vfs_rmdir(NULL, ENG);
        engine_fail_stage = stage;
        engine_observed = NULL;
        engine_fail_hits = 0;
        struct blkdev *published = NULL;
        const char *why = NULL;
        bool mounted = cfst_engine_mount(&published, 64, &why);
        engine_fail_stage = 0;
        struct blkdev *bd = engine_observed;
        bool released = bd && bd->gone && __atomic_load_n(&bd->obj.refcount, __ATOMIC_RELAXED) == 1;
        bool injected = !mounted && published == NULL && why != NULL && engine_fail_hits == 1;
        kinfo("selftest: cosmofs-fixture-cleanup: stage=%u hits=%llu released=%u", stage,
              (unsigned long long)engine_fail_hits, released);
        /* Reclaim old-behavior leaks after observing them, so later tests remain independent. */
        if (mounted)
            (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
        if (bd) {
            if (!bd->gone)
                ramblk_destroy(bd);
            blkdev_put(bd);
        }
        engine_observed = NULL;
        (void)vfs_rmdir(NULL, ENG);
        ok = ok && injected && released;
    }
    if (!ok) {
        *reason = "fixture cleanup: registered RAM device retained after setup failure";
        return false;
    }
#else
    (void)reason;
#endif
    return true;
}
#endif
