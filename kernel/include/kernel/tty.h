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

#include <kernel/process.h>
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
    /* The session that controls this terminal and, within it, the group
     * whose processes the control characters signal
     * (docs/kernel/tty/design.md, "The controlling terminal"). Both 0
     * until a session leader claims the terminal. Under `lock`. */
    pid_t sid;
    pid_t fg_pgid;
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

/* Replace the line-discipline flags (TTY_ECHO, TTY_ICRNL); returns what
 * they were. The keyboard test turns echo off while the harness types,
 * so what it types does not land in the middle of a log line. */
unsigned tty_set_flags(struct tty *t, unsigned flags);

/*
 * The controlling terminal (docs/kernel/tty/design.md).
 *
 * `tty_set_pgrp` names the foreground process group -- the one that
 * `^C` and `^\` signal. A session leader whose session has no terminal
 * yet claims one by naming a group on it; after that only processes of
 * that session may name a group, and only a group of that session.
 * `tty_get_pgrp` answers -ENOTTY to anyone outside the session, because
 * to them this is not a controlling terminal at all.
 */
int tty_set_pgrp(struct tty *t, pid_t pgid);
/* The leader of session `sid` has exited: any terminal that session
 * controlled is released -- SIGHUP to what was its foreground group,
 * and the terminal free for the next session leader to claim. */
void tty_session_exit(pid_t sid);
/* The foreground group as the kernel sees it, without the session check
 * the system calls make; 0 when the terminal has none. */
pid_t tty_foreground_pgrp(struct tty *t);
int tty_get_pgrp(struct tty *t, pid_t *out);
/* The tty behind a kobject, or NULL when the object is not a terminal. */
struct kobject;
struct tty *tty_of_object(struct kobject *obj);

#endif /* KERNEL_TTY_H */
