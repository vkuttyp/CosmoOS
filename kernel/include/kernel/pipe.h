/*
 * pipe.h - The pipe's ring, and anonymous pipes over it (docs/kernel/ipc/).
 *
 * The ring (`struct pipe`) is a bounded byte stream with a count of live
 * readers and of live writers; it does not know who counts them. The
 * anonymous pipe is one client: two kobject ends, the read end's type
 * has `read`, the write end's has `write`, both have `stat`, and each
 * end is one reader or one writer for as long as it lives. A named
 * pipe (kernel/ipc/fifo.c) is the other client: the same ring behind a
 * filesystem node, its counts the opens of each side.
 */

#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/wait.h>

#define PIPE_SIZE 16384u   /* ring capacity */
#define PIPE_BUF  4096u    /* writes up to this size are never interleaved */

/*
 * The ring. `readers` and `writers` are the live readers and writers,
 * whatever they are (end objects, or a FIFO's opens); the ring gives end
 * of file when `writers` is zero and the ring is empty, and -EPIPE when
 * `readers` is zero. A client changes the counts under `lock` and wakes
 * the queue the other side sleeps on: `wr_wq` when a reader goes (writers
 * learn -EPIPE), `rd_wq` when a writer goes (readers learn EOF). The
 * lock is a leaf: never held while blocking or touching user memory.
 */
struct pipe {
    spinlock_t lock;
    uint8_t *buf;
    unsigned head, tail, used;
    unsigned readers, writers;
    struct waitqueue rd_wq, wr_wq;
};

struct pipe_stats {
    uint64_t created, alive, bytes;   /* rings (anonymous and named), and bytes read */
};

/* A ring with no readers and no writers; NULL when out of memory. */
struct pipe *pipe_ring_alloc(void);
/* The counts must both be zero: nothing is left that could reach it. */
void pipe_ring_free(struct pipe *p);
/* The stream: `nonblock` is the caller's mode (the thread's I/O-ring
 * mode is applied inside). A read returns 0 only when the ring is empty
 * and no writer remains, -EAGAIN when non-blocking and empty with a
 * writer; a write returns -EPIPE with no reader, lands at most PIPE_BUF
 * bytes whole, and a non-blocking write that cannot fit returns what it
 * wrote or -EAGAIN. Both waits are killable (-EINTR). */
int64_t pipe_ring_read(struct pipe *p, void *buf, size_t len, bool nonblock);
int64_t pipe_ring_write(struct pipe *p, const void *buf, size_t len, bool nonblock);
/* Readiness: READABLE with bytes, READABLE|HANGUP with no writer;
 * WRITABLE with PIPE_BUF free, WRITABLE|ERROR with no reader. */
unsigned pipe_ring_ready_rd(struct pipe *p);
unsigned pipe_ring_ready_wr(struct pipe *p);

/* Create an anonymous pipe; returns the two referenced end objects (one
 * reference each, owned by the caller). -ENOMEM on failure. */
int pipe_create(struct kobject **read_end, struct kobject **write_end);

void pipe_get_stats(struct pipe_stats *out);

#endif /* KERNEL_PIPE_H */
