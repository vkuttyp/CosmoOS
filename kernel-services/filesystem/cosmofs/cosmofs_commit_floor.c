/*
 * cosmofs_commit_floor.c - The C under cosmofs_commit.cee (roadmap 2.0
 * SA7): the two test seams the commit has that a build configuration
 * decides -- the debug build's "the machine stops at this commit"
 * (test_commit_armed, test_commits_left, fields only a debug struct cfs
 * has) and test-snapshot's crash point during a rollback's switch
 * (COSMOFS_STOP=1 builds only). The commit is CeeSafe (cosmofs_commit.cee,
 * generated into cosmofs_commit.c).
 */

#include <kernel/log.h>
#include <kernel/thread.h>

#include "cosmofs_internal.h"

bool cfs_commit_floor_stopped(struct cfs *fs);
void cfs_commit_floor_switch_stop(struct cfs *fs);

/* Test hook: the machine "stops" at this commit. True when this is the
 * commit it stops at; otherwise one fewer commit to go. */
bool cfs_commit_floor_stopped(struct cfs *fs)
{
#if CONFIG_DEBUG
    if (fs->test_commit_armed) {
        if (fs->test_commits_left == 0)
            return true;
        fs->test_commits_left--;
    }
#else
    (void)fs;
#endif
    return false;
}

/* test-snapshot's crash point "during the switch": every block of the
 * rollback's transaction is written and flushed, its root is not. A
 * build with COSMOFS_STOP=1 only; the harness kills the machine here. */
void cfs_commit_floor_switch_stop(struct cfs *fs)
{
#if CONFIG_COSMOFS_STOP
    if (fs->switching) {
        kinfo("cosmofs: test stop: the rollback switch's blocks are written and its root is not");
        for (;;)
            thread_sleep_ms(1000);
    }
#else
    (void)fs;
#endif
}
