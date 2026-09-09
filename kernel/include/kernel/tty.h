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
#include <uapi/cosmo/syscall.h>
#include <kernel/types.h>
#include <kernel/wait.h>

#define TTY_LINE_MAX  1024u   /* one line under edit, including its newline */
#define TTY_INPUT_MAX 4096u   /* completed lines waiting for readers */

/* The line discipline's modes, the same bits the uapi exposes so that
 * nothing has to translate between them (docs/kernel/tty/design.md). */
#define TTY_ECHO   COSMO_TTY_ECHO
#define TTY_ICRNL  COSMO_TTY_ICRNL
#define TTY_ICANON COSMO_TTY_ICANON
#define TTY_ISIG   COSMO_TTY_ISIG

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
    /* Non-canonical mode: how a read behaves when there is nothing to
     * read yet. `vtime` is carried and not honoured (see the uapi). */
    uint8_t vmin, vtime;
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
/* Whether a read would return rather than block: something is queued,
 * or `VMIN` 0 promises an answer without it. What poll readiness and
 * the non-blocking path ask, so that both agree with `tty_read`. */
bool tty_read_ready(struct tty *t);

void tty_get_stats(struct tty *t, struct tty_stats *out);

/* The modes a program sees and sets (docs/kernel/tty/design.md,
 * "Modes"). Setting them drops whatever input is queued when the
 * canonical bit changes: the ring holds lines in one mode and bytes in
 * the other, and a reader must not be handed a mixture. */
void tty_get_termios(struct tty *t, struct cosmo_termios *out);
void tty_set_termios(struct tty *t, const struct cosmo_termios *in);
/* The terminal's size in characters, 0x0 when it does not know (the
 * serial console; the framebuffer console knows its own). */
void tty_get_size(struct tty *t, struct cosmo_ttysize *out);

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
/* The session that controls this terminal, 0 for none. */
pid_t tty_session_of(struct tty *t);
/* The foreground group as the kernel sees it, without the session check
 * the system calls make; 0 when the terminal has none. */
pid_t tty_foreground_pgrp(struct tty *t);
int tty_get_pgrp(struct tty *t, pid_t *out);
/* The tty behind a kobject, or NULL when the object is not a terminal. */
struct kobject;
struct tty *tty_of_object(struct kobject *obj);
/* And behind an open file's vnode: `/dev/console` is the console,
 * `/dev/tty` is the caller's controlling terminal (NULL when it has
 * none), anything else is not a terminal (kernel/tty/ttydev.c). */
struct vnode;
struct tty *tty_of_vnode(const struct vnode *vn);
/* The tty behind whatever a handle holds -- the console object or an
 * open file on one of the terminal nodes -- or NULL. Every system call
 * that resolves a handle to a terminal goes through this. */
struct tty *tty_of_open(struct kobject *obj);
/* One-time: register /dev/console and /dev/tty. After the VFS. */
void tty_dev_init(void);

#endif /* KERNEL_TTY_H */
