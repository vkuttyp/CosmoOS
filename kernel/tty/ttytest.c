/*
 * ttytest.c - Self-test of the line discipline on a private tty
 * (docs/kernel/tty/testing.md). Echo is off so the log stays clean.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/tty.h>
#include <kernel/wait.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "tty: " #cond;                                                         \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

static void feed(struct tty *t, const char *s)
{
    tty_input(t, (const uint8_t *)s, strlen(s));
}

struct reader {
    struct tty *tty;
    char buf[64];
    int64_t got;
    volatile bool done;
};

static void reader_thread(void *arg)
{
    struct reader *r = arg;
    r->got = tty_read(r->tty, r->buf, sizeof(r->buf));
    r->done = true;
    thread_exit(0);
}

bool selftest_tty_ldisc(const char **reason)
{
    static struct tty t;   /* 5 KiB: not on the stack */
    tty_setup(&t, "test");
    t.flags &= ~TTY_ECHO;
    char buf[64];

    /* Erase, kill, newline. */
    feed(&t, "abc\x7f" "d\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 4 && memcmp(buf, "abd\n", 4) == 0);
    feed(&t, "xyz\x15q\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && memcmp(buf, "q\n", 2) == 0);
    feed(&t, "\bnothing to erase\b\b\b\b\b\b\b\b\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 9 && memcmp(buf, "nothing \n", 9) == 0);

    /* CR becomes NL; two lines are two reads. */
    feed(&t, "a\rb\r");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && buf[0] == 'a');
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && buf[0] == 'b');

    /* A small buffer takes a prefix; the rest follows with its newline. */
    feed(&t, "hello\n");
    CHECK(tty_read(&t, buf, 3) == 3 && memcmp(buf, "hel", 3) == 0);
    CHECK(tty_read(&t, buf, sizeof(buf)) == 3 && memcmp(buf, "lo\n", 3) == 0);

    /* ^D: end of file on an empty line, otherwise the partial line. */
    feed(&t, "\x04");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 0);
    feed(&t, "par\x04");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 3 && memcmp(buf, "par", 3) == 0);
    feed(&t, "next\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 5);
    /* A ^D record read with a buffer that fits the text exactly. */
    feed(&t, "abc\x04");
    CHECK(tty_read(&t, buf, 3) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(t.lines == 0);   /* the terminator went with it */

    /* Control bytes other than the editing keys are dropped (^C too). */
    feed(&t, "x\x03y\x1b\x01z\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 4 && memcmp(buf, "xyz\n", 4) == 0);

    /* Line length: TTY_LINE_MAX - 1 characters plus the newline. */
    static char big[1200];
    memset(big, 'a', sizeof(big));
    tty_input(&t, (const uint8_t *)big, 1100);
    feed(&t, "\n");
    struct tty_stats st;
    tty_get_stats(&t, &st);
    CHECK(st.dropped_bytes == 1100 - (TTY_LINE_MAX - 1));
    static char line[TTY_LINE_MAX + 8];
    CHECK(tty_read(&t, line, sizeof(line)) == (int64_t)TTY_LINE_MAX);
    CHECK(line[TTY_LINE_MAX - 1] == '\n' && line[0] == 'a');

    /* Ring limit: lines beyond TTY_INPUT_MAX are dropped, older ones kept. */
    for (unsigned i = 0; i < 100; i++) {
        tty_input(&t, (const uint8_t *)big, 99);
        feed(&t, "\n");
    }
    tty_get_stats(&t, &st);
    CHECK(st.dropped_lines > 0 && t.lines == TTY_INPUT_MAX / 100);
    for (unsigned i = 0; i < TTY_INPUT_MAX / 100; i++)
        CHECK(tty_read(&t, line, sizeof(line)) == 100);
    CHECK(t.used == 0 && t.lines == 0);

    /* A blocked reader wakes when a line completes. */
    struct reader r = { .tty = &t };
    struct thread *th = thread_create(reader_thread, &r, "tty-reader", 32);
    CHECK(th != NULL);
    thread_sleep_ms(20);
    CHECK(!r.done);
    feed(&t, "wake\n");
    for (unsigned i = 0; i < 100 && !r.done; i++)
        thread_sleep_ms(10);
    CHECK(r.done && r.got == 5 && memcmp(r.buf, "wake\n", 5) == 0);
    thread_join(th);

    /* Zero-length read never blocks. */
    CHECK(tty_read(&t, buf, 0) == 0);
    tty_get_stats(&t, &st);
    CHECK(st.eofs == 1 && st.lines_in > 0);
    return true;
}
