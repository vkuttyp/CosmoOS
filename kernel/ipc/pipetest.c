/*
 * pipetest.c - Self-test of anonymous pipes between kernel threads
 * (docs/kernel/ipc/testing.md).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/pipe.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "pipe: " #cond;                                                        \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

static int64_t obj_read(struct kobject *o, void *b, size_t n)
{
    return ((const struct kobject_io_type *)o->type)->read(o, b, n);
}

static int64_t obj_write(struct kobject *o, const void *b, size_t n)
{
    return ((const struct kobject_io_type *)o->type)->write(o, b, n);
}

static uint8_t pattern(uint32_t i)
{
    return (uint8_t)(i * 7 + (i >> 8));
}

#define STREAM_BYTES (1u << 20)

struct writer {
    struct kobject *wr;
    int result;
};

static void stream_writer(void *arg)
{
    struct writer *w = arg;
    uint8_t *chunk = kmalloc(9000, 0);
    uint32_t sent = 0;
    unsigned step = 1;
    w->result = 0;
    while (sent < STREAM_BYTES && chunk) {
        uint32_t n = (step * 613u) % 9000u + 1;
        if (n > STREAM_BYTES - sent)
            n = STREAM_BYTES - sent;
        for (uint32_t i = 0; i < n; i++)
            chunk[i] = pattern(sent + i);
        int64_t rc = obj_write(w->wr, chunk, n);
        if (rc != (int64_t)n) {
            w->result = rc < 0 ? (int)rc : -EIO;
            break;
        }
        sent += n;
        step++;
    }
    kfree(chunk);
    kobject_put(w->wr);   /* EOF for the reader */
    thread_exit(0);
}

struct record_writer {
    struct kobject *wr;
    uint8_t value;
    int result;
};

#define RECORD_LEN 1000u
#define RECORDS    200u

static void record_writer(void *arg)
{
    struct record_writer *w = arg;
    uint8_t rec[RECORD_LEN];
    memset(rec, w->value, sizeof(rec));
    w->result = 0;
    for (unsigned i = 0; i < RECORDS; i++) {
        int64_t rc = obj_write(w->wr, rec, sizeof(rec));
        if (rc != (int64_t)sizeof(rec)) {
            w->result = -EIO;
            break;
        }
    }
    kobject_put(w->wr);
    thread_exit(0);
}

bool selftest_ipc_pipe(const char **reason)
{
    struct pipe_stats s0, s1;
    pipe_get_stats(&s0);
    struct kobject *rd, *wr;

    /* A stream through mismatched chunk sizes, then EOF. */
    CHECK(pipe_create(&rd, &wr) == 0);
    struct writer w = { .wr = wr };
    struct thread *t = thread_create(stream_writer, &w, "pipe-writer", 32);
    CHECK(t != NULL);
    uint8_t *buf = kmalloc(7000, 0);
    CHECK(buf != NULL);
    uint32_t got = 0;
    unsigned step = 3;
    bool ok = true;
    for (;;) {
        uint32_t want = (step * 331u) % 7000u + 1;
        int64_t n = obj_read(rd, buf, want);
        if (n == 0)
            break;
        if (n < 0 || n > (int64_t)want) {
            ok = false;
            break;
        }
        for (int64_t i = 0; i < n; i++)
            if (buf[i] != pattern(got + (uint32_t)i))
                ok = false;
        got += (uint32_t)n;
        step++;
    }
    kfree(buf);
    thread_join(t);
    CHECK(ok && got == STREAM_BYTES && w.result == 0);
    CHECK(obj_read(rd, buf, 0) == 0);
    kobject_put(rd);

    /* No reader: -EPIPE. */
    CHECK(pipe_create(&rd, &wr) == 0);
    kobject_put(rd);
    CHECK(obj_write(wr, "x", 1) == -EPIPE);
    kobject_put(wr);

    /* fstat shape and the byte count. */
    CHECK(pipe_create(&rd, &wr) == 0);
    CHECK(obj_write(wr, "abc", 3) == 3);
    struct cosmo_stat st;
    CHECK(((const struct kobject_io_type *)rd->type)->stat(rd, &st) == 0);
    CHECK(st.type == COSMO_DT_FIFO && st.size == 3);
    uint8_t small[8];
    CHECK(obj_read(rd, small, 8) == 3 && memcmp(small, "abc", 3) == 0);
    kobject_put(wr);
    CHECK(obj_read(rd, small, 8) == 0);   /* EOF */
    kobject_put(rd);

    /* Writes no larger than PIPE_BUF from two writers never interleave. */
    CHECK(pipe_create(&rd, &wr) == 0);
    struct record_writer a = { .wr = wr, .value = 0x11 }, b = { .wr = wr, .value = 0x22 };
    kobject_get(wr);   /* two writer references */
    struct thread *ta = thread_create(record_writer, &a, "pipe-wa", 32);
    struct thread *tb = thread_create(record_writer, &b, "pipe-wb", 32);
    CHECK(ta && tb);
    uint8_t *rec = kmalloc(RECORD_LEN, 0);
    CHECK(rec != NULL);
    unsigned records = 0, bad = 0;
    for (;;) {
        unsigned have = 0;
        int64_t n = 0;
        while (have < RECORD_LEN) {
            n = obj_read(rd, rec + have, RECORD_LEN - have);
            if (n <= 0)
                break;
            have += (unsigned)n;
        }
        if (have == 0 && n == 0)
            break;
        if (have != RECORD_LEN) {
            bad++;
            break;
        }
        for (unsigned i = 1; i < RECORD_LEN; i++)
            if (rec[i] != rec[0])
                bad++;
        records++;
    }
    kfree(rec);
    thread_join(ta);
    thread_join(tb);
    CHECK(bad == 0 && records == 2 * RECORDS && a.result == 0 && b.result == 0);
    kobject_put(rd);

    pipe_get_stats(&s1);
    CHECK(s1.created == s0.created + 4 && s1.alive == s0.alive);
    kinfo("selftest: ipc-pipe: %u KiB streamed, %u records", STREAM_BYTES >> 10, records);
    return true;
}
