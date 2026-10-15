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
#include <kernel/list.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/tty.h>

#define TTY_EOF_MARK 0x04u

static struct tty g_console_tty;

/* Every terminal a session may control: the console and each live pty
 * slave (docs/kernel/tty/design.md, "Many terminals"). Lock order:
 * g_ttys_lock -> tty.lock. */
static LIST_HEAD(g_ttys);
static spinlock_t g_ttys_lock = SPINLOCK_INIT("ttys");
/* Taking a controlling terminal is one step across the registry: "does the
 * session have one" and "this one is the session's" under one lock, or two
 * claims racing in one session could each find none (T17). Order:
 * g_claim_lock -> g_ttys_lock -> tty.lock. */
static spinlock_t g_claim_lock = SPINLOCK_INIT("tty-claim");

void tty_register(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_ttys_lock);
    list_push_back(&g_ttys, &t->link);
    spin_unlock_irqrestore(&g_ttys_lock, s);
}

void tty_unregister(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_ttys_lock);
    list_remove(&t->link);
    list_init(&t->link);
    spin_unlock_irqrestore(&g_ttys_lock, s);
}

void tty_get(struct tty *t)
{
    if (t->ops != NULL && t->ops->get != NULL)
        t->ops->get(t);
}

void tty_put(struct tty *t)
{
    if (t->ops != NULL && t->ops->put != NULL)
        t->ops->put(t);
}

/* A session controls at most one terminal: the one whose `sid` is its. */
struct tty *tty_controlling(pid_t sid)
{
    if (sid == 0)
        return NULL;
    struct tty *found = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_ttys_lock);
    struct tty *t;
    list_for_each_entry(t, &g_ttys, link) {
        arch_irq_state_t ts = spin_lock_irqsave(&t->lock);
        bool mine = t->sid == sid;
        spin_unlock_irqrestore(&t->lock, ts);
        if (mine) {
            found = t;
            tty_get(t);   /* under the registry lock: the terminal cannot be unregistered and freed first */
            break;
        }
    }
    spin_unlock_irqrestore(&g_ttys_lock, s);
    return found;
}

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
    t->flags = TTY_DEFAULT_MODES;
    t->name = name;
    list_init(&t->link);
}

void tty_init(void)
{
    tty_setup(&g_console_tty, "console");
    tty_register(&g_console_tty);
}

struct tty *tty_console(void)
{
    return &g_console_tty;
}

/* Lock held. */
static void echo(struct tty *t, const char *s, size_t n)
{
    if (!(t->flags & TTY_ECHO))
        return;
    if (t->ops != NULL && t->ops->echo != NULL)
        t->ops->echo(t, s, n);
    else
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
    /* A session controls one terminal: a leader whose session has another
     * may not claim this one by naming a group on it. The check and the
     * claim below are one step under g_claim_lock. */
    arch_irq_state_t cl = spin_lock_irqsave(&g_claim_lock);
    struct tty *other = tty_controlling(sid);
    if (other != NULL) {
        tty_put(other);
        if (other != t) {
            spin_unlock_irqrestore(&g_claim_lock, cl);
            return -EPERM;
        }
    }
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    pid_t fg = t->fg_pgid;
    int claim_rc = 0;
    if (t->sid == 0) {
        if (self->pid != sid)
            claim_rc = -EPERM;   /* only a session leader claims a terminal */
        else if (t->hungup)
            claim_rc = -EIO;     /* a hung-up slave is no one's terminal again, as in tty_claim_for */
        else
            t->sid = sid;   /* claimed and named in one step */
    } else if (t->sid != sid) {
        claim_rc = -EPERM;
    }
    spin_unlock_irqrestore(&t->lock, s);
    spin_unlock_irqrestore(&g_claim_lock, cl);
    if (claim_rc)
        return claim_rc;
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

/*
 * The session `sid` lets go of `t` (its leader exited, gave the terminal
 * up, or the far side hung up): the foreground group is told -- SIGHUP,
 * and SIGCONT after it so that a stopped job hears the hangup -- and the
 * terminal is free for the next session leader. `reset` also puts the
 * modes back (a terminal a person types at again); `leader_too` signals
 * the session's leader as well (a hangup: the leader is the shell the line
 * dropped under).
 */
static void tty_release(struct tty *t, pid_t sid, bool reset, bool hangup, bool leader_too)
{
    pid_t fg = 0;
    bool ended = false;
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    if (hangup)
        t->hungup = true;
    if (t->sid == sid && sid != 0) {
        ended = true;
        fg = t->fg_pgid;
        t->sid = 0;
        t->fg_pgid = 0;
        if (reset) {
            /*
             * And the modes go back to a terminal a person can type at. A
             * program that dies in raw mode has no shell left to restore
             * anything -- the shell only restores what *it* set -- so
             * without this the machine stays unusable until it is reset,
             * which is the one failure of terminal modes that is worse than
             * not having them.
             */
            t->flags = TTY_DEFAULT_MODES;
            t->vmin = 1;
            t->vtime = 0;
            t->head = t->tail = t->used = t->lines = 0;
            t->line_len = 0;
        }
    }
    spin_unlock_irqrestore(&t->lock, s);
    if (fg != 0) {
        tty_signal_group(t, fg, SIGHUP);
        tty_signal_group(t, fg, SIGCONT);
    }
    if (ended && leader_too) {
        struct process *leader = process_lookup(sid);
        if (leader != NULL) {
            if (leader->pgid != fg) {
                struct signal_info info = { .sig = SIGHUP, .source = SIGSRC_KERNEL };
                signal_send(leader, SIGHUP, &info);
                info.sig = SIGCONT;
                signal_send(leader, SIGCONT, &info);
            }
            process_put(leader);
        }
    }
    /* A poller of /dev/tty sleeps on `readers`; its answer just became
     * ERROR (no controlling terminal), so it is told -- whether or not a
     * foreground group was there to hang up (the device-readiness unit).
     * A hung-up terminal's readers return end of file. */
    if (ended || hangup)
        waitqueue_wake_all(&t->readers);
}

void tty_session_exit(pid_t sid)
{
    struct tty *t = tty_controlling(sid);
    if (t == NULL)
        return;
    tty_release(t, sid, true, false, false);
    tty_put(t);
}

void tty_hangup(struct tty *t)
{
    pid_t sid = tty_session_of(t);
    tty_release(t, sid, false, true, true);
}

int tty_set_controlling(struct tty *t)
{
    struct process *self = process_current();
    pid_t sid = 0, pgid = 0;
    process_current_ids(&pgid, &sid);
    if (self == NULL || self->pid != sid)
        return -EPERM;   /* only a session leader takes a controlling terminal */
    arch_irq_state_t cl = spin_lock_irqsave(&g_claim_lock);   /* the check and the claim: one step (T17) */
    struct tty *other = tty_controlling(sid);
    if (other != NULL) {
        spin_unlock_irqrestore(&g_claim_lock, cl);
        tty_put(other);
        return other == t ? 0 : -EPERM;
    }
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    int rc = 0;
    if (t->hungup)
        rc = -EIO;
    else if (t->sid != 0)
        rc = -EPERM;   /* another session's; stealing it is not offered */
    else {
        t->sid = sid;
        t->fg_pgid = pgid;
    }
    spin_unlock_irqrestore(&t->lock, s);
    spin_unlock_irqrestore(&g_claim_lock, cl);
    return rc;
}

bool tty_claim_for(struct tty *t, pid_t sid, pid_t pgid)
{
    arch_irq_state_t cl = spin_lock_irqsave(&g_claim_lock);
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    bool ok = t->sid == 0 && !t->hungup;
    if (ok) {
        t->sid = sid;
        t->fg_pgid = pgid;
    }
    spin_unlock_irqrestore(&t->lock, s);
    spin_unlock_irqrestore(&g_claim_lock, cl);
    return ok;
}

int tty_disassociate(struct tty *t)
{
    struct process *self = process_current();
    pid_t sid = process_current_sid();
    if (tty_session_of(t) != sid || sid == 0)
        return -ENOTTY;
    if (self != NULL && self->pid == sid)
        tty_release(t, sid, false, false, false);
    return 0;
}

int tty_get_sid(struct tty *t, pid_t *out)
{
    pid_t sid = process_current_sid();
    pid_t owner = tty_session_of(t);
    if (owner == 0 || owner != sid)
        return -ENOTTY;
    *out = owner;
    return 0;
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
    /* A reader already blocked was told to wait for a line under the old
     * modes; under the new ones it may have nothing left to wait for --
     * `VMIN` 0 means "answer with whatever is there, including nothing".
     * Waking it makes it re-read the modes rather than sleep on a
     * promise that has been withdrawn. */
    waitqueue_wake_all(&t->readers);
    spin_unlock_irqrestore(&t->lock, s);
}

void tty_flush_input(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    t->head = t->tail = t->used = t->lines = 0;
    t->line_len = 0;
    spin_unlock_irqrestore(&t->lock, s);
}

unsigned tty_out_queued(struct tty *t)
{
    return t->ops != NULL && t->ops->out_queued != NULL ? t->ops->out_queued(t) : 0;
}

void tty_flush_output(struct tty *t)
{
    if (t->ops != NULL && t->ops->flush_out != NULL)
        t->ops->flush_out(t);
}

int tty_drain_output(struct tty *t)
{
    return t->ops != NULL && t->ops->drain != NULL ? t->ops->drain(t) : 0;
}

unsigned tty_queued(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    unsigned n = t->used;
    if (t->flags & TTY_ICANON)
        for (unsigned i = 0, at = t->head; i < t->used; i++, at = (at + 1) % TTY_INPUT_MAX)
            if (t->ring[at] == TTY_EOF_MARK)
                n--;
    spin_unlock_irqrestore(&t->lock, s);
    return n;
}

void tty_set_size(struct tty *t, const struct cosmo_ttysize *in)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    bool changed = !t->size_set || t->size.cols != in->cols || t->size.rows != in->rows;
    t->size.cols = in->cols;
    t->size.rows = in->rows;
    t->size_set = true;
    t->size_gen = fbcon_switches();
    pid_t fg = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    /* A program drawing the whole screen redraws on SIGWINCH; one that
     * set the same size again has nothing to redraw. */
    if (changed && fg != 0)
        tty_signal_group(t, fg, SIGWINCH);
}

void tty_console_resized(uint32_t gen)
{
    struct tty *t = tty_console();
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    /* tty_get_size asks fbcon again: the window decides, as a terminal
     * emulator's does. A size set after the switch read `gen` (or a later
     * one) and is newer than it: kept. */
    if ((int32_t)(t->size_gen - gen) < 0)
        t->size_set = false;
    pid_t fg = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    if (fg != 0)
        tty_signal_group(t, fg, SIGWINCH);
}

void tty_get_size(struct tty *t, struct cosmo_ttysize *out)
{
    memset(out, 0, sizeof(*out));
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    bool set = t->size_set;
    if (set) {
        out->cols = t->size.cols;
        out->rows = t->size.rows;
    }
    spin_unlock_irqrestore(&t->lock, s);
    if (set || t != tty_console())
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
 * Whether a read would return rather than block, which is not the same
 * question: `VMIN` 0 promises an answer with nothing queued at all.
 * Everything that asks "would this block?" -- poll readiness, the
 * non-blocking path -- must answer it the same way `tty_read` does, or
 * a poll parks a caller that a read would have served. Relaxed loads,
 * like `tty_has_line`: readiness is a hint by nature, and the decision
 * to return nothing is made under the lock in `tty_read`.
 */
bool tty_read_ready(struct tty *t)
{
    if (__atomic_load_n(&t->lines, __ATOMIC_RELAXED) > 0 || __atomic_load_n(&t->hungup, __ATOMIC_RELAXED))
        return true;   /* a hung-up terminal answers at once: end of file */
    unsigned flags = __atomic_load_n(&t->flags, __ATOMIC_RELAXED);
    return !(flags & TTY_ICANON) && __atomic_load_n(&t->vmin, __ATOMIC_RELAXED) == 0;
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
    return tty_read_nb(t, buf, len, false);
}

int64_t tty_read_nb(struct tty *t, void *buf, size_t len, bool nonblock)
{
    if (len == 0)
        return 0;
    uint8_t *out = buf;
    for (;;) {
        int64_t allowed = tty_read_allowed(t);
        if (allowed != 0)
            return allowed;
        if (io_nonblocking(nonblock) && !tty_read_ready(t))
            return -EAGAIN;   /* a non-blocking open, or an I/O ring entry: it parks instead of waiting here */
        /* VMIN 0 in non-canonical mode: answer with whatever is there,
         * including nothing. Checked before the wait, which is the only
         * thing that distinguishes it -- and under the lock, because the
         * modes belong to it like every other field of the terminal. */
        arch_irq_state_t ms = spin_lock_irqsave(&t->lock);
        bool poll_only = !(t->flags & TTY_ICANON) && t->vmin == 0 && t->lines == 0;
        bool eof = t->hungup && t->lines == 0;   /* the far side is gone and nothing is left: end of file */
        spin_unlock_irqrestore(&t->lock, ms);
        if (poll_only || eof)
            return 0;
        /* The `VMIN` 0 term is what lets a mode change release a reader
         * that is already here: the condition is re-checked under the
         * lock at the top of the loop, so a racy read of it costs at
         * most one extra turn. */
        int rc = wait_event_killable(&t->readers,
                                     t->lines > 0 || t->hungup || (!(t->flags & TTY_ICANON) && t->vmin == 0));
        if (rc)
            return rc;
        arch_irq_state_t s = spin_lock_irqsave(&t->lock);
        if (t->lines == 0) {
            spin_unlock_irqrestore(&t->lock, s);
            continue;   /* another reader took the line, or a hangup: the top of the loop says which */
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

/*
 * A background job writing to its terminal, with TOSTOP set, is stopped
 * with SIGTTOU -- the write counterpart of the read rule, but optional,
 * because most terminals let background output through. An orphaned
 * group gets -EIO (nothing could continue it); a writer that ignores or
 * blocks SIGTTOU writes, which is what POSIX says (unlike the read, where
 * that case is -EIO).
 */
static int64_t tty_write_allowed(struct tty *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    bool tostop = (t->flags & TTY_TOSTOP) != 0;
    pid_t tty_sid = t->sid, fg = t->fg_pgid;
    spin_unlock_irqrestore(&t->lock, s);
    if (!tostop || tty_sid == 0 || fg == 0)
        return 0;
    pid_t sid = 0, pgid = 0;
    process_current_ids(&pgid, &sid);
    if (sid != tty_sid || pgid == fg)
        return 0;
    if (process_group_is_orphaned(pgid, sid))
        return -EIO;
    struct signal_info info = { .sig = SIGTTOU, .source = SIGSRC_KERNEL };
    if (!signal_raise_stop_self(SIGTTOU, &info))
        return 0;   /* ignored or blocked: the write goes ahead */
    return -EINTR;   /* the stop happens at the return to user mode; the write restarts after SIGCONT */
}

int64_t tty_write(struct tty *t, const void *buf, size_t len, bool nonblock)
{
    if (len == 0)
        return 0;
    int64_t rc = tty_write_allowed(t);
    if (rc != 0)
        return rc;
    if (__atomic_load_n(&t->hungup, __ATOMIC_ACQUIRE))
        return -EIO;
    if (t->ops != NULL && t->ops->write != NULL)
        return t->ops->write(t, buf, len, nonblock);
    console_write(buf, len);
    return (int64_t)len;
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
