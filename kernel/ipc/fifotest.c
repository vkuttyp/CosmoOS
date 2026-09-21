/*
 * fifotest.c - Self-test of named pipes (docs/kernel/ipc/testing.md;
 * docs/audit/next-subsystem-named-pipes.md).
 *
 * The FIFO is exercised as a program would reach it: a node made by
 * vfs_mknod, files from vfs_open, readiness through the file kobject's
 * type. Live rings (fifo_count, pipe_stats.alive) are counted before
 * and after: the leak part of invariant I9 is asserted, not hoped for.
 * The killed opener is a real process (init --probe), because a kernel
 * thread has no process to kill.
 */

#include <kernel/bootarchive.h>
#include <kernel/errno.h>
#include <kernel/fifo.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "fifo: " #cond;                                                        \
            ok = false;                                                                      \
            goto out;                                                                        \
        }                                                                                    \
    } while (0)

#define FIFO_PATH "/tmp/fifo-t"
#define KILL_PATH "/tmp/fifo-k"

#define O_RD   COSMO_O_RDONLY
#define O_WR   COSMO_O_WRONLY
#define O_RW   COSMO_O_RDWR
#define O_NB   COSMO_O_NONBLOCK

/* An open from a second thread, so the calling thread can see whether it waits. */
struct opener {
    const char *path;
    unsigned flags;
    struct file *f;
    int rc;
    unsigned done;
};

static void opener_main(void *arg)
{
    struct opener *o = arg;
    o->rc = vfs_open(NULL, o->path, o->flags, 0, &o->f);
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    thread_exit(0);
}

static struct thread *start_opener(struct opener *o)
{
    o->f = NULL;
    o->rc = -1;
    o->done = 0;
    return thread_create(opener_main, o, "fifo-open", 32);
}

/* True if `*flag` became set within `ms`. */
static bool flag_within(const unsigned *flag, unsigned ms)
{
    uint64_t end = clock_now_ns() + (uint64_t)ms * 1000000ull;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() > end)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static bool count_within(unsigned want, unsigned ms)
{
    uint64_t end = clock_now_ns() + (uint64_t)ms * 1000000ull;
    while (fifo_count() != want) {
        if (clock_now_ns() > end)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static int open_at(const char *path, unsigned flags, struct file **out)
{
    return vfs_open(NULL, path, flags, 0, out);
}

static void put(struct file **f)
{
    if (*f) {
        file_put(*f);
        *f = NULL;
    }
}

bool selftest_ipc_fifo(const char **reason)
{
    bool ok = true;
    struct file *rf = NULL, *wf = NULL, *rf2 = NULL, *wf2 = NULL, *xf = NULL;
    struct thread *t = NULL;
    struct opener o = { 0 };
    struct process *p = NULL;
    struct vnode *vn = NULL;
    char buf[32];
    struct pipe_stats s0, s1;
    pipe_get_stats(&s0);
    unsigned f0 = fifo_count();
    (void)vfs_unlink(NULL, FIFO_PATH);
    (void)vfs_unlink(NULL, KILL_PATH);

    /* The node: DT_FIFO, the mode as given, owned by the maker; only a
     * FIFO or a socket is a special node, and only where the filesystem
     * has mknod. */
    CHECK(vfs_mknod(NULL, FIFO_PATH, 0644, VNODE_FIFO, &vn) == 0);
    CHECK(vn->type == VNODE_FIFO && vn->mode == 0644);
    struct cosmo_stat st;
    vnode_stat(vn, &st);
    CHECK(st.type == COSMO_DT_FIFO);
    vnode_put(vn);
    vn = NULL;
    CHECK(vfs_mknod(NULL, "/tmp/fifo-reg", 0644, VNODE_REG, &vn) == -EINVAL);
    CHECK(vfs_mknod(NULL, "/proc/fifo-x", 0644, VNODE_FIFO, &vn) == -EOPNOTSUPP);
    CHECK(vfs_mknod(NULL, FIFO_PATH, 0644, VNODE_FIFO, &vn) == -EEXIST);
    CHECK(fifo_count() == f0);   /* a node with no opens has no ring */

    /* open(O_RDONLY) waits for a writer: the reader thread is still in
     * its open after 30 ms with nobody on the other side, and returns
     * once the writer opens. */
    o = (struct opener){ .path = FIFO_PATH, .flags = O_RD };
    t = start_opener(&o);
    CHECK(t != NULL);
    CHECK(!flag_within(&o.done, 30));
    CHECK(fifo_count() == f0 + 1);   /* the reader's open made the ring */
    CHECK(open_at(FIFO_PATH, O_WR, &wf) == 0);
    CHECK(flag_within(&o.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(o.rc == 0 && o.f != NULL);
    rf = o.f;
    o.f = NULL;   /* rf owns it now */
    /* Bytes, the file's readiness on the way, and end of file after the
     * last writer's close -- after the bytes it wrote. */
    CHECK(kobject_poll_wq(&rf->obj, COSMO_IO_READABLE) != NULL);
    CHECK((kobject_ready(&rf->obj) & (COSMO_IO_READABLE | COSMO_IO_HANGUP)) == 0);
    CHECK(kobject_ready(&wf->obj) == COSMO_IO_WRITABLE);
    CHECK(file_write(wf, "hello", 5) == 5);
    CHECK(kobject_ready(&rf->obj) == COSMO_IO_READABLE);
    CHECK(file_read(rf, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(file_write(wf, "bye", 3) == 3);
    put(&wf);
    CHECK(kobject_ready(&rf->obj) == (COSMO_IO_READABLE | COSMO_IO_HANGUP));
    CHECK(file_read(rf, buf, sizeof(buf)) == 3 && memcmp(buf, "bye", 3) == 0);
    CHECK(file_read(rf, buf, sizeof(buf)) == 0);
    CHECK(file_seek(rf, 0, COSMO_SEEK_SET) == -ESPIPE);
    put(&rf);
    CHECK(fifo_count() == f0);   /* the last release freed the ring */

    /* open(O_WRONLY) waits for a reader, the other order; the last
     * reader's close gives -EPIPE, and the writer sees it in readiness. */
    o = (struct opener){ .path = FIFO_PATH, .flags = O_WR };
    t = start_opener(&o);
    CHECK(t != NULL);
    CHECK(!flag_within(&o.done, 30));
    CHECK(open_at(FIFO_PATH, O_RD, &rf) == 0);
    CHECK(flag_within(&o.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(o.rc == 0 && o.f != NULL);
    wf = o.f;
    o.f = NULL;
    CHECK(file_write(wf, "x", 1) == 1);
    put(&rf);
    CHECK(kobject_ready(&wf->obj) == (COSMO_IO_WRITABLE | COSMO_IO_ERROR));
    CHECK(file_write(wf, "y", 1) == -EPIPE);
    put(&wf);
    CHECK(fifo_count() == f0);

    /* O_NONBLOCK: a write-only open with no reader is -ENXIO and leaves
     * no ring; a read-only one returns at once and reads end of file
     * until a writer arrives, then -EAGAIN while it is empty. */
    CHECK(open_at(FIFO_PATH, O_WR | O_NB, &wf) == -ENXIO);
    CHECK(fifo_count() == f0);
    CHECK(open_at(FIFO_PATH, O_RD | O_NB, &rf) == 0);
    CHECK(file_read(rf, buf, sizeof(buf)) == 0);
    CHECK(kobject_ready(&rf->obj) == (COSMO_IO_READABLE | COSMO_IO_HANGUP));
    CHECK(open_at(FIFO_PATH, O_WR | O_NB, &wf) == 0);
    CHECK(kobject_ready(&rf->obj) == 0);
    CHECK(file_read(rf, buf, sizeof(buf)) == -EAGAIN);
    CHECK(file_write(wf, "nb", 2) == 2);
    CHECK(file_read(rf, buf, sizeof(buf)) == 2 && memcmp(buf, "nb", 2) == 0);

    /* Non-blocking mode is per open: a second reader opened blocking
     * stays so when the first is switched, and each answers for itself. */
    CHECK(open_at(FIFO_PATH, O_RD, &rf2) == 0);
    CHECK(kobject_set_nonblock(&rf->obj, -1) == 1 && kobject_set_nonblock(&rf2->obj, -1) == 0);
    CHECK(kobject_set_nonblock(&rf2->obj, 1) == 0);
    CHECK(kobject_set_nonblock(&rf->obj, 0) == 1);
    CHECK(kobject_set_nonblock(&rf->obj, -1) == 0 && kobject_set_nonblock(&rf2->obj, -1) == 1);
    CHECK(file_read(rf2, buf, sizeof(buf)) == -EAGAIN);
    CHECK(kobject_set_nonblock(&rf->obj, 1) == 0);

    /* Two readers and two writers: the counts follow each close. One
     * writer's close is not end of file; the second's is, for both
     * readers; a new writer finds the remaining reader; the last
     * reader's close is -EPIPE for it. */
    CHECK(open_at(FIFO_PATH, O_WR | O_NB, &wf2) == 0);
    put(&wf);
    CHECK(file_read(rf, buf, sizeof(buf)) == -EAGAIN);
    put(&wf2);
    CHECK(file_read(rf, buf, sizeof(buf)) == 0);
    CHECK(file_read(rf2, buf, sizeof(buf)) == 0);
    CHECK(kobject_ready(&rf2->obj) == (COSMO_IO_READABLE | COSMO_IO_HANGUP));
    put(&rf);
    CHECK(open_at(FIFO_PATH, O_WR | O_NB, &wf) == 0);
    put(&rf2);
    CHECK(file_write(wf, "z", 1) == -EPIPE);
    put(&wf);
    CHECK(fifo_count() == f0);

    /* O_RDWR counts as both sides and never blocks (from a thread with a
     * bounded wait, so an open that did block fails a check instead of
     * hanging the boot): it reads what it wrote and never end of file. */
    o = (struct opener){ .path = FIFO_PATH, .flags = O_RW };
    t = start_opener(&o);
    CHECK(t != NULL);
    CHECK(flag_within(&o.done, 2000));
    thread_join(t);
    t = NULL;
    CHECK(o.rc == 0 && o.f != NULL);
    xf = o.f;
    o.f = NULL;
    CHECK(file_write(xf, "rw", 2) == 2);
    CHECK(kobject_ready(&xf->obj) == (COSMO_IO_READABLE | COSMO_IO_WRITABLE));
    CHECK(file_read(xf, buf, sizeof(buf)) == 2 && memcmp(buf, "rw", 2) == 0);
    CHECK(kobject_ready(&xf->obj) == COSMO_IO_WRITABLE);
    put(&xf);
    CHECK(fifo_count() == f0);

    /* Unlinked while open: the opens keep their ring, a new open finds
     * no name, the last release frees the ring and the node goes with it. */
    CHECK(open_at(FIFO_PATH, O_RD | O_NB, &rf) == 0);
    CHECK(open_at(FIFO_PATH, O_WR | O_NB, &wf) == 0);
    CHECK(vfs_unlink(NULL, FIFO_PATH) == 0);
    CHECK(file_write(wf, "u", 1) == 1);
    CHECK(file_read(rf, buf, sizeof(buf)) == 1 && buf[0] == 'u');
    CHECK(open_at(FIFO_PATH, O_RD | O_NB, &rf2) == -ENOENT);
    put(&wf);
    put(&rf);
    CHECK(fifo_count() == f0);

    /* A blocked opener killed leaves nothing behind. A process (a kernel
     * thread has nobody to kill it) opens read-only and waits for a
     * writer; killed there, its open returns -EINTR and undoes itself:
     * the ring is gone, a non-blocking writer finds no reader, and a
     * blocking writer waits for a real one. */
    const void *image;
    size_t image_size;
    if (bootarchive_find("init", &image, &image_size)) {
        CHECK(vfs_mknod(NULL, KILL_PATH, 0644, VNODE_FIFO, &vn) == 0);
        vnode_put(vn);
        vn = NULL;
        const char *argv[] = { "init", "--probe", "fifo-block-read:" KILL_PATH, NULL };
        CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
        CHECK(count_within(f0 + 1, 5000));   /* the child's open made the ring and is waiting */
        thread_sleep_ms(20);
        CHECK(!completion_done(&p->exited));
        process_kill(p, COSMO_SIGKILL);
        int status = process_wait_exit(p);
        CHECK(status == 128 + COSMO_SIGKILL);
        process_put(p);
        p = NULL;
        CHECK(fifo_count() == f0);
        CHECK(open_at(KILL_PATH, O_WR | O_NB, &wf) == -ENXIO);
        o = (struct opener){ .path = KILL_PATH, .flags = O_WR };
        t = start_opener(&o);
        CHECK(t != NULL);
        CHECK(!flag_within(&o.done, 30));
        CHECK(open_at(KILL_PATH, O_RD, &rf) == 0);
        CHECK(flag_within(&o.done, 2000));
        thread_join(t);
        t = NULL;
        CHECK(o.rc == 0 && o.f != NULL);
        wf = o.f;
        o.f = NULL;
        put(&wf);
        put(&rf);
        CHECK(vfs_unlink(NULL, KILL_PATH) == 0);
        CHECK(fifo_count() == f0);
    }

    pipe_get_stats(&s1);
    CHECK(s1.alive == s0.alive);

out:
    if (t != NULL)
        thread_join(t);
    if (o.f != NULL)
        file_put(o.f);   /* an opener's file the test never adopted */
    put(&rf);
    put(&wf);
    put(&rf2);
    put(&wf2);
    put(&xf);
    if (p != NULL) {
        process_kill(p, COSMO_SIGKILL);
        process_wait_exit(p);
        process_put(p);
    }
    if (vn != NULL)
        vnode_put(vn);
    (void)vfs_unlink(NULL, FIFO_PATH);
    (void)vfs_unlink(NULL, KILL_PATH);
    return ok;
}
