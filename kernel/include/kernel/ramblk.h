/*
 * ramblk.h - A RAM block device with a write recorder and snapshots, for
 * the crash-consistency and fault-injection harnesses
 * (docs/verification/design.md, "The RAM block device"). Debug builds.
 */

#ifndef KERNEL_RAMBLK_H
#define KERNEL_RAMBLK_H

#include <kernel/blk.h>

#define RAMBLK_BLOCK 4096u

/* One recorded write (or flush) in completion order. */
struct ramblk_write {
    uint64_t sector;
    uint32_t nsectors;   /* 0: a flush */
    uint8_t *data;       /* nsectors * 512 bytes, or NULL */
};

struct ramblk_log {
    struct ramblk_write *w;
    unsigned n, cap;
    unsigned dropped;    /* writes not recorded because the log was full */
};

/* Create and register a device of `nblocks` 4 KiB blocks (named ram<letter>).
 * The caller holds the creator's reference: ramblk_destroy drops it. */
struct blkdev *ramblk_create(uint64_t nblocks);
void ramblk_destroy(struct blkdev *bd);

/* Record every completed write and flush from now on / stop and hand the
 * log over (the caller frees it with ramblk_log_free). */
void ramblk_record_start(struct blkdev *bd, unsigned max_entries);
struct ramblk_log *ramblk_record_stop(struct blkdev *bd);
void ramblk_log_free(struct ramblk_log *log);
/* The log length right now (a sync point's position while recording). */
unsigned ramblk_record_count(struct blkdev *bd);

/* A copy of the whole device / the device made equal to a snapshot. */
uint8_t *ramblk_snapshot(struct blkdev *bd);
void ramblk_restore(struct blkdev *bd, const uint8_t *image);

/* Deferred mode for block-layer tests: completions run on a worker thread
 * and submit answers -EAGAIN above `limit` requests in flight; 0 returns
 * to synchronous completion (after completing what is deferred). */
void ramblk_set_deferred(struct blkdev *bd, unsigned limit);
/* Stall: in deferred mode the worker stops completing (a silent device);
 * the block layer's timeout thread then calls the driver's timeout
 * operation, which completes the request with -ETIMEDOUT. */
void ramblk_set_stall(struct blkdev *bd, bool stall);

/* Apply the first `count` entries of `log`; with `torn`, the last write is
 * applied only up to half its sectors (rounded down, at least one). */
void ramblk_replay(struct blkdev *bd, const struct ramblk_log *log, unsigned count, bool torn);

#endif /* KERNEL_RAMBLK_H */
