/*
 * cwdtest - the current directory, from more than one thread
 * (docs/audit/next-subsystem-cwd-ref.md).
 *
 * Every relative-path system call resolves against the process's current
 * directory. Until this unit they all read `p->cwd` with no lock and no
 * reference and handed the raw pointer to a path walk, while another
 * thread's `chdir` could swap it and drop the last reference -- freeing a
 * vnode a walk was inside. This program is the race that finds that.
 *
 * **Every move here is relative**, and that is a requirement rather than
 * a style. An absolute `chdir` bypasses the normalisation base entirely,
 * so a torn `cwd_path` is never read and `getcwd` already copies under
 * the lock: written with absolute paths, step 2 passes against the
 * unfixed kernel and proves nothing. The layout exists to make relative
 * moves always valid:
 *
 *     /tmp/cwdr/a   with a file `is-a`
 *     /tmp/cwdr/b   with a file `is-b`
 *
 * From either directory, `../a` and `../b` are both real, so no move can
 * fail for an ordinary reason and hide the race behind an -ENOENT.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cosmo/syscall.h>
#include <cosmo/thread.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("cwdtest: FAIL %s at line %d\n", #cond, __LINE__);        \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define STEP(n)                                                              \
    do {                                                                     \
        printf("cwdtest: step %s\n", (n));                                   \
        fflush(stdout);                                                      \
    } while (0)

#define ROUNDS 4000u

/*
 * The names are long, different from each other in every byte, and of
 * **different lengths** -- all three on purpose.
 *
 * A first version used `/tmp/cwdr/a` and `/tmp/cwdr/b`, which cannot
 * detect a torn `cwd_path` at all: the two strings differ in exactly one
 * byte, so any mixture of them is still one of the two valid answers, and
 * a single-byte store is atomic anyway. The proof for the torn-buffer
 * defect passed against the unfixed kernel because of it.
 *
 * With these, a copy interrupted part-way leaves the head of one name and
 * the tail of the other -- a string that is neither, and visibly so.
 */
#define NAME_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NAME_B "bbbb"
static const char *DIR_A = "/tmp/cwdr/" NAME_A;
static const char *DIR_B = "/tmp/cwdr/" NAME_B;

/*
 * The writers and the observer meet here. `parked` lets the observer stop
 * the writers before it looks at the process's directory -- see step 3 for
 * why a check that runs while they are moving cannot be trusted.
 */
static cosmo_mutex_t m = COSMO_MUTEX_INIT;
static cosmo_cond_t c = COSMO_COND_INIT;
static volatile unsigned quiesce;      /* writers: park at the top of the loop */
static volatile unsigned parked;       /* how many are parked */
static volatile unsigned writers_stop;
static volatile unsigned bad_errno;    /* an open failed for something other than -ENOENT */
static volatile unsigned bad_path;     /* getcwd answered neither directory */
static volatile unsigned opens_ok, opens_enoent;

/*
 * The only way this program ends a step. It must be **under the mutex and
 * with a broadcast**: an observer asleep in `quiesce_writers` is waiting
 * on `c`, and a bare store to `writers_stop` leaves it asleep for ever.
 * Two writers set the flag bare before review found it, and the failure is
 * a hung boot rather than a failed check.
 */
static void publish_stop(void)
{
    cosmo_mutex_lock(&m);
    writers_stop = 1;
    quiesce = 0;                 /* so nobody parks on the way out */
    cosmo_cond_broadcast(&c);
    cosmo_mutex_unlock(&m);
}

static void writer_checkpoint(void)
{
    cosmo_mutex_lock(&m);
    while (quiesce) {
        parked++;
        cosmo_cond_broadcast(&c);
        while (quiesce)
            cosmo_cond_wait(&c, &m);
        parked--;
        cosmo_cond_broadcast(&c);
    }
    cosmo_mutex_unlock(&m);
}

/* Alternate between the two directories, relatively. */
static void *chdir_writer(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        writer_checkpoint();
        if (chdir("../" NAME_A) != 0 && chdir("../" NAME_B) != 0)
            continue;    /* a move may lose a race with the other writer; both are valid */
        writer_checkpoint();
        (void)chdir("../" NAME_B);
    }
    publish_stop();
    return NULL;
}

/*
 * Open a relative path while the directory moves under it. The file exists
 * in one directory and not the other, so **either answer is correct** --
 * what must never happen is any *other* errno, and what must never happen
 * at all is a fault, which is the bug this program is named for and which
 * the boot test sees as a dead process rather than as a failed check.
 */
static void *open_reader(void *arg)
{
    (void)arg;
    while (!writers_stop) {
        int fd = open("is-a", O_RDONLY);
        if (fd >= 0) {
            opens_ok++;
            close(fd);
        } else if (errno == ENOENT) {
            opens_enoent++;
        } else {
            bad_errno = (unsigned)errno;
            break;
        }
    }
    return NULL;
}

/* --- step 2: a move that keeps the base -----------------------------------
 *
 * `../X` cannot show a torn `cwd_path`, and finding that out took a run:
 * `..` discards the last component, which is the *only* part where two
 * sibling names differ. Whatever mixture of them the reader saw, the
 * result was the same either way.
 *
 * So step 2 moves **down** instead. A plain `chdir("s")` keeps the whole
 * base and appends to it, so every differing byte survives into the
 * answer. The two bases are at different depths and share nothing after
 * `/tmp/cwdr/`:
 *
 *     /tmp/cwdr/pppppppppppppppppppppppppppppppp/s
 *     /tmp/cwdr/q/s
 *
 * Each writer walks its own pair down and up. A reader that catches the
 * buffer mid-copy normalises against the head of one and the tail of the
 * other, and `getcwd` then answers a path that is neither. */
#define NAME_P "pppppppppppppppppppppppppppppppp"
#define NAME_Q "q"
#define P_TOP "/tmp/cwdr/" NAME_P
#define P_SUB "/tmp/cwdr/" NAME_P "/s"
#define Q_TOP "/tmp/cwdr/" NAME_Q
#define Q_SUB "/tmp/cwdr/" NAME_Q "/s"

static void *depth_writer(void *arg)
{
    /* 0 or 1 rather than the string itself: a string literal is `const
     * char *`, and casting the const away to fit `void *` is a -Werror
     * here and a lie anywhere. */
    const char *top = (unsigned long)arg ? Q_TOP : P_TOP;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        /* Step 3 stops the writers before it compares; without this they
         * never park and the quiesce waits forever. Step 2 never
         * quiesces, so it costs nothing there. */
        writer_checkpoint();
        if (chdir(top) != 0)
            continue;
        (void)chdir("s");     /* down: keeps the whole base, so a tear shows */
        writer_checkpoint();
        (void)chdir("..");
    }
    publish_stop();
    return NULL;
}

/* Every answer must be one of the four real paths. A torn base produces a
 * fifth. */
/*
 * Every path two relative writers can legitimately produce.
 *
 * **The set is larger than the directories they aim at**, and a first
 * version of this test failed on a correct kernel for not knowing that.
 * Two threads sharing one current directory each issue `chdir("..")`
 * believing they know where they are; when the other has already moved,
 * the `..` applies to *its* directory instead, and the pair can walk the
 * process up out of the subtree altogether -- `/tmp` and `/` included.
 * That is what relative moves from two threads mean, not a defect.
 *
 * A torn `cwd_path` still cannot produce any of these: the two bases
 * share only `/tmp/cwdr/`, so a mixture of them has one name's head and
 * the other's tail and is in none of the sets below.
 */
static int path_is_legitimate(const char *p)
{
    static const char *ok[] = { "/", "/tmp", "/tmp/cwdr",
                                P_TOP, P_SUB, Q_TOP, Q_SUB,
                                "/tmp/cwdr/" NAME_A, "/tmp/cwdr/" NAME_B };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (strcmp(p, ok[i]) == 0)
            return 1;
    return 0;
}

static void *depth_observer(void *arg)
{
    (void)arg;
    char buf[256];
    while (!writers_stop) {
        if (getcwd(buf, sizeof(buf)) == NULL) {
            bad_path++;
            break;
        }
        if (!path_is_legitimate(buf)) {
            printf("cwdtest: getcwd answered '%s'\n", buf);
            bad_path++;
            break;
        }
    }
    return NULL;
}

/*
 * Start `n` threads, and if any start fails, stop and join the ones that
 * did. Without this a partial start hangs the boot suite rather than
 * failing it: the survivors park, `parked` never reaches `n`, and nobody
 * is left to set `writers_stop`. Stack allocation can fail in this
 * environment, so a refused start is a case and not a hypothetical.
 */
static int start_all(cosmo_thread_t *t, unsigned n, void *(*fn)(void *), int pass_index)
{
    unsigned made = 0;
    for (; made < n; made++) {
        void *arg = pass_index ? (void *)(unsigned long)made : NULL;
        if (cosmo_thread_start(&t[made], fn, arg, 32u * 1024u) != 0)
            break;
    }
    if (made == n)
        return 0;
    printf("cwdtest: only %u of %u threads started\n", made, n);
    publish_stop();
    for (unsigned i = 0; i < made; i++)
        (void)cosmo_thread_join(&t[i], NULL);
    return -1;
}

/* Stop the writers and wait until every one of them is parked. */
static void quiesce_writers(unsigned n)
{
    cosmo_mutex_lock(&m);
    quiesce = 1;
    cosmo_cond_broadcast(&c);
    while (parked < n && !writers_stop)
        cosmo_cond_wait(&c, &m);
    cosmo_mutex_unlock(&m);
}

static void release_writers(void)
{
    cosmo_mutex_lock(&m);
    quiesce = 0;
    cosmo_cond_broadcast(&c);
    cosmo_mutex_unlock(&m);
}

/* --- step 4: the victim directory ---------------------------------------- */
#define VICTIM "vic"
static volatile unsigned vic_walks, vic_frees_seen;

/* Move the process in and out of the victim. The `chdir("..")` is the put
 * that can be the last one, once the killer has removed the entry. */
static void *vic_mover(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        if (chdir(VICTIM) == 0)
            (void)chdir("..");
    }
    publish_stop();
    return NULL;
}

/* Remove the entry, which drops ramfs's pinned reference, and put it back
 * so the mover has something to enter next time round. */
static void *vic_killer(void *arg)
{
    (void)arg;
    while (!writers_stop) {
        (void)rmdir("/tmp/cwdr/" VICTIM);
        (void)mkdir("/tmp/cwdr/" VICTIM, 0755);
    }
    return NULL;
}

/* Walk a relative path. When the process is inside the victim this
 * dereferences the victim's vnode -- with a reference of its own since
 * this unit, and with a bare pointer before it. */
static void *vic_walker(void *arg)
{
    (void)arg;
    char buf[256];
    while (!writers_stop) {
        int fd = open("f", O_RDONLY);
        if (fd >= 0) {
            close(fd);
        } else if (errno != ENOENT && errno != ENOTDIR) {
            bad_errno = (unsigned)errno;
            break;
        }
        if (getcwd(buf, sizeof(buf)) != NULL && strstr(buf, VICTIM) != NULL)
            vic_walks++;
    }
    return NULL;
}

static int make_layout(void)
{
    if (mkdir("/tmp/cwdr", 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(DIR_A, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(DIR_B, 0755) != 0 && errno != EEXIST)
        return -1;
    int fd = open("/tmp/cwdr/" NAME_A "/is-a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    fd = open("/tmp/cwdr/" NAME_B "/is-b", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    /* Step 2's and step 3's pair: different depths, no shared bytes after
     * the root, and a marker in each leaf so that a path and a vnode
     * naming different leaves can be told apart. */
    if (mkdir(P_TOP, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(P_SUB, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(Q_TOP, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(Q_SUB, 0755) != 0 && errno != EEXIST)
        return -1;
    fd = open(P_SUB "/mark-p", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    fd = open(Q_SUB "/mark-q", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

/* --- --held: the held-walk racer (docs/audit/next-subsystem-cwd-hold.md) --
 *
 * Two threads and one pass. The kernel test arms the seam for the process
 * name "cwdtest" before spawning this, so the seam, not this program,
 * orders the race: A's relative open is held with its pointer in hand,
 * B's chdir waits for that hold before it publishes, and A resumes only
 * once B has put the old directory.
 *
 *   capture  "f" exists only in d1; B moves the process to d2. A's open
 *            must succeed: the walk resolved against the directory it
 *            captured, not the one the process has now.
 *   outlive  B unlinks d1/f and rmdirs d1 -- BY ABSOLUTE PATH, because a
 *            relative unlink is a relative walk and the seam would hold B,
 *            the releaser -- then moves away. A's open must fail ENOENT
 *            and this process must be alive to say so. With the reference
 *            removed at open, the kernel panics by name instead.
 *
 * Everything but A's one open is absolute, so the seam holds exactly the
 * walk it is for. The four steps in main() are untouched by this.
 */
#define H_ROOT "/tmp/cwdh"
#define H_D1 H_ROOT "/d1"
#define H_D2 H_ROOT "/d2"
#define H_F1 H_D1 "/f"

static volatile int h_open_fd, h_open_errno, h_swap_rc;
static int h_outlive, h_swapfirst, h_failswap;

/* debug.cwd_hold: 0 idle, 1 armed, 2 held, 3 released, 4 the swapper is waiting. */
static long held_state(void)
{
    char buf[16];
    long n = cosmo_sysctl("debug.cwd_hold", buf, sizeof(buf) - 1);
    if (n < 0)
        return -1;
    buf[n < (long)sizeof(buf) - 1 ? n : (long)sizeof(buf) - 1] = 0;
    return atol(buf);
}

static void *held_opener(void *arg)
{
    (void)arg;
    int fd = open("f", O_RDONLY);   /* the one relative walk: the held one */
    h_open_fd = fd;
    h_open_errno = fd < 0 ? errno : 0;
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void *held_swapper(void *arg)
{
    (void)arg;
    if (h_failswap) {
        /* A chdir that fails after registering as the swapper: the held
         * walk must be released by the failure, not by its bound. */
        h_swap_rc = (chdir(H_ROOT "/absent") != 0 && errno == ENOENT) ? 0 : 24;
        return NULL;
    }
    if (h_outlive) {
        if (unlink(H_F1) != 0) { h_swap_rc = 21; return NULL; }
        if (rmdir(H_D1) != 0)  { h_swap_rc = 22; return NULL; }
    }
    if (chdir(H_D2) != 0)      { h_swap_rc = 23; return NULL; }
    h_swap_rc = 0;
    return NULL;
}

static void held_cleanup(void)
{
    (void)chdir("/");
    (void)unlink(H_F1);
    (void)rmdir(H_D1);
    (void)rmdir(H_D2);
    (void)rmdir(H_ROOT);
}

static int held_main(const char *pass)
{
    h_outlive = pass[0] == 'o';
    h_swapfirst = pass[0] == 's';
    h_failswap = pass[0] == 'f';
    if (!h_outlive && !h_swapfirst && !h_failswap && pass[0] != 'c')
        return 2;
    (void)mkdir(H_ROOT, 0755);
    (void)mkdir(H_D1, 0755);
    (void)mkdir(H_D2, 0755);
    int fd = open(H_F1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 10;
    close(fd);
    if (chdir(H_D1) != 0)
        return 11;
    h_open_fd = -1000;
    h_swap_rc = -1;
    cosmo_thread_t a, b;
    if (h_swapfirst) {
        /*
         * The swapper first, and the walker only once the seam says the
         * swapper is already waiting inside chdir. This is the order
         * under which a chdir that published BEFORE waiting would
         * install d2 under a walk that has yet to capture d1 -- the
         * design error review found in the report -- and the order the
         * other two passes cannot produce, because A's open reaches the
         * seam first by construction. The wait is on the seam's own
         * state, never on time.
         */
        if (cosmo_thread_start(&b, held_swapper, NULL, 32u * 1024u) != 0) {
            held_cleanup();
            return 13;
        }
        for (unsigned spins = 0; held_state() != 4; spins++) {
            if (spins > 200000) {
                (void)cosmo_thread_join(&b, NULL);
                held_cleanup();
                printf("cwdtest: --held swapfirst: the swapper never reached its wait (state %ld)\n", held_state());
                return 17;
            }
            cosmo_yield();
        }
        if (cosmo_thread_start(&a, held_opener, NULL, 32u * 1024u) != 0) {
            (void)cosmo_thread_join(&b, NULL);
            held_cleanup();
            return 12;
        }
    } else if (h_failswap) {
        /* The walker first and held; then a swapper whose chdir fails
         * (the dirfd unit's review: a failed chdir left the seam's
         * swapper registered and the held walk waiting out its bound). */
        if (cosmo_thread_start(&a, held_opener, NULL, 32u * 1024u) != 0) {
            held_cleanup();
            return 12;
        }
        for (unsigned spins = 0; held_state() != 2; spins++) {
            if (spins > 200000) {
                (void)cosmo_thread_join(&a, NULL);
                held_cleanup();
                return 18;
            }
            cosmo_yield();
        }
        if (cosmo_thread_start(&b, held_swapper, NULL, 32u * 1024u) != 0) {
            (void)cosmo_thread_join(&a, NULL);
            held_cleanup();
            return 13;
        }
    } else {
        if (cosmo_thread_start(&a, held_opener, NULL, 32u * 1024u) != 0) {
            held_cleanup();
            return 12;
        }
        if (cosmo_thread_start(&b, held_swapper, NULL, 32u * 1024u) != 0) {
            (void)cosmo_thread_join(&a, NULL);
            held_cleanup();
            return 13;
        }
    }
    (void)cosmo_thread_join(&a, NULL);
    (void)cosmo_thread_join(&b, NULL);
    int ofd = h_open_fd, oerr = h_open_errno, src = h_swap_rc;
    held_cleanup();
    if (src != 0) {
        printf("cwdtest: --held %s: swapper failed %d\n", pass, src);
        return src;
    }
    if (h_outlive) {
        if (ofd >= 0 || oerr != ENOENT) {
            printf("cwdtest: --held outlive: open gave fd %d errno %d, wanted ENOENT\n", ofd, oerr);
            return 15;
        }
        printf("cwdtest: --held outlive ok\n");
        return 0;
    }
    if (ofd < 0) {
        printf("cwdtest: --held %s: open failed, errno %d\n", pass, oerr);
        return 16;
    }
    printf("cwdtest: --held %s ok\n", pass);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--held") == 0) {
        int rc = held_main(argv[2]);
        fflush(stdout);
        return rc;
    }
    printf("cwdtest: start\n");
    fflush(stdout);
    CHECK(make_layout() == 0);
    CHECK(chdir(DIR_A) == 0);

    STEP("1");
    /*
     * (1) **A walk survives the directory moving under it.** One thread
     * alternates between the two directories, another opens a relative
     * path in a loop. Before this unit the opener held a pointer to a
     * vnode the writer could free.
     *
     * The observable is that the process is still alive and every refusal
     * was -ENOENT. A use-after-free here is a fault, which ends the
     * process -- so this step's real verdict is that the ones after it
     * run at all.
     */
    {
        cosmo_thread_t w, r;
        writers_stop = quiesce = parked = bad_errno = 0;
        opens_ok = opens_enoent = 0;
        /*
         * The reader loops until the writer ends the step, so a writer
         * that never starts is a hang rather than a failure. Every step
         * here starts its stop-producing thread first and bails if it is
         * refused -- stack allocation can be, and a hung boot costs the
         * whole suite.
         */
        if (cosmo_thread_start(&w, chdir_writer, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            goto step1_done;
        }
        if (cosmo_thread_start(&r, open_reader, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            publish_stop();
            (void)cosmo_thread_join(&w, NULL);
            goto step1_done;
        }
        CHECK(cosmo_thread_join(&w, NULL) == 0);
        CHECK(cosmo_thread_join(&r, NULL) == 0);
        CHECK(bad_errno == 0);
        /* Both answers must actually have happened, or the threads did not
         * overlap and this step tested one directory in peace. */
        CHECK(opens_ok > 0);
        CHECK(opens_enoent > 0);
        printf("cwdtest: %u opens ok, %u enoent\n", opens_ok, opens_enoent);
step1_done:;
    }

    STEP("2");
    /*
     * (2) **`chdir` racing `chdir`, with a reader of the name.** Two
     * writers moving relatively, one observer calling `getcwd`. The name
     * must always be exactly one of the two directories: `process_chdir`
     * normalises against `cwd_path`, and reading that buffer while another
     * thread is copying into it yields the head of one path and the tail
     * of another.
     */
    {
        cosmo_thread_t w[2], o;
        CHECK(chdir("/tmp/cwdr") == 0);
        writers_stop = quiesce = parked = bad_path = 0;
        if (start_all(w, 2, depth_writer, 1) != 0) {
            CHECK(0);
            goto step2_done;
        }
        if (cosmo_thread_start(&o, depth_observer, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            publish_stop();
            (void)cosmo_thread_join(&w[0], NULL);
            (void)cosmo_thread_join(&w[1], NULL);
            goto step2_done;
        }
        CHECK(cosmo_thread_join(&w[0], NULL) == 0);
        CHECK(cosmo_thread_join(&w[1], NULL) == 0);
        CHECK(cosmo_thread_join(&o, NULL) == 0);
        CHECK(bad_path == 0);
step2_done:;
    }

    STEP("3");
    /*
     * (3) **The name and the directory agree.** `process_chdir` publishes
     * a path and a vnode; if it takes them from two acquisitions of the
     * lock rather than one snapshot, they can come from different
     * directories -- and the process is then left reporting one directory
     * through `getcwd` while resolving relative paths in another.
     *
     * **The writers are stopped first, and that is load-bearing.** The
     * obvious version of this check -- `getcwd`, then open the file that
     * belongs to it -- fails on a *correct* kernel, because a writer may
     * legitimately `chdir` between those two system calls. Quiescing makes
     * the comparison honest, and it works because the defect's damage is
     * **persistent, not transient**: a process left holding one
     * directory's path and another's vnode stays that way until its next
     * `chdir`.
     */
    {
        cosmo_thread_t w[2];
        char buf[256];
        CHECK(chdir("/tmp/cwdr") == 0);
        writers_stop = quiesce = parked = bad_path = 0;
        /*
         * **The writers move *down*, not between siblings**, and that is
         * what makes this step able to fail at all. With sibling moves --
         * which a first version used -- normalising `../NAME_A` from
         * either sibling produces `DIR_A`, and looking `../NAME_A` up from
         * either sibling also produces `DIR_A`. Path and vnode agree no
         * matter which directory each was taken from, so two acquisitions
         * are indistinguishable from one and the proof for this step
         * passed against the defect. Review found it.
         *
         * A down-move keeps the base: normalising `s` against P gives
         * `P/s` while looking `s` up in Q gives `Q/s`, so a pair taken
         * from different directories is a path and a vnode that name
         * different places -- which the marker files below detect.
         */
        if (start_all(w, 2, depth_writer, 1) != 0) {
            CHECK(0);   /* reported by start_all; the step cannot run */
            goto step3_done;
        }
        unsigned checked = 0;
        for (unsigned i = 0; i < 200u && !writers_stop; i++) {
            quiesce_writers(2);
            if (writers_stop)
                break;
            if (getcwd(buf, sizeof(buf)) != NULL) {
                const char *want = strcmp(buf, P_SUB) == 0 ? "mark-p"
                                 : strcmp(buf, Q_SUB) == 0 ? "mark-q" : NULL;
                if (want == NULL) {
                    /* The writers can walk the process out of the pair --
                     * see path_is_legitimate. That is not a defect, and
                     * there is nothing to compare, so this round is
                     * skipped rather than failed. A path that is not even
                     * legitimate still is a defect. */
                    if (!path_is_legitimate(buf)) {
                        printf("cwdtest: getcwd answered '%s'\n", buf);
                        bad_path++;
                    }
                } else {
                    int fd = open(want, O_RDONLY);   /* relative: uses the vnode */
                    if (fd < 0) {
                        printf("cwdtest: getcwd said %s but '%s' is absent (errno %d)\n",
                               buf, want, errno);
                        bad_path++;
                    } else {
                        close(fd);
                        checked++;
                    }
                }
            }
            release_writers();
        }
        publish_stop();
        CHECK(cosmo_thread_join(&w[0], NULL) == 0);
        CHECK(cosmo_thread_join(&w[1], NULL) == 0);
        CHECK(bad_path == 0);
        CHECK(checked > 0);   /* the comparison actually ran */
        printf("cwdtest: %u coherent checks\n", checked);
    }
step3_done:;

    STEP("4");
    /*
     * (4) **The directory is removed while a walk is inside it**, which is
     * what it actually takes to free the vnode here.
     *
     * Steps 1 to 3 cannot free anything, and finding that out is most of
     * what this step is for. `ramfs` -- which backs `/tmp` -- holds a
     * **pinned reference** from a directory entry to its child
     * (`kernel-services/vfs/ramfs.c:25`), so every directory that still
     * exists is referenced by its parent no matter what any `chdir` does.
     * A measurement said so before this step was written: zero directory
     * vnodes were freed during the whole of steps 1 to 3.
     *
     * So the last reference can only be the current directory once the
     * entry is gone. Three threads: one moving the process in and out of
     * `vic`, one removing and recreating `vic`, and one walking a relative
     * path. When `vic` has been removed and the mover leaves it, the put
     * is the last -- and a walker that kept no reference of its own is
     * inside a freed vnode.
     */
    {
        cosmo_thread_t mv, kl, wk;
        CHECK(chdir("/tmp/cwdr") == 0);
        (void)mkdir(VICTIM, 0755);
        writers_stop = quiesce = parked = bad_errno = 0;
        vic_walks = vic_frees_seen = 0;
        /* The mover is the one that ends this step; the other two loop on
         * its flag, so it starts first and the rest bail if refused. */
        if (cosmo_thread_start(&mv, vic_mover, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            goto step4_done;
        }
        if (cosmo_thread_start(&kl, vic_killer, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            publish_stop();
            (void)cosmo_thread_join(&mv, NULL);
            goto step4_done;
        }
        if (cosmo_thread_start(&wk, vic_walker, NULL, 32u * 1024u) != 0) {
            CHECK(0);
            publish_stop();
            (void)cosmo_thread_join(&mv, NULL);
            (void)cosmo_thread_join(&kl, NULL);
            goto step4_done;
        }
        CHECK(cosmo_thread_join(&mv, NULL) == 0);
        CHECK(cosmo_thread_join(&kl, NULL) == 0);
        CHECK(cosmo_thread_join(&wk, NULL) == 0);
        CHECK(bad_errno == 0);
        CHECK(vic_walks > 0);
        printf("cwdtest: %u walks in the victim\n", vic_walks);
step4_done:;
        CHECK(chdir("/tmp/cwdr") == 0);
        (void)mkdir(VICTIM, 0755);
    }

    CHECK(chdir("/") == 0);
    printf("cwdtest: %d failure(s)\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
