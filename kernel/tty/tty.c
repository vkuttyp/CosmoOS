/*
 * tty.c - Canonical line discipline (docs/kernel/tty/design.md).
 *
 * tty_input runs in interrupt context under tty->lock and echoes through
 * console_write (its own IRQ-safe lock; lock order tty.lock -> console
 * lock, never the reverse). tty_read runs in thread context and takes
 * the same lock only around the copy out of the ring.
 *
 * The ring holds records: a line's bytes followed by its terminator,
 * which is '\n' for a normal line or TTY_EOF_MARK for a line ended by ^D
 * (an empty record with just the mark is an end of file). `lines` counts
 * records; printable input never contains the mark byte.
 */

#include <kernel/console.h>
#include <kernel/fbcon.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/tty.h>

#define TTY_EOF_MARK 0x04u

static struct tty g_console_tty;

/* Every process of the group, one signal each. The kernel is the sender,
 * so no credential check applies: the person at the keyboard is already
 * as privileged as this terminal's session. */
static void tty_signal_group(struct tty *t, pid_t pgid, int sig)
{
    /* An orphaned group must never be stopped: nothing is left in its
     * session to continue it, so ^Z would take it away for good. The
     * keystroke is dropped instead, which is what POSIX says and what
     * keeps this from being able to wedge the machine. */
    if (signal_default_is_stop(sig)) {
        pid_t sid;
        arch_irq_state_t s = spin_lock_irqsave(&t->lock);
        sid = t->sid;
        spin_unlock_irqrestore(&t->lock, s);
        if (process_group_is_orphaned(pgid, sid))
            return;
    }
    struct signal_info info = { .sig = sig, .source = SIGSRC_KERNEL };
    pid_t after = 0;
    struct process *p;
    while ((p = process_group_next(pgid, after)) != NULL) {
        after = p->pid;
        signal_send(p, sig, &info);
        process_put(p);
    }
}

void tty_setup(struct tty *t, const char *name)
{
    memset(t, 0, sizeof(*t));
    spinlock_init(&t->lock, "tty");
    waitqueue_init(&t->readers, "tty-readers");
    t->vmin = 1;   /* a non-canonical read blocks for one byte */
    t->flags = TTY_ECHO | TTY_ICRNL | TTY_ICANON | TTY_ISIG;
    t->name = name;
}

void tty_init(void)
{
    tty_setup(&g_console_tty, "console");
}

struct tty *tty_console(void)
{
    return &g_console_tty;
}

/* Lock held. */
static void echo(struct tty *t, const char *s, size_t n)
{
    if (t->flags & TTY_ECHO)
        console_write(s, n);
}

/* Lock held. Append the line under edit plus `term` as one record. */
static void commit(struct tty *t, uint8_t term)
{
    unsigned n = t->line_len + 1;
    if (TTY_INPUT_MAX - t->used >= n) {
        for (unsigned i = 0; i < t->line_len; i++) {
            t->ring[t->tail] = t->line[i];
            t->tail = (t->tail + 1) % TTY_INPUT_MAX;
        }
        t->ring[t->tail] = term;
        t->tail = (t->tail + 1) % TTY_INPUT_MAX;
        t->used += n;
        t->lines++;
        t->stats.lines_in++;
        if (term == TTY_EOF_MARK && t->line_len == 0)
            t->stats.eofs++;
        waitqueue_wake_all(&t->readers);
    } else {
        t->stats.dropped_lines++;
        echo(t, "\a", 1);
    }
    t->line_len = 0;
}

/* Lock held. Non-canonical mode: the byte goes straight into the ring
 * and `lines` counts bytes rather than records, so a reader takes what
 * is there without waiting for a terminator. Nothing is echoed and
 * nothing is edited -- a program in this mode is drawing its own line
 * and would have to undraw ours. */
static void push_raw(struct tty *t, uint8_t c)
{
    if (t->used >= TTY_INPUT_MAX) {
        t->stats.dropped_bytes++;
        return;
    }
    t->ring[t->tail] = c;
    t->tail = (t->tail + 1) % TTY_INPUT_MAX;
    t->used++;
    t->lines++;
    waitqueue_wake_all(&t->readers);
}

/*
 * The control characters that raise signals. What they name is a group,
 * not a process: everything in the foreground job stops at once, which
 * is the point of the group existing. The signal is sent after the lock
 * is dropped -- sending walks the process table and wakes threads, and
 * tty->lock is taken in interrupt context, so nothing that long-running
 * belongs under it.
 */
static int signal_char(uint8_t c)
{
    switch (c) {
    case 0x03: return SIGINT;    /* ^C */
    case 0x1c: return SIGQUIT;   /* ^\ */
    case 0x1a: return SIGTSTP;   /* ^Z */
    default: return 0;
    }
}

/*
 * The bytes up to and including the first one that raises a signal, or
 * all of them when none does. Lock held; returns how many were eaten
 * and, through `sig`/`pgid`, what the caller must send once it has let
 * the lock go. Stopping at the first signal is what keeps a batch
 * holding two of them from collapsing into one: `tty_input` comes
 * straight back for the rest.
 */
static size_t feed_locked(struct tty *t, const uint8_t *bytes, size_t n, int *sig, pid_t *pgid)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' && (t->flags & TTY_ICRNL))
            c = '\n';
        /* Non-canonical: every byte is data, including the ones that
         * would have been editing keys. The signal characters are still
         * signals if ISIG is on -- the two modes are independent, and a
         * program that wants ^C as a byte turns ISIG off as well. */
        if (!(t->flags & TTY_ICANON)) {
            if ((t->flags & TTY_ISIG) && signal_char(c) != 0 && t->fg_pgid != 0) {
                *sig = signal_char(c);
                *pgid = t->fg_pgid;
                t->stats.rx_bytes += i + 1;
                return i + 1;
            }
            push_raw(t, c);
            continue;
        }
        if (c == '\n') {
            echo(t, "\n", 1);
            commit(t, '\n');
        } else if (c == 0x7f || c == '\b') {
            if (t->line_len > 0) {
                t->line_len--;
                echo(t, "\b \b", 3);
            }
        } else if (c == 0x15) {   /* ^U: kill the line */
            while (t->line_len > 0) {
                t->line_len--;
                echo(t, "\b \b", 3);
            }
        } else if (c == 0x04) {   /* ^D: end of file, or end the partial line */
            commit(t, TTY_EOF_MARK);
        } else if ((t->flags & TTY_ISIG) && signal_char(c) != 0 && t->fg_pgid != 0) {
            /* Echoed the way every terminal echoes it, the line under
             * edit thrown away: what was typed before the interrupt is
             * not part of the next command. */
            char ctrl[2] = { '^', (char)('@' + c) };
            echo(t, ctrl, 2);
            echo(t, "\n", 1);
            t->line_len = 0;
            *sig = signal_char(c);
            *pgid = t->fg_pgid;
            t->stats.rx_bytes += i + 1;
            return i + 1;
        } else if ((c >= 0x20 && c < 0x7f) || c == '\t') {
            if (t->line_len < TTY_LINE_MAX - 1) {
                t->line[t->line_len++] = c;
                echo(t, (const char *)&c, 1);
            } else {
                t->stats.dropped_bytes++;
                echo(t, "\a", 1);
            }
        }
        /* other control bytes are dropped */
    }
    t->stats.rx_bytes += n;
    return n;
}

void tty_input(struct tty *t, const uint8_t *bytes, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int sig = 0;
        pid_t pgid = 0;
        arch_irq_state_t s = spin_lock_irqsave(&t->lock);
        off += feed_locked(t, bytes + off, n - off, &sig, &pgid);
        spin_unlock_irqrestore(&t->lock, s);
        /* Outside the lock: sending walks the process table and wakes
         * threads, and this runs in interrupt context, so nothing that
         * long belongs under a lock the whole line discipline shares. */
        if (sig != 0)
            tty_signal_group(t, pgid, sig);
    }
}

/*
 * The controlling terminal. A session leader whose session has no
 * terminal claims this one by naming a foreground group on it; from
 * then on the terminal belongs to that session, and only that session
 * may name a group -- which is what stops a second shell from taking
 * the keyboard away from the first.
 */
int tty_set_pgrp(struct tty *t, pid_t pgid)
{
    struct process *self = process_current();
    if (self == NULL || pgid == 0)
        return -EINVAL;
    pid_t sid = process_current_sid();
    /* The group must be one of the caller's session, checked first and
     * outside the tty lock because it walks the process table -- and
     * first so that a caller naming a group it may not name does not
     * claim the terminal on the way to being refused. A group that
     * empties between here and the store signals nobody, which is what
     * an empty group does anyway. */
    if (!process_group_in_session(pgid, sid))
        return -EPERM;
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t fg = t->fg_pgid;
    if (t->sid == 0) {
        if (self->pid != sid) {
            spin_unlock_irqrestore(&t->lock, s);
            return -EPERM;   /* only a session leader claims a terminal */
        }
        t->sid = sid;   /* claimed and named in one step */
    } else if (t->sid != sid) {
        spin_unlock_irqrestore(&t->lock, s);
        return -EPERM;
    }
    spin_unlock_irqrestore(&t->lock, s);
    /*
     * Changing the foreground group from a background process is a
     * SIGTTOU, unless the caller ignores or blocks it -- which every
     * shell does, because taking the terminal back after a job means
     * doing exactly this from the background. An orphaned group is not
     * stopped, here as anywhere.
     */
    pid_t mine = process_current_pgid();
    if (fg != 0 && mine != fg && !process_group_is_orphaned(mine, sid)) {
        struct signal_info info = { .sig = SIGTTOU, .source = SIGSRC_KERNEL };
        /* A caller that ignores or blocks SIGTTOU -- which every shell
         * does, because taking the terminal back after a job is done
         * from the background -- gets to make the call instead. Asked
         * and sent in one step, for the same reason as the read above. */
        if (signal_raise_stop_self(SIGTTOU, &info))
            return -EINTR;
    }
    s = spin_lock_irqsave(&t->lock);
    if (t->sid != sid) {
        spin_unlock_irqrestore(&t->lock, s);
        return -EPERM;   /* claimed by someone else in between */
    }
    t->fg_pgid = pgid;
    spin_unlock_irqrestore(&t->lock, s);
    return 0;
}

void tty_session_exit(pid_t sid)
{
    struct tty *t = tty_console();
    pid_t hangup = 0;
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    if (t->sid == sid) {
        hangup = t->fg_pgid;
        t->sid = 0;
        t->fg_pgid = 0;
        /*
         * And the modes go back to a terminal a person can type at. A
         * program that dies in raw mode has no shell left to restore
         * anything -- the shell only restores what *it* set -- so
         * without this the machine stays unusable until it is reset,
         * which is the one failure of terminal modes that is worse than
         * not having them.
         */
        t->flags = TTY_ECHO | TTY_ICRNL | TTY_ICANON | TTY_ISIG;
        t->vmin = 1;
        t->vtime = 0;
        t->head = t->tail = t->used = t->lines = 0;
        t->line_len = 0;
    }
    spin_unlock_irqrestore(&t->lock, s);
    if (hangup != 0)
        tty_signal_group(t, hangup, SIGHUP);
}

void tty_get_termios(struct tty *t, struct cosmo_termios *out)
{
    memset(out, 0, sizeof(*out));
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    out->modes = t->flags & COSMO_TTY_MODES;
    out->vmin = t->vmin;
    out->vtime = t->vtime;
    spin_unlock_irqrestore(&t->lock, s);
}

void tty_set_termios(struct tty *t, const struct cosmo_termios *in)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    unsigned was = t->flags;
    t->flags = in->modes & COSMO_TTY_MODES;
    t->vmin = in->vmin;
    t->vtime = in->vtime;
    /* The ring holds records in one mode and bare bytes in the other,
     * so anything queued across the change would be read as the wrong
     * shape. Dropping it is what POSIX's TCSAFLUSH does, and the only
     * honest option when the two formats cannot be told apart. */
    if ((was & TTY_ICANON) != (t->flags & TTY_ICANON)) {
        t->head = t->tail = t->used = t->lines = 0;
        t->line_len = 0;
    }
    spin_unlock_irqrestore(&t->lock, s);
}

void tty_get_size(struct tty *t, struct cosmo_ttysize *out)
{
    memset(out, 0, sizeof(*out));
    if (t != tty_console())
        return;
    /*
     * The framebuffer console knows its geometry; a serial line does
     * not, and there is no way to ask one. Zero is the honest answer
     * there -- a made-up 80x24 is a lie a program cannot detect, and a
     * program that gets 0 can fall back to its own default knowing that
     * is what it is doing.
     */
    struct fbcon_geometry g;
    if (fbcon_geometry(&g)) {
        out->cols = (uint16_t)g.cols;
        out->rows = (uint16_t)g.rows;
    }
}

pid_t tty_session_of(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t sid = t->sid;
    spin_unlock_irqrestore(&t->lock, s);
    return sid;
}

pid_t tty_foreground_pgrp(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t pgid = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    return pgid;
}

int tty_get_pgrp(struct tty *t, pid_t *out)
{
    pid_t sid = process_current_sid();
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    int rc = (t->sid != 0 && t->sid == sid) ? 0 : -ENOTTY;
    if (rc == 0)
        *out = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    return rc;
}

bool tty_has_line(struct tty *t)
{
    return __atomic_load_n(&t->lines, __ATOMIC_RELAXED) > 0;
}

/*
 * A reader that is not in the terminal's foreground group is stopped
 * with SIGTTIN rather than allowed to take the line the shell is
 * waiting for -- without this, background jobs and a usable terminal
 * are mutually exclusive. An *orphaned* group gets -EIO instead: it
 * cannot be stopped, because nothing is left to continue it.
 *
 * Returns 0 to go ahead, or a negative errno. A stop is not an error:
 * the caller loops and asks again once it has been continued, by which
 * time it may legitimately be the foreground group.
 */
static int64_t tty_read_allowed(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t tty_sid = t->sid;
    pid_t fg = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    if (tty_sid == 0 || fg == 0)
        return 0;   /* nobody's terminal: the old behaviour */
    pid_t sid = 0, pgid = 0;
    process_current_ids(&pgid, &sid);
    if (sid != tty_sid || pgid == fg)
        return 0;   /* another session's reader is not this one's business */
    /*
     * Two ways the stop cannot happen, and both must fail the read
     * rather than pretend: an orphaned group has nothing left to
     * continue it, and a caller that blocks or ignores SIGTTIN will
     * never be stopped by it. Returning -EINTR in either case would
     * hand a retrying program an interruption that never becomes a
     * stop, and it would retry for ever. POSIX says -EIO.
     */
    if (process_group_is_orphaned(pgid, sid))
        return -EIO;
    struct signal_info info = { .sig = SIGTTIN, .source = SIGSRC_KERNEL };
    if (!signal_raise_stop_self(SIGTTIN, &info))
        return -EIO;   /* ignored or blocked: no stop will follow */
    return -EINTR;   /* the stop happens at the return to user mode */
}

int64_t tty_read(struct tty *t, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    uint8_t *out = buf;
    for (;;) {
        int64_t allowed = tty_read_allowed(t);
        if (allowed != 0)
            return allowed;
        if (io_nonblocking(false) && !tty_has_line(t))
            return -EAGAIN;   /* an I/O ring entry: it parks instead of waiting here */
        /* VMIN 0 in non-canonical mode: answer with whatever is there,
         * including nothing. Checked before the wait, which is the only
         * thing that distinguishes it -- and under the lock, because the
         * modes belong to it like every other field of the terminal. */
        arch_irq_state_t ms = spin_lock_irqsave(&t->lock);
        bool poll_only = !(t->flags & TTY_ICANON) && t->vmin == 0 && t->lines == 0;
        spin_unlock_irqrestore(&t->lock, ms);
        if (poll_only)
            return 0;
        int rc = wait_event_killable(&t->readers, t->lines > 0);
        if (rc)
            return rc;
        arch_irq_state_t s = spin_lock_irqsave(&t->lock);
        if (t->lines == 0) {
            spin_unlock_irqrestore(&t->lock, s);
            continue;   /* another reader took the line */
        }
        /* Non-canonical: `lines` counts bytes, so take what is there up
         * to the caller's buffer. There is no terminator to look for --
         * the record structure belongs to the canonical mode. */
        if (!(t->flags & TTY_ICANON)) {
            size_t got = 0;
            while (got < len && t->used > 0 && t->lines > 0) {
                out[got++] = t->ring[t->head];
                t->head = (t->head + 1) % TTY_INPUT_MAX;
                t->used--;
                t->lines--;
            }
            t->stats.lines_read++;
            spin_unlock_irqrestore(&t->lock, s);
            return (int64_t)got;
        }
        size_t n = 0;
        bool ended = false;
        while (t->used > 0) {
            uint8_t c = t->ring[t->head];
            if (c == TTY_EOF_MARK) {
                /* Terminator of a ^D record: consumed, not delivered. */
                t->head = (t->head + 1) % TTY_INPUT_MAX;
                t->used--;
                ended = true;
                break;
            }
            if (n == len)
                break;   /* the rest of the line waits for the next read */
            t->head = (t->head + 1) % TTY_INPUT_MAX;
            t->used--;
            out[n++] = c;
            if (c == '\n') {
                ended = true;
                break;
            }
        }
        if (ended)
            t->lines--;
        t->stats.lines_read++;
        spin_unlock_irqrestore(&t->lock, s);
        return (int64_t)n;   /* 0 only for an empty ^D record: end of file */
    }
}

void tty_get_stats(struct tty *t, struct tty_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    *out = t->stats;
    spin_unlock_irqrestore(&t->lock, s);
}


/* Module ABI v1 exports (docs/kernel/module/api.md). A driver for an
 * input device hands its bytes to the console tty exactly as the UARTs
 * do: usb_hid is the first, and the reason these are exported. */
#include <kernel/module.h>
EXPORT_SYMBOL(tty_console);
EXPORT_SYMBOL(tty_input);
