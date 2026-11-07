/*
 * cosmofstest.h - What the cosmofs self-tests share (roadmap 2.0 SA7):
 * the helpers cosmofstest_lib.cee defines (generated into
 * cosmofstest_lib.c) and cosmofstest_lib_floor.c's seams, by their C
 * names, for the tests still in C.
 */

#ifndef COSMOFSTEST_H
#define COSMOFSTEST_H

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/types.h>

struct mount;

/* The scratch virtio disk (vda), referenced, or NULL. */
struct blkdev *cfst_scratch(void);
/* `path` created or truncated, holding `len` bytes of `data`. */
bool cfst_write_file(const char *path, const void *data, size_t len);
/* Whether `path` holds exactly the `len` bytes of `data`. */
bool cfst_read_matches(const char *path, const void *data, size_t len);
/* The mount `path` is on, or NULL. */
struct mount *cfst_mount_of(const char *path);
/* `len` bytes nothing can compress, from `seed`. */
void cfst_fill_incompressible(uint8_t *buf, size_t len, uint32_t seed);
/* A file of `pages` blocks nothing can compress. */
bool cfst_write_wide_file(const char *path, uint32_t pages);
/* A RAM device of `nblocks` blocks formatted and mounted at /mnt/eng,
 * write-back off; on failure nothing is left behind. */
bool cfst_engine_mount(struct blkdev **bdp, uint64_t nblocks, const char **reason);
/* /mnt/eng unmounted and removed, its device destroyed. */
bool cfst_engine_unmount(struct blkdev *bd, const char **reason);
/* Pool block `blkno` with byte `off` flipped, behind the filesystem's back. */
bool cfst_corrupt_block(struct blkdev *bd, uint64_t blkno, uint32_t off);
/* A metadata block's CRC32C with its header's CRC field taken as zero. */
uint32_t cfst_block_crc(const uint8_t *block);
/* One copy of one block overwritten with `fill`: a rotting disk. */
bool cfst_rot_copy(struct blkdev *bd, uint64_t blk, uint8_t fill);
/* The structural check of /mnt/eng into `rep`; false if the check itself failed. */
bool cfst_check_is_clean(const char **reason, struct cosmofs_check_report *rep);
/* /mnt/eng with one file and one directory, write-back off. */
bool cfst_check_fixture(struct blkdev **bd, const char **reason);
/* The classes of a report that were not empty, for a failure message. */
const char *cfst_check_why(const struct cosmofs_check_report *r);
/* The fixture's file's inode number, or 0. */
uint64_t cfst_check_file_ino(void);
/* The fixture unmounted and removed, its device (if any) destroyed. */
void cfst_check_teardown(struct blkdev *bd);
/* One snapshot operation (enum cosmofs_snap_op) on /mnt/eng. */
int cfst_snapctl(uint32_t op, const char *name, struct cosmofs_snap_info *out, uint32_t max, uint32_t *count);
/* The record of `v` named `name`, or NULL. */
const struct cosmofs_snap_info *cfst_info_named(const struct cosmofs_snap_info *v, uint32_t n, const char *name);
/* /mnt/eng unmounted and mounted again, write-back off. */
bool cfst_remount_eng(struct blkdev *bd, const char **reason);

/* cosmofstest_lib_floor.c: the engine mount's fault seams. */
void cfst_floor_fail_begin(uint32_t stage, struct blkdev *bd);
void cfst_floor_fail_end(uint32_t stage);

/* `CHECK_CLEAN(r)`: a report that is not clean fails the test, its classes the reason. */
#define CHECK_CLEAN(r)                                                         \
    do {                                                                       \
        if (!(r).clean) {                                                      \
            *reason = cfst_check_why(&(r));                                    \
            return false;                                                      \
        }                                                                      \
    } while (0)

#endif /* COSMOFSTEST_H */
