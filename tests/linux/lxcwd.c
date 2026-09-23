/*
 * lxcwd.c - the held-walk racer, Linux ABI (docs/audit/next-subsystem-cwd-hold.md).
 *
 * Two threads and one pass, named by argv[1]. The kernel test arms the
 * held-walk seam for the process name "lxcwd" before spawning this (via
 * `init --probe cwd-hold-linux:<pass>`, because a kernel-created process
 * is always native), so the seam, not this program, orders the race:
 *
 *   capture  A opens "f" (relative: the walk the seam holds, with the
 *            pointer to d1 in hand); B chdirs to d2, where "f" does not
 *            exist. A's open must SUCCEED: the walk resolved against the
 *            directory it captured.
 *   outlive  A opens "f"; B unlinks d1/f and rmdirs d1 BY ABSOLUTE PATH
 *            (a relative unlink from B would be a relative walk and the
 *            seam would hold B, the releaser), then chdirs to d2. A's
 *            open must fail ENOENT, and the process must live: the walk's
 *            reference outlived the swap. With the reference removed at
 *            the Linux door's open, the kernel panics by name instead.
 *
 * Every path this program uses outside A's one open is absolute, so the
 * seam holds exactly the walk it is for. Exit codes are the checks below.
 */

#include "lxabi.h"

#define THREAD_FLAGS                                                                                        \
    (LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD | LX_CLONE_SYSVSEM |  \
     LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID | LX_CLONE_CHILD_SETTID)

#define ROOT "/tmp/lxcwd"
#define D1 ROOT "/d1"
#define D2 ROOT "/d2"
#define F1 D1 "/f"

static char g_stacks[2][16384] __attribute__((aligned(16)));
static uint64_t g_tcb[2][4];
static int32_t g_tidword[2];
static volatile long g_open_rc;
static volatile int g_outlive;

/* Join like a libc: wait while the CHILD_CLEARTID word is nonzero. */
static int lx_join(int32_t *word)
{
    for (int i = 0; i < 400; i++) {
        int32_t v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v == 0)
            return 0;
        struct lx_timespec to = { 0, 50000000 };
        sc6(LX_futex, word, LX_FUTEX_WAIT, v, &to, 0, 0);
    }
    return -1;
}

/* Thread A: the one relative walk in the program. */
static int t_open(void *arg)
{
    (void)arg;
    long fd = sc4(LX_openat, LX_AT_FDCWD, "f", LX_O_RDONLY, 0);
    g_open_rc = fd;
    if (fd >= 0)
        sc1(LX_close, fd);
    return 0;
}

/* Thread B: the swapper. Absolute paths only. */
static int t_swap(void *arg)
{
    (void)arg;
    if (g_outlive) {
        if (sc3(LX_unlinkat, LX_AT_FDCWD, F1, 0) != 0)
            return 21;
        if (sc3(LX_unlinkat, LX_AT_FDCWD, D1, LX_AT_REMOVEDIR) != 0)
            return 22;
    }
    if (sc1(LX_chdir, D2) != 0)
        return 23;
    return 0;
}

static int setup(void)
{
    (void)sc3(LX_mkdirat, LX_AT_FDCWD, ROOT, 0755);
    (void)sc3(LX_mkdirat, LX_AT_FDCWD, D1, 0755);
    (void)sc3(LX_mkdirat, LX_AT_FDCWD, D2, 0755);
    long fd = sc4(LX_openat, LX_AT_FDCWD, F1, LX_O_WRONLY | LX_O_CREAT, 0644);
    if (fd < 0)
        return 10;
    sc1(LX_close, fd);
    if (sc1(LX_chdir, D1) != 0)
        return 11;
    return 0;
}

static void cleanup(void)
{
    (void)sc1(LX_chdir, "/");
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, F1, 0);
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, D1, LX_AT_REMOVEDIR);
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, D2, LX_AT_REMOVEDIR);
    (void)sc3(LX_unlinkat, LX_AT_FDCWD, ROOT, LX_AT_REMOVEDIR);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *pass = argv[1];
    if (pass[0] == 'o')
        g_outlive = 1;
    else if (pass[0] != 'c')
        return 2;
    int rc = setup();
    if (rc)
        return rc;
    g_open_rc = -1000;
    int32_t ptid;
    if (lx_clone(t_open, g_stacks[0] + sizeof(g_stacks[0]), 0, THREAD_FLAGS, &ptid, &g_tidword[0], g_tcb[0]) <= 0) {
        cleanup();
        return 12;
    }
    if (lx_clone(t_swap, g_stacks[1] + sizeof(g_stacks[1]), 0, THREAD_FLAGS, &ptid, &g_tidword[1], g_tcb[1]) <= 0) {
        cleanup();
        return 13;
    }
    int ja = lx_join(&g_tidword[0]);
    int jb = lx_join(&g_tidword[1]);
    long open_rc = g_open_rc;
    cleanup();
    if (ja != 0 || jb != 0)
        return 14;
    if (g_outlive) {
        if (open_rc != -2) {   /* -ENOENT: the directory is dead, and the process is alive to say so */
            lx_puts("lxcwd: outlive: open did not fail ENOENT\n");
            return 15;
        }
        lx_puts("LXCWD: outlive ok\n");
        return 0;
    }
    if (open_rc < 0) {
        lx_puts("lxcwd: capture: open failed\n");
        return 16;
    }
    lx_puts("LXCWD: capture ok\n");
    return 0;
}
