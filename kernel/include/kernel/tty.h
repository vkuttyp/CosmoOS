/*
 * tty.h - The line discipline (docs/kernel/tty/).
 *
 * A tty collects bytes from a device in interrupt context, edits them
 * into lines (canonical mode: erase, kill, EOF, CR to NL, echo) and hands
 * complete lines to readers in thread context. There is one console tty,
 * fed by the serial receive interrupt and read through the console
 * kobject (handle 0 of every process that inherited it).
 */

#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/wait.h>

#define TTY_LINE_MAX  1024u   /* one line under edit, including its newline */
#define TTY_INPUT_MAX 4096u   /* completed lines waiting for readers */

#define TTY_ECHO  (1u << 0)
#define TTY_ICRNL (1u << 1)

struct tty_stats {
    uint64_t rx_bytes, lines_in, lines_read, dropped_lines, dropped_bytes, eofs;
};

struct tty {
    spinlock_t lock;
    uint8_t line[TTY_LINE_MAX];
    unsigned line_len;
    uint8_t ring[TTY_INPUT_MAX];
    unsigned head, tail, used;
    unsigned lines;           /* complete lines (newline-terminated or EOF marks) in the ring */
    struct waitqueue readers;
    struct tty_stats stats;
    unsigned flags;
    const char *name;
};

/* Set up a tty (echo and CR->NL on). */
void tty_setup(struct tty *t, const char *name);

/* One-time: the console tty. */
void tty_init(void);
struct tty *tty_console(void);

/* Deliver received bytes. Any context; never blocks, allocates or logs. */
void tty_input(struct tty *t, const uint8_t *bytes, size_t n);

/* Thread context. Blocks until a complete line exists; returns at most
 * one line (or a prefix of it when `len` is smaller), 0 at an EOF mark,
 * -EINTR when the calling process is being killed. */
int64_t tty_read(struct tty *t, void *buf, size_t len);
/* A complete line (or an EOF mark) waits: tty_read would not block. Any context. */
bool tty_has_line(struct tty *t);

void tty_get_stats(struct tty *t, struct tty_stats *out);

#endif /* KERNEL_TTY_H */
