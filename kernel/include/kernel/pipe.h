/*
 * pipe.h - Anonymous pipes (docs/kernel/ipc/).
 *
 * A pipe is a bounded byte stream with two kobject ends. The read end's
 * type has `read`, the write end's has `write`; both have `stat`. The
 * ends are the objects handles refer to; the pipe itself dies with the
 * last end.
 */

#ifndef KERNEL_PIPE_H
#define KERNEL_PIPE_H

#include <kernel/object.h>
#include <kernel/types.h>

#define PIPE_SIZE 16384u   /* ring capacity */
#define PIPE_BUF  4096u    /* writes up to this size are never interleaved */

struct pipe_stats {
    uint64_t created, alive, bytes;
};

/* Create a pipe; returns the two referenced end objects (one reference
 * each, owned by the caller). -ENOMEM on failure. */
int pipe_create(struct kobject **read_end, struct kobject **write_end);

void pipe_get_stats(struct pipe_stats *out);

#endif /* KERNEL_PIPE_H */
