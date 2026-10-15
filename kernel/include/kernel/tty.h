/*
 * tty.h - The line discipline (docs/kernel/tty/).
 *
 * A tty collects bytes from a device in interrupt context, edits them
 * into lines (canonical mode: erase, kill, EOF, CR to NL, echo) and hands
 * complete lines to readers in thread context. The console tty is fed by
 * the serial receive interrupt and read through the console kobject
 * (handle 0 of every process that inherited it); since roadmap M4 each
 * pseudo-terminal's slave is a tty too (kernel/tty/pty.c), fed by writes
 * to its master and writing into the master's queue.
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
#define TTY_OPOST  COSMO_TTY_OPOST
#define TTY_TOSTOP COSMO_TTY_TOSTOP
/* The modes a terminal starts with and goes back to when its session ends. */
#define TTY_DEFAULT_MODES (TTY_ECHO | TTY_ICRNL | TTY_ICANON | TTY_ISIG | TTY_OPOST)

struct tty;
/*
 * Where a terminal's output goes: the console's sinks, or a pty master's
 * queue. `echo` runs with tty->lock held, possibly in interrupt context,
 * and must not block (a full queue drops); `write` runs in thread context
 * for a program's write and may block, honouring `nonblock`. `get`/`put`
 * keep a dynamic terminal alive while a caller holds it. NULL ops: the
 * console.
 */
struct tty_ops {
    void (*echo)(struct tty *t, const char *s, size_t n);
    int64_t (*write)(struct tty *t, const void *buf, size_t len, bool nonblock);
    void (*get)(struct tty *t);
    void (*put)(struct tty *t);
    /* The output not yet taken by the far side (a pty: what the master has
     * not read), dropping it, and waiting for it to go. NULL: nothing is
     * held (the console's sinks are synchronous). */
    unsigned (*out_queued)(struct tty *t);
    void (*flush_out)(struct tty *t);
    int (*drain)(struct tty *t);
};

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
    /* Roadmap M4. */
    const struct tty_ops *ops;     /* NULL: the console */
    void *priv;                    /* the ops' own (a pty's pair) */
    struct cosmo_ttysize size;     /* TIOCSWINSZ; under `lock` */
    bool size_set;                 /* `size` was set: the console otherwise asks the framebuffer */
    bool hungup;                   /* the far side is gone: reads return 0, writes -EIO; under `lock` */
    struct list_node link;         /* the registry (tty_register), for the controlling-terminal lookup */
};

/* Set up a tty (echo and CR->NL on). */
void tty_setup(struct tty *t, const char *name);
/* Add a terminal to / remove it from the registry: a terminal a session
 * may control must be findable from the session (tty_controlling). The
 * console is registered by tty_init. Any context but interrupt. */
void tty_register(struct tty *t);
void tty_unregister(struct tty *t);
/* A reference for a caller that keeps the pointer past the lookup that
 * found it (no-ops for the console). */
void tty_get(struct tty *t);
void tty_put(struct tty *t);
/* The terminal session `sid` controls, referenced, or NULL. */
struct tty *tty_controlling(pid_t sid);

/* One-time: the console tty. */
void tty_init(void);
struct tty *tty_console(void);

/* Deliver received bytes. Any context; never blocks, allocates or logs. */
void tty_input(struct tty *t, const uint8_t *bytes, size_t n);

/* Thread context. Blocks until a complete line exists; returns at most
 * one line (or a prefix of it when `len` is smaller), 0 at an EOF mark,
 * -EINTR when the calling process is being killed. */
int64_t tty_read(struct tty *t, void *buf, size_t len);
/* The same with the caller's non-blocking bit (a device file's open mode):
 * -EAGAIN instead of waiting when set and nothing is ready. tty_read is
 * this with false, the console object's mode. */
int64_t tty_read_nb(struct tty *t, void *buf, size_t len, bool nonblock);
/* A complete line (or an EOF mark) waits: tty_read would not block. Any context. */
bool tty_has_line(struct tty *t);
/* Whether a read would return rather than block: something is queued,
 * or `VMIN` 0 promises an answer without it. What poll readiness and
 * the non-blocking path ask, so that both agree with `tty_read`. */
bool tty_read_ready(struct tty *t);
/* A program's write to the terminal (thread context): with TOSTOP set, a
 * background group of the controlling session is stopped with SIGTTOU
 * first (or refused -EIO when orphaned); then the bytes go out through
 * the ops (a pty may block for room). -EIO once hung up. */
int64_t tty_write(struct tty *t, const void *buf, size_t len, bool nonblock);

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
/* TCFLSH / TCSETSF: drop queued input and the line under edit. */
void tty_flush_input(struct tty *t);
/* The output side (tty_ops): TIOCOUTQ, TCFLSH's output half, TCSETSW's wait. */
unsigned tty_out_queued(struct tty *t);
void tty_flush_output(struct tty *t);
int tty_drain_output(struct tty *t);
/* FIONREAD: the bytes queued for readers (a canonical line's terminator
 * counted, an EOF mark not). */
unsigned tty_queued(struct tty *t);
/* TIOCSWINSZ: a change raises SIGWINCH on the foreground group. */
void tty_set_size(struct tty *t, const struct cosmo_ttysize *in);
/* The framebuffer console's cell grid changed (fbcon_attach/fbcon_detach):
 * the console's size is the framebuffer's again, a size set by TIOCSWINSZ
 * dropped, and SIGWINCH goes to the foreground group. Thread context. */
void tty_console_resized(void);
/* The far side has gone (a pty's master closed): SIGHUP and SIGCONT to
 * the controlling session's foreground group and its leader, the session
 * lets go of the terminal, readers see end of file and writers -EIO. */
void tty_hangup(struct tty *t);

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
/* TIOCSCTTY: the caller, a session leader whose session controls no
 * terminal, takes `t` as its controlling terminal with its own group in
 * the foreground. -EPERM when it does not lead a session, its session has
 * a terminal, or another session controls `t`. */
int tty_set_controlling(struct tty *t);
/* The same on behalf of a just-created process (COSMO_SPAWN_SETSID), and
 * only when `t` is nobody's: false otherwise, which is not an error. */
bool tty_claim_for(struct tty *t, pid_t sid, pid_t pgid);
/* TIOCNOTTY: a session leader gives its terminal up (SIGHUP and SIGCONT to
 * the foreground group, as at its exit); any other member of the session
 * may ask and nothing changes, since control is the session's. -ENOTTY
 * when `t` is not the caller's controlling terminal. */
int tty_disassociate(struct tty *t);
/* TIOCGSID: the session controlling `t`, for a caller in that session;
 * -ENOTTY otherwise. */
int tty_get_sid(struct tty *t, pid_t *out);
/* The tty behind a kobject, or NULL when the object is not a terminal. */
struct kobject;
struct tty *tty_of_object(struct kobject *obj);
/* And behind an open file's vnode: `/dev/console` is the console,
 * `/dev/tty` is the caller's controlling terminal (NULL when it has
 * none), anything else is not a terminal (kernel/tty/ttydev.c). */
struct vnode;
struct tty *tty_of_vnode(const struct vnode *vn);
/* The tty behind whatever a handle holds -- the console object, an open
 * file on one of the terminal nodes, a pty's master or slave -- or NULL.
 * Every system call that resolves a handle to a terminal goes through
 * this. Referenced: tty_put when done. */
struct tty *tty_of_open(struct kobject *obj);
/* The path a terminal handle was opened by, for /proc/<pid>/fd: the
 * console object and /dev/console "/dev/console", /dev/tty "/dev/tty", a
 * pty master "/dev/ptmx", a slave "/dev/pts/N". False for anything else. */
bool tty_name_of_open(struct kobject *obj, char *buf, size_t n);
/* One-time: register /dev/console and /dev/tty. After the VFS. */
void tty_dev_init(void);

#endif /* KERNEL_TTY_H */
