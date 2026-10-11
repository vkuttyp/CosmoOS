/*
 * pty.h - Pseudo-terminals (kernel/tty/pty.c; docs/kernel/tty/design.md,
 * "Pseudo-terminals"; roadmap M4).
 *
 * /dev/ptmx clones a master/slave pair; the slave is /dev/pts/N on the pts
 * filesystem (a ramfs instance at /dev/pts). The slave is a struct tty with
 * the console's line discipline; the master reads what the slave writes
 * and echoes, and writes what the slave reads.
 */
#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include <kernel/types.h>

#define PTY_MAX     64u        /* pairs at once: /dev/pts/0 .. /dev/pts/63 */
#define PTY_OUT_MAX 8192u      /* the master's queue: slave output and echo */

struct file;
struct tty;

/* Boot, after /dev exists: mount /dev/pts, create /dev/ptmx. */
void pty_init(void);
/* The terminal behind a master or slave open file, referenced; NULL for
 * any other file. tty_of_open's third case. */
struct tty *pty_tty_of_file(struct file *f);
/* The master's ioctls: TIOCGPTN (the slave's number), TIOCSPTLCK and
 * TIOCGPTLCK (whether the slave may be opened). -ENOTTY for a file that is
 * not a master. */
int pty_get_index(struct file *f, unsigned *out);
int pty_set_lock(struct file *f, bool locked);
int pty_get_lock(struct file *f, bool *out);
/* "/dev/ptmx" for a master, "/dev/pts/N" for a slave; false otherwise. */
bool pty_name_of_file(struct file *f, char *buf, size_t n);
/* Bytes waiting in a master's queue (FIONREAD on the master); 0 otherwise. */
unsigned pty_master_queued(struct file *f);

struct pty_stats {
    uint64_t opened, freed;    /* pairs created by /dev/ptmx, and freed after their last reference */
};
void pty_get_stats(struct pty_stats *out);

#endif /* KERNEL_PTY_H */
