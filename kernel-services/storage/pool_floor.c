/*
 * pool_floor.c - The C under kernel-services/storage/pool.cee (roadmap 2.0
 * SA7): g_pool_fault_errors, the sysctl's variable (fs.fault_errors), the
 * spare slots, an array of pointers CeeSafe does not import, and the values
 * the port writes as its own constants. The pool is CeeSafe (pool.cee,
 * generated into pool.c).
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/storage.h>

_Static_assert(EINVAL == 22 && ENOSPC == 28 && EEXIST == 17 && ENODEV == 19 && EIO == 5 && ENOMEM == 12 &&
                   EAGAIN == 11 && EBUSY == 16 && ENOENT == 2 && BIO_PREFLUSH == 1 && BIO_FUA == 2,
               "pool.cee's errno.h and blk.h values");
_Static_assert(POOL_BLOCK == 4096 && POOL_MAX_MEMBERS == 255 && POOL_MAX_COPIES == 4 && POOL_MAX_SPARES == 4 &&
                   POOL_WRITE_ONLINE == 0x80000000u && POOL_DVA_SHIFT == 56 &&
                   POOL_DVA_BLK_MASK == 0x00FFFFFFFFFFFFFFull && POOL_DIRTY_BYTES == 944 &&
                   POOL_DIRTY_BITS == 7552,
               "pool.cee's storage.h values");
_Static_assert(POOL_COPY_ONLINE == 0 && POOL_COPY_MISSING == 1 && POOL_COPY_OFFLINE == 2 && POOL_COPY_FAULTED == 3 &&
                   POOL_COPY_RESILVERING == 4 && POOL_COPY_NSTATES == 5,
               "pool.cee's copy states");
_Static_assert(sizeof(struct pool_copy) <= 16 * 8, "pool.cee swaps a copy through 16 words");

uint32_t g_pool_fault_errors = 16;

uint32_t *pool_floor_fault_errors(void);
struct blkdev **pool_floor_spare(struct spool *p, uint32_t i);
uint64_t *pool_floor_spare_tag(struct spool *p, uint32_t i);

uint32_t *pool_floor_fault_errors(void) { return &g_pool_fault_errors; }

struct blkdev **pool_floor_spare(struct spool *p, uint32_t i) { return &p->spare[i]; }

uint64_t *pool_floor_spare_tag(struct spool *p, uint32_t i) { return &p->spare_tag[i]; }
