/*
 * test_memdev.c - /dev/null and /dev/zero (kernel-services/vfs/memdev.c) on the host: roadmap 2.0 SA7. A digest
 * recorded from the C over memdev_init, its ramfs_mkchr calls failing now and then, and its warnings, and over random
 * reads and writes of both devices: each result, whether the device fills reads, and every byte of the buffer and of
 * the guards around it, so a read that touches a byte it should not is in the digest too.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/vfs.h>

#include <stdio.h>

void memdev_init(void);

/* The digest of the C (memdev.c, 2026-11-07), which the CeeSafe port gives. */
#define MEMDEV_DIGEST 0xb166a187062d2cf0ull
#define ROUNDS 4000u
#define SEED 0x5EED0DE7202611A7ull
#define GUARD 16u
#define MAXLEN 300u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

/* The devices memdev_init made, by name. */
static const struct chrdev_ops *g_null, *g_zero;
static unsigned g_made, g_warned;

int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)
{
    size_t n = 0;
    while (path[n])
        n++;
    note(1, n);
    mix(path, n);
    note(2, mode);
    note(3, ops != NULL);
    note(4, priv != NULL);
    note(5, out != NULL);
    g_made++;
    unsigned f = pick(5);
    if (f == 0)
        return -EEXIST;
    if (f == 1)
        return -ENOMEM;
    if (n == 9 && path[5] == 'n')
        g_null = ops;
    else if (n == 9 && path[5] == 'z')
        g_zero = ops;
    return 0;
}

static void on_klog(int level, const char *line)
{
    size_t n = 0;
    while (line[n])
        n++;
    note(6, (uint64_t)level);
    note(7, n);
    mix(line, n);
    g_warned++;
}

/* The fields a device's ops set and leave unset: the same for every device. */
static void note_ops(uint32_t tag, const struct chrdev_ops *o)
{
    note(tag, o->read != NULL);
    note(tag, o->write != NULL);
    note(tag, o->open != NULL);
    note(tag, o->release != NULL);
    note(tag, o->open_priv);
    note(tag, o->read_file != NULL);
    note(tag, o->write_file != NULL);
    note(tag, o->fills);
    note(tag, o->ready != NULL);
    note(tag, o->poll_wq != NULL);
    note(tag, o->set_nonblock != NULL);
    note(tag, o->ioctl != NULL);
}

static void test_devices(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    int min = harness_klog_min;
    harness_klog_min = KLOG_ERROR + 1;

    static uint8_t buf[GUARD + MAXLEN + GUARD];
    struct vnode *vn = (struct vnode *)(uintptr_t)0x1000;
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (round % 50 == 0) {
            g_null = g_zero = NULL;
            memdev_init();
            note(8, g_null != NULL);
            note(9, g_zero != NULL);
            if (g_null)
                note_ops(10, g_null);
            if (g_zero)
                note_ops(11, g_zero);
        }
        const struct chrdev_ops *o = pick(2) ? g_null : g_zero;
        if (o == NULL)
            continue;
        for (size_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)next();
        uint64_t off = next();       /* each draw its own statement: the order is the same under every compiler */
        off >>= pick(64);
        size_t len = pick(MAXLEN + 1);
        if (pick(4) == 0)
            len = 0;
        int64_t r;
        if (pick(2)) {
            r = o->read(vn, off, buf + GUARD, len);
            note(12, (uint64_t)r);
        } else {
            r = o->write(vn, off, buf + GUARD, len);
            note(13, (uint64_t)r);
        }
        note(14, o == g_zero);
        mix(buf, sizeof(buf));
    }
    note(15, g_made);
    note(16, g_warned);

    harness_klog_min = min;
    harness_klog_hook = NULL;
    printf("memdev digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_made == 2 * (ROUNDS / 50));
    EXPECT(g_warned > 0);
    EXPECT(g_digest == MEMDEV_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "memdev_devices", test_devices },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
