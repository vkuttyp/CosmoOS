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
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/tty.h>

#define TTY_EOF_MARK 0x04u

static struct tty g_console_tty;

void tty_setup(struct tty *t, const char *name)
{
    memset(t, 0, sizeof(*t));
    spinlock_init(&t->lock, "tty");
    waitqueue_init(&t->readers, "tty-readers");
    t->flags = TTY_ECHO | TTY_ICRNL;
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

void tty_input(struct tty *t, const uint8_t *bytes, size_t n)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    t->stats.rx_bytes += n;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' && (t->flags & TTY_ICRNL))
            c = '\n';
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
        } else if ((c >= 0x20 && c < 0x7f) || c == '\t') {
            if (t->line_len < TTY_LINE_MAX - 1) {
                t->line[t->line_len++] = c;
                echo(t, (const char *)&c, 1);
            } else {
                t->stats.dropped_bytes++;
                echo(t, "\a", 1);
            }
        }
        /* other control bytes are dropped (no signals yet, so ^C too) */
    }
    spin_unlock_irqrestore(&t->lock, s);
}

bool tty_has_line(struct tty *t)
{
    return __atomic_load_n(&t->lines, __ATOMIC_RELAXED) > 0;
}

int64_t tty_read(struct tty *t, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    uint8_t *out = buf;
    for (;;) {
        int rc = wait_event_killable(&t->readers, t->lines > 0);
        if (rc)
            return rc;
        arch_irq_state_t s = spin_lock_irqsave(&t->lock);
        if (t->lines == 0) {
            spin_unlock_irqrestore(&t->lock, s);
            continue;   /* another reader took the line */
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
