/*
 * fifo.h - Named pipes: the pipe's ring behind a filesystem node
 * (docs/kernel/ipc/design.md, "Named pipes";
 * docs/audit/next-subsystem-named-pipes.md).
 *
 * A `struct fifo` belongs to a VNODE_FIFO node (the filesystem owns it:
 * ramfs allocates one at mknod and frees it at evict). Its ring exists
 * exactly while an open of the node does -- made by the first open,
 * freed by the last release -- and the ring's reader and writer counts
 * are the live opens of each side (invariant I9). The per-open state
 * (which side, non-blocking or not) lives in the open file's `priv`.
 *
 * The filesystem's vnode ops call these with the file; the fifo does not
 * know which filesystem holds it.
 */

#ifndef KERNEL_FIFO_H
#define KERNEL_FIFO_H

#include <kernel/types.h>

struct fifo;
struct file;
struct waitqueue;

/* A fifo with no ring and no opens; NULL when out of memory. */
struct fifo *fifo_alloc(void);
/* No open may remain (the ring is NULL); asserted. */
void fifo_free(struct fifo *fifo);

/*
 * POSIX's open rules, from f->flags: O_RDONLY waits for a writer,
 * O_WRONLY for a reader -- for the other side to have opened since this
 * open joined (its open generation), so a peer that came and went
 * meanwhile counts; with O_NONBLOCK a
 * read-only open returns at once and a write-only one is -ENXIO when no
 * reader is there; O_RDWR counts as both and never blocks. The wait is
 * killable (-EINTR). An open that fails leaves the fifo as it found it:
 * its count is taken back and a ring it made is freed.
 */
int fifo_open(struct fifo *fifo, struct file *f);
void fifo_release(struct fifo *fifo, struct file *f);
int64_t fifo_read(struct fifo *fifo, struct file *f, void *buf, size_t len);
int64_t fifo_write(struct fifo *fifo, struct file *f, const void *buf, size_t len);
unsigned fifo_ready(struct fifo *fifo, struct file *f);
struct waitqueue *fifo_poll_wq(struct fifo *fifo, struct file *f, unsigned events);
/* Per open, as POSIX has it: `on` 1/0 sets, -1 asks; returns the previous mode. */
int fifo_set_nonblock(struct fifo *fifo, struct file *f, int on);

/* Live rings held by fifos (the leak test). */
unsigned fifo_count(void);

#endif /* KERNEL_FIFO_H */
